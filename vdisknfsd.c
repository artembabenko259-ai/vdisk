/* vdisknfsd -- minimal NFSv3 + MOUNTv3 server backed by real POSIX file I/O.
 * Written for vdisk's "browse the persisted Linux disk live from Windows"
 * feature, after both the kernel's rpc.nfsd (hangs the whole guest) and
 * unfs3's unfsd (segfaults on requests arriving via QEMU hostfwd/NAT) proved
 * unreliable. Single-purpose, no portmapper, fixed ports, one export root
 * (always "/" in practice). Two threads: one serving MOUNT, one serving NFS.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

typedef unsigned int u32;
typedef unsigned long long u64;

#define PROG_MOUNT 100005u
#define PROG_NFS   100003u

#define MOUNTPROC_NULL 0u
#define MOUNTPROC_MNT  1u
#define MOUNTPROC_UMNT 3u

#define NFSPROC_NULL     0u
#define NFSPROC_GETATTR  1u
#define NFSPROC_SETATTR  2u
#define NFSPROC_LOOKUP   3u
#define NFSPROC_ACCESS   4u
#define NFSPROC_READ     6u
#define NFSPROC_WRITE    7u
#define NFSPROC_CREATE   8u
#define NFSPROC_MKDIR    9u
#define NFSPROC_REMOVE   12u
#define NFSPROC_RMDIR    13u
#define NFSPROC_RENAME   14u
#define NFSPROC_READDIR  16u
#define NFSPROC_FSSTAT   18u
#define NFSPROC_FSINFO   19u
#define NFSPROC_PATHCONF 20u
#define NFSPROC_COMMIT   21u

#define NFS3_OK 0u
#define E_NFS3ERR_PERM 1u
#define E_NFS3ERR_NOENT 2u
#define E_NFS3ERR_IO 5u
#define E_NFS3ERR_EXIST 17u
#define E_NFS3ERR_NOTDIR 20u
#define E_NFS3ERR_ISDIR 21u
#define E_NFS3ERR_NOSPC 28u
#define E_NFS3ERR_NOTEMPTY 66u
#define E_NFS3ERR_SERVERFAULT 10006u

#define NF3REG 1u
#define NF3DIR 2u
#define NF3LNK 5u

static char g_root[4096];

/* ---------------- id <-> path table (protected by g_lock) ---------------- */
#define MAX_IDS 65536
static char *g_paths[MAX_IDS];
static int g_id_count = 1; /* id 0 reserved for the export root */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *path_of(u64 id) {
    if (id == 0) return g_root;
    if (id >= (u64)g_id_count) return NULL;
    return g_paths[id];
}

// Returns NFS_ID_INVALID (never a real id, since 0 is the reserved root and
// valid ids are otherwise < MAX_IDS) if the table is full -- callers must
// check for this and fail the RPC instead of handing out a handle, since
// silently returning 0 used to alias every over-capacity file to the export
// root, redirecting later GETATTR/READ/WRITE/REMOVE calls onto "/" instead
// of the file the caller actually meant.
#define NFS_ID_INVALID ((u64)-1)

static u64 id_of_path(const char *path) {
    pthread_mutex_lock(&g_lock);
    if (strcmp(path, g_root) == 0) { pthread_mutex_unlock(&g_lock); return 0; }
    for (int i = 1; i < g_id_count; i++) {
        if (g_paths[i] && strcmp(g_paths[i], path) == 0) { pthread_mutex_unlock(&g_lock); return (u64)i; }
    }
    if (g_id_count >= MAX_IDS) { pthread_mutex_unlock(&g_lock); return NFS_ID_INVALID; }
    int id = g_id_count++;
    g_paths[id] = strdup(path);
    pthread_mutex_unlock(&g_lock);
    return (u64)id;
}

static void join_path(char *out, size_t outcap, const char *dir, const char *name) {
    if (strcmp(dir, "/") == 0) snprintf(out, outcap, "/%s", name);
    else snprintf(out, outcap, "%s/%s", dir, name);
}

static u32 errno_to_nfs(int e) {
    switch (e) {
        case ENOENT: return E_NFS3ERR_NOENT;
        case EACCES: case EPERM: return E_NFS3ERR_PERM;
        case EEXIST: return E_NFS3ERR_EXIST;
        case ENOTDIR: return E_NFS3ERR_NOTDIR;
        case EISDIR: return E_NFS3ERR_ISDIR;
        case ENOSPC: return E_NFS3ERR_NOSPC;
        case ENOTEMPTY: return E_NFS3ERR_NOTEMPTY;
        default: return E_NFS3ERR_IO;
    }
}

/* ------------------------------- XDR ------------------------------------- */
typedef struct { unsigned char *data; size_t len, cap; } xw_t;
static void w_ensure(xw_t *w, size_t extra) {
    if (w->len + extra <= w->cap) return;
    size_t nc = w->cap ? w->cap * 2 : 8192;
    while (nc < w->len + extra) nc *= 2;
    w->data = realloc(w->data, nc);
    w->cap = nc;
}
static void w_u32(xw_t *w, u32 v) {
    w_ensure(w, 4);
    w->data[w->len++] = (unsigned char)(v >> 24);
    w->data[w->len++] = (unsigned char)(v >> 16);
    w->data[w->len++] = (unsigned char)(v >> 8);
    w->data[w->len++] = (unsigned char)v;
}
static void w_u64(xw_t *w, u64 v) { w_u32(w, (u32)(v >> 32)); w_u32(w, (u32)v); }
static void w_bytes(xw_t *w, const void *d, u32 n) {
    w_u32(w, n);
    w_ensure(w, ((size_t)n + 3) & ~(size_t)3);
    memcpy(w->data + w->len, d, n);
    w->len += n;
    u32 pad = (4 - (n & 3)) & 3;
    for (u32 i = 0; i < pad; i++) w->data[w->len++] = 0;
}
static void w_str(xw_t *w, const char *s) { w_bytes(w, s, (u32)strlen(s)); }
static void w_bool(xw_t *w, int v) { w_u32(w, v ? 1 : 0); }
static void w_fh(xw_t *w, u64 id) {
    unsigned char b[8];
    for (int i = 0; i < 8; i++) b[i] = (unsigned char)(id >> (8 * (7 - i)));
    w_bytes(w, b, 8);
}

typedef struct { const unsigned char *data; size_t len, pos; int error; } xr_t;
static u32 r_u32(xr_t *r) {
    if (r->error || r->pos + 4 > r->len) { r->error = 1; return 0; }
    u32 v = ((u32)r->data[r->pos] << 24) | ((u32)r->data[r->pos+1] << 16) |
            ((u32)r->data[r->pos+2] << 8) | r->data[r->pos+3];
    r->pos += 4;
    return v;
}
static u64 r_u64(xr_t *r) { u64 hi = r_u32(r), lo = r_u32(r); return (hi << 32) | lo; }
static u32 r_bytes(xr_t *r, char *out, u32 outcap) {
    u32 n = r_u32(r);
    if (r->error) return 0;
    // 64-bit: n is attacker-controlled and near UINT32_MAX wraps (n+3) back
    // to ~0 in 32-bit arithmetic, which used to make the bounds check below
    // pass trivially and then memcpy up to outcap-1 bytes from wherever
    // r->pos happened to be -- a heap over-read past the real buffer.
    u64 padded = ((u64)n + 3) & ~3ull;
    if ((u64)r->pos + padded > (u64)r->len) { r->error = 1; return 0; }
    u32 copy = n < outcap - 1 ? n : outcap - 1;
    memcpy(out, r->data + r->pos, copy);
    out[copy] = '\0';
    r->pos += (size_t)padded;
    return copy;
}
static void r_skip_auth(xr_t *r) {
    r_u32(r);
    u32 len = r_u32(r);
    u64 padded = ((u64)len + 3) & ~3ull;
    if ((u64)r->pos + padded > (u64)r->len) { r->error = 1; return; }
    r->pos += (size_t)padded;
}
static u64 r_fh(xr_t *r) {
    unsigned char buf[64];
    u32 n = r_bytes(r, (char *)buf, sizeof(buf));
    if (r->error || n != 8) return 0;
    u64 id = 0;
    for (int i = 0; i < 8; i++) id = (id << 8) | buf[i];
    return id;
}

typedef struct { int have_mode; u32 mode; int have_size; u64 size; } sattr_t;
static sattr_t r_sattr3(xr_t *r) {
    sattr_t s; memset(&s, 0, sizeof(s));
    if (r_u32(r)) { s.have_mode = 1; s.mode = r_u32(r); }
    if (r_u32(r)) r_u32(r);
    if (r_u32(r)) r_u32(r);
    if (r_u32(r)) { s.have_size = 1; s.size = r_u64(r); }
    if (r_u32(r) == 2) { r_u32(r); r_u32(r); }
    if (r_u32(r) == 2) { r_u32(r); r_u32(r); }
    return s;
}

/* ----------------------------- attrs -------------------------------------- */
static int fill_attr(const char *path, xw_t *w) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno_to_nfs(errno);
    u32 type = S_ISDIR(st.st_mode) ? NF3DIR : S_ISLNK(st.st_mode) ? NF3LNK : NF3REG;
    w_u32(w, type);
    w_u32(w, st.st_mode & 07777);
    w_u32(w, (u32)st.st_nlink);
    w_u32(w, (u32)st.st_uid);
    w_u32(w, (u32)st.st_gid);
    w_u64(w, (u64)st.st_size);
    w_u64(w, (u64)st.st_blocks * 512);
    w_u32(w, 0); w_u32(w, 0);
    w_u64(w, 1);
    w_u64(w, (u64)st.st_ino);
    w_u32(w, (u32)st.st_atime); w_u32(w, 0);
    w_u32(w, (u32)st.st_mtime); w_u32(w, 0);
    w_u32(w, (u32)st.st_ctime); w_u32(w, 0);
    return 0;
}
static void w_post_op_attr_ok(xw_t *w, const char *path) {
    xw_t tmp; memset(&tmp, 0, sizeof(tmp));
    int rc = fill_attr(path, &tmp);
    if (rc == 0) { w_bool(w, 1); w_ensure(w, tmp.len); memcpy(w->data + w->len, tmp.data, tmp.len); w->len += tmp.len; }
    else w_bool(w, 0);
    free(tmp.data);
}
static void w_wcc_data_absent(xw_t *w) { w_bool(w, 0); w_bool(w, 0); }

/* --------------------------- MOUNT handler -------------------------------- */
static void handle_mount(u32 proc, xr_t *r, xw_t *w) {
    if (proc == MOUNTPROC_MNT) {
        char path[1024];
        r_bytes(r, path, sizeof(path));
        w_u32(w, 0);
        w_fh(w, 0);
        w_u32(w, 1); w_u32(w, 1); /* one auth flavor: AUTH_SYS */
    }
    /* NULL/UMNT: no body */
}

/* ---------------------------- NFS handler --------------------------------- */
static void handle_nfs(u32 proc, xr_t *r, xw_t *w) {
    switch (proc) {
    case NFSPROC_GETATTR: {
        u64 id = r_fh(r);
        const char *p = path_of(id);
        if (!p) { w_u32(w, E_NFS3ERR_NOENT); break; }
        xw_t tmp; memset(&tmp, 0, sizeof(tmp));
        u32 rc = fill_attr(p, &tmp);
        w_u32(w, rc);
        if (rc == 0) { w_ensure(w, tmp.len); memcpy(w->data + w->len, tmp.data, tmp.len); w->len += tmp.len; }
        free(tmp.data);
        break;
    }
    case NFSPROC_SETATTR: {
        u64 id = r_fh(r);
        sattr_t s = r_sattr3(r);
        r_u32(r);
        const char *p = path_of(id);
        int rc = 0;
        if (!p) rc = E_NFS3ERR_NOENT;
        else if (s.have_size && truncate(p, (off_t)s.size) != 0) rc = errno_to_nfs(errno);
        w_u32(w, rc);
        w_wcc_data_absent(w);
        break;
    }
    case NFSPROC_LOOKUP: {
        u64 dirid = r_fh(r);
        char name[512];
        r_bytes(r, name, sizeof(name));
        const char *dir = path_of(dirid);
        if (!dir) { w_u32(w, E_NFS3ERR_NOENT); w_bool(w, 0); break; }
        char full[4096];
        join_path(full, sizeof(full), dir, name);
        struct stat st;
        if (lstat(full, &st) != 0) { w_u32(w, errno_to_nfs(errno)); w_bool(w, 0); break; }
        u64 id = id_of_path(full);
        if (id == NFS_ID_INVALID) { w_u32(w, E_NFS3ERR_NOSPC); w_bool(w, 0); break; }
        w_u32(w, NFS3_OK);
        w_fh(w, id);
        w_post_op_attr_ok(w, full);
        w_bool(w, 0);
        break;
    }
    case NFSPROC_ACCESS: {
        u64 id = r_fh(r);
        u32 bits = r_u32(r);
        const char *p = path_of(id);
        w_u32(w, p ? NFS3_OK : E_NFS3ERR_NOENT);
        if (p) w_post_op_attr_ok(w, p); else w_bool(w, 0);
        w_u32(w, bits);
        break;
    }
    case NFSPROC_READ: {
        u64 id = r_fh(r);
        u64 offset = r_u64(r);
        u32 count = r_u32(r);
        if (count > 262144) count = 262144;
        const char *p = path_of(id);
        if (!p) { w_u32(w, E_NFS3ERR_NOENT); w_bool(w, 0); break; }
        int fd = open(p, O_RDONLY);
        if (fd < 0) { w_u32(w, errno_to_nfs(errno)); w_bool(w, 0); break; }
        char *buf = malloc(count ? count : 1);
        ssize_t got = pread(fd, buf, count, (off_t)offset);
        close(fd);
        if (got < 0) { w_u32(w, errno_to_nfs(errno)); w_bool(w, 0); free(buf); break; }
        struct stat st; lstat(p, &st);
        w_u32(w, NFS3_OK);
        w_post_op_attr_ok(w, p);
        w_u32(w, (u32)got);
        w_bool(w, (offset + (u64)got) >= (u64)st.st_size);
        w_bytes(w, buf, (u32)got);
        free(buf);
        break;
    }
    case NFSPROC_WRITE: {
        u64 id = r_fh(r);
        u64 offset = r_u64(r);
        u32 count = r_u32(r);
        r_u32(r); /* stable */
        u32 declared = r_u32(r);
        (void)declared;
        if (r->pos + count > r->len) { r->error = 1; break; }
        const unsigned char *data = r->data + r->pos;
        r->pos += (size_t)(((u64)count + 3) & ~3ull);
        const char *p = path_of(id);
        if (!p) { w_u32(w, E_NFS3ERR_NOENT); w_wcc_data_absent(w); break; }
        int fd = open(p, O_WRONLY);
        if (fd < 0) { w_u32(w, errno_to_nfs(errno)); w_wcc_data_absent(w); break; }
        ssize_t wr = pwrite(fd, data, count, (off_t)offset);
        close(fd);
        if (wr < 0) { w_u32(w, errno_to_nfs(errno)); w_wcc_data_absent(w); break; }
        w_u32(w, NFS3_OK);
        w_wcc_data_absent(w);
        w_u32(w, (u32)wr);
        w_u32(w, 2);
        w_u64(w, 0x5644534b4e465331ULL);
        break;
    }
    case NFSPROC_CREATE: {
        u64 dirid = r_fh(r);
        char name[512];
        r_bytes(r, name, sizeof(name));
        u32 mode3 = r_u32(r);
        u32 mode = 0644;
        if (mode3 == 2) { char v[16]; r_bytes(r, v, sizeof(v)); }
        else { sattr_t s = r_sattr3(r); if (s.have_mode) mode = s.mode; }
        const char *dir = path_of(dirid);
        if (!dir) { w_u32(w, E_NFS3ERR_NOENT); w_bool(w, 0); w_wcc_data_absent(w); break; }
        char full[4096];
        join_path(full, sizeof(full), dir, name);
        int fd = open(full, O_CREAT | O_WRONLY | O_TRUNC, mode & 0777);
        if (fd < 0) { w_u32(w, errno_to_nfs(errno)); w_bool(w, 0); w_wcc_data_absent(w); break; }
        close(fd);
        u64 id = id_of_path(full);
        if (id == NFS_ID_INVALID) { unlink(full); w_u32(w, E_NFS3ERR_NOSPC); w_bool(w, 0); w_wcc_data_absent(w); break; }
        w_u32(w, NFS3_OK);
        w_bool(w, 1); w_fh(w, id);
        w_post_op_attr_ok(w, full);
        w_wcc_data_absent(w);
        break;
    }
    case NFSPROC_MKDIR: {
        u64 dirid = r_fh(r);
        char name[512];
        r_bytes(r, name, sizeof(name));
        sattr_t s = r_sattr3(r);
        u32 mode = s.have_mode ? s.mode : 0755;
        const char *dir = path_of(dirid);
        if (!dir) { w_u32(w, E_NFS3ERR_NOENT); w_bool(w, 0); w_wcc_data_absent(w); break; }
        char full[4096];
        join_path(full, sizeof(full), dir, name);
        if (mkdir(full, mode & 0777) != 0) { w_u32(w, errno_to_nfs(errno)); w_bool(w, 0); w_wcc_data_absent(w); break; }
        u64 id = id_of_path(full);
        if (id == NFS_ID_INVALID) { rmdir(full); w_u32(w, E_NFS3ERR_NOSPC); w_bool(w, 0); w_wcc_data_absent(w); break; }
        w_u32(w, NFS3_OK);
        w_bool(w, 1); w_fh(w, id);
        w_post_op_attr_ok(w, full);
        w_wcc_data_absent(w);
        break;
    }
    case NFSPROC_REMOVE: case NFSPROC_RMDIR: {
        u64 dirid = r_fh(r);
        char name[512];
        r_bytes(r, name, sizeof(name));
        const char *dir = path_of(dirid);
        if (!dir) { w_u32(w, E_NFS3ERR_NOENT); w_wcc_data_absent(w); break; }
        char full[4096];
        join_path(full, sizeof(full), dir, name);
        int rc = (proc == NFSPROC_REMOVE) ? unlink(full) : rmdir(full);
        w_u32(w, rc == 0 ? NFS3_OK : errno_to_nfs(errno));
        w_wcc_data_absent(w);
        break;
    }
    case NFSPROC_RENAME: {
        u64 fromdir = r_fh(r);
        char fromname[512]; r_bytes(r, fromname, sizeof(fromname));
        u64 todir = r_fh(r);
        char toname[512]; r_bytes(r, toname, sizeof(toname));
        const char *fd_ = path_of(fromdir);
        const char *td_ = path_of(todir);
        if (!fd_ || !td_) { w_u32(w, E_NFS3ERR_NOENT); w_wcc_data_absent(w); w_wcc_data_absent(w); break; }
        char ffull[4096], tfull[4096];
        join_path(ffull, sizeof(ffull), fd_, fromname);
        join_path(tfull, sizeof(tfull), td_, toname);
        int rc = rename(ffull, tfull);
        w_u32(w, rc == 0 ? NFS3_OK : errno_to_nfs(errno));
        w_wcc_data_absent(w);
        w_wcc_data_absent(w);
        break;
    }
    case NFSPROC_READDIR: {
        u64 id = r_fh(r);
        u64 cookie = r_u64(r);
        r_u64(r);
        r_u32(r);
        const char *p = path_of(id);
        if (!p) { w_u32(w, E_NFS3ERR_NOENT); w_bool(w, 0); break; }
        DIR *d = opendir(p);
        if (!d) { w_u32(w, errno_to_nfs(errno)); w_bool(w, 0); break; }
        w_u32(w, NFS3_OK);
        w_post_op_attr_ok(w, p);
        w_u64(w, 0);
        u64 idx = 0;
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            idx++;
            if (idx <= cookie) continue;
            w_bool(w, 1);
            w_u64(w, (u64)de->d_ino);
            w_str(w, de->d_name);
            w_u64(w, idx);
        }
        closedir(d);
        w_bool(w, 0);
        w_bool(w, 1);
        break;
    }
    case NFSPROC_FSSTAT: {
        u64 id = r_fh(r);
        const char *p = path_of(id);
        if (!p) { w_u32(w, E_NFS3ERR_NOENT); w_bool(w, 0); break; }
        struct statvfs sv;
        if (statvfs(p, &sv) != 0) { w_u32(w, errno_to_nfs(errno)); w_post_op_attr_ok(w, p); break; }
        w_u32(w, NFS3_OK);
        w_post_op_attr_ok(w, p);
        u64 tb = (u64)sv.f_blocks * sv.f_frsize;
        u64 fb = (u64)sv.f_bfree * sv.f_frsize;
        u64 ab = (u64)sv.f_bavail * sv.f_frsize;
        w_u64(w, tb); w_u64(w, fb); w_u64(w, ab);
        w_u64(w, (u64)sv.f_files); w_u64(w, (u64)sv.f_ffree); w_u64(w, (u64)sv.f_favail);
        w_u32(w, 0);
        break;
    }
    case NFSPROC_FSINFO: {
        u64 id = r_fh(r);
        const char *p = path_of(id);
        w_u32(w, NFS3_OK);
        if (p) w_post_op_attr_ok(w, p); else w_bool(w, 0);
        w_u32(w, 65536); w_u32(w, 65536); w_u32(w, 4096);
        w_u32(w, 65536); w_u32(w, 65536); w_u32(w, 4096);
        w_u32(w, 4096);
        w_u64(w, 0xFFFFFFFFFFULL);
        w_u32(w, 1); w_u32(w, 0);
        w_u32(w, 8u | 16u);
        break;
    }
    case NFSPROC_PATHCONF: {
        u64 id = r_fh(r);
        const char *p = path_of(id);
        w_u32(w, NFS3_OK);
        if (p) w_post_op_attr_ok(w, p); else w_bool(w, 0);
        w_u32(w, 32000); w_u32(w, 255);
        w_bool(w, 1); w_bool(w, 0); w_bool(w, 0); w_bool(w, 1);
        break;
    }
    case NFSPROC_COMMIT: {
        r_fh(r); r_u64(r); r_u32(r);
        w_u32(w, NFS3_OK);
        w_wcc_data_absent(w);
        w_u64(w, 0x5644534b4e465331ULL);
        break;
    }
    default:
        break;
    }
}

/* ------------------------------ RPC loop ---------------------------------- */
static int recv_all(int s, unsigned char *buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r <= 0) return 0;
        got += r;
    }
    return 1;
}
static int send_all(int s, const unsigned char *buf, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(s, buf + sent, n - sent, 0);
        if (r <= 0) return 0;
        sent += r;
    }
    return 1;
}

typedef void (*dispatch_fn)(u32 proc, xr_t *r, xw_t *w);

static void serve_connection(int cs, u32 prog, dispatch_fn fn) {
    unsigned char *inbuf = NULL; size_t incap = 0;
    for (;;) {
        size_t total = 0;
        for (;;) {
            unsigned char hdr[4];
            if (!recv_all(cs, hdr, 4)) { free(inbuf); return; }
            u32 m = ((u32)hdr[0] << 24) | ((u32)hdr[1] << 16) | ((u32)hdr[2] << 8) | hdr[3];
            int last = (m & 0x80000000u) != 0;
            u32 flen = m & 0x7FFFFFFFu;
            if (total + flen > incap) {
                size_t nc = incap ? incap * 2 : 65536;
                while (nc < total + flen) nc *= 2;
                inbuf = realloc(inbuf, nc);
                incap = nc;
            }
            if (flen && !recv_all(cs, inbuf + total, (int)flen)) { free(inbuf); return; }
            total += flen;
            if (last) break;
        }

        xr_t r = { inbuf, total, 0, 0 };
        u32 xid = r_u32(&r);
        u32 msgtype = r_u32(&r);
        if (msgtype != 0) continue;
        r_u32(&r); /* rpcvers */
        u32 rprog = r_u32(&r);
        r_u32(&r); /* vers */
        u32 proc = r_u32(&r);
        r_skip_auth(&r);
        r_skip_auth(&r);
        if (r.error) continue;

        xw_t w; memset(&w, 0, sizeof(w));
        if (rprog == prog) fn(proc, &r, &w);

        xw_t full; memset(&full, 0, sizeof(full));
        w_u32(&full, xid);
        w_u32(&full, 1);
        w_u32(&full, 0);
        w_u32(&full, 0); w_u32(&full, 0);
        w_u32(&full, 0);
        w_ensure(&full, w.len);
        memcpy(full.data + full.len, w.data, w.len);
        full.len += w.len;
        free(w.data);

        u32 marker = 0x80000000u | (u32)full.len;
        unsigned char mh[4] = { (unsigned char)(marker>>24), (unsigned char)(marker>>16),
                                 (unsigned char)(marker>>8), (unsigned char)marker };
        send_all(cs, mh, 4);
        send_all(cs, full.data, (int)full.len);
        free(full.data);
    }
}

typedef struct { int port; u32 prog; dispatch_fn fn; } listener_arg_t;

static void *listener_thread(void *arg0) {
    listener_arg_t *a = (listener_arg_t *)arg0;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(a->port);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "vdisknfsd: bind port %d failed: %s\n", a->port, strerror(errno));
        return NULL;
    }
    listen(ls, 4);
    fprintf(stderr, "vdisknfsd: listening on port %d (prog %u)\n", a->port, a->prog);
    for (;;) {
        int cs = accept(ls, NULL, NULL);
        if (cs < 0) continue;
        int nodelay = 1;
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        serve_connection(cs, a->prog, a->fn);
        close(cs);
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: vdisknfsd <nfs_port> <mount_port> <export_root>\n");
        return 1;
    }
    int nfs_port = atoi(argv[1]);
    int mount_port = atoi(argv[2]);
    strncpy(g_root, argv[3], sizeof(g_root) - 1);
    if (strlen(g_root) > 1) {
        size_t l = strlen(g_root);
        if (g_root[l-1] == '/') g_root[l-1] = '\0';
    }
    if (g_root[0] == '\0') strcpy(g_root, "/");

    signal(SIGPIPE, SIG_IGN);

    listener_arg_t mount_arg = { mount_port, PROG_MOUNT, handle_mount };
    listener_arg_t nfs_arg = { nfs_port, PROG_NFS, handle_nfs };

    pthread_t th;
    pthread_create(&th, NULL, listener_thread, &mount_arg);
    listener_thread(&nfs_arg);
    return 0;
}
