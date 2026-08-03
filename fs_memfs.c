// In-memory filesystem for vdisk, built on the WinFsp FUSE compatibility API.
// A single instance backs one mounted drive; file data lives either in system
// RAM (heap) or in GPU VRAM (CUDA), selected at mount time. This process stays
// alive for the whole lifetime of the disk -- WinFsp unmounts automatically
// when the process exits.

#define FUSE_USE_VERSION 29

#include <fuse/fuse.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include "fs_memfs.h"
#include "vram_allocator.h"

#define MODE_DIR  0040000
#define MODE_REG  0100000
#define NAME_MAX_LEN 255
#define GRANULARITY (64 * 1024)

typedef struct node {
    char           name[NAME_MAX_LEN + 1];
    int            is_dir;
    unsigned int   mode;          // permission bits (0777 style)
    time_t         atime, mtime, ctime;

    struct node   *parent;
    struct node   *children;      // first child   (directories)
    struct node   *next;          // next sibling

    // File contents (unused for directories).
    unsigned char *ram;           // RAM backend buffer
    size_t         vram_dptr;     // VRAM backend device pointer
    size_t         size;          // logical file size
    size_t         cap;           // allocated capacity
} node_t;

static node_t           *g_root;
static int               g_use_vram;
static unsigned long long g_capacity;   // soft quota, bytes
static unsigned long long g_used;       // sum of file sizes, bytes
static CRITICAL_SECTION  g_lock;

// --- blob storage (RAM or VRAM) -------------------------------------------

static size_t round_up(size_t x) {
    if (x == 0) x = 1;
    return (x + GRANULARITY - 1) / GRANULARITY * GRANULARITY;
}

static int blob_ensure(node_t *n, size_t need) {
    if (need <= n->cap) return 1;
    size_t newcap = round_up(need);

    if (g_use_vram) {
        size_t nd = vram_allocate(newcap);
        if (!nd) return 0;
        if (n->vram_dptr && n->size) {
            unsigned char *tmp = (unsigned char *)malloc(n->size);
            if (tmp) {
                vram_read(n->vram_dptr, 0, tmp, n->size);
                vram_write(nd, 0, tmp, n->size);
                free(tmp);
            }
        }
        if (n->vram_dptr) vram_free(n->vram_dptr);
        n->vram_dptr = nd;
    } else {
        unsigned char *nb = (unsigned char *)realloc(n->ram, newcap);
        if (!nb) return 0;
        n->ram = nb;
    }
    n->cap = newcap;
    return 1;
}

static void blob_free(node_t *n) {
    if (g_use_vram) {
        if (n->vram_dptr) vram_free(n->vram_dptr);
        n->vram_dptr = 0;
    } else {
        free(n->ram);
        n->ram = NULL;
    }
    n->cap = 0;
}

static void blob_read(node_t *n, size_t off, void *dst, size_t len) {
    if (g_use_vram) vram_read(n->vram_dptr, off, dst, len);
    else            memcpy(dst, n->ram + off, len);
}

static void blob_write(node_t *n, size_t off, const void *src, size_t len) {
    if (g_use_vram) vram_write(n->vram_dptr, off, src, len);
    else            memcpy(n->ram + off, src, len);
}

// Fill [start, start+len) of a file's blob with zeros.
static void blob_zero(node_t *n, size_t start, size_t len) {
    unsigned char zeros[GRANULARITY];
    memset(zeros, 0, sizeof(zeros));
    size_t off = start;
    while (len > 0) {
        size_t chunk = len < sizeof(zeros) ? len : sizeof(zeros);
        blob_write(n, off, zeros, chunk);
        off += chunk;
        len -= chunk;
    }
}

// --- tree helpers ---------------------------------------------------------

static node_t *node_child(node_t *dir, const char *name) {
    for (node_t *c = dir->children; c; c = c->next)
        if (_stricmp(c->name, name) == 0) return c;
    return NULL;
}

static node_t *node_lookup(const char *path) {
    if (!path || path[0] != '/') return NULL;
    if (path[1] == '\0') return g_root;

    char buf[4096];
    if (strlen(path + 1) >= sizeof(buf)) return NULL;
    strcpy_s(buf, sizeof(buf), path + 1);

    node_t *cur = g_root;
    char *ctx = NULL;
    for (char *tok = strtok_s(buf, "/", &ctx); tok; tok = strtok_s(NULL, "/", &ctx)) {
        if (!cur->is_dir) return NULL;
        cur = node_child(cur, tok);
        if (!cur) return NULL;
    }
    return cur;
}

static node_t *parent_lookup(const char *path, const char **leaf) {
    const char *slash = strrchr(path, '/');
    if (!slash) return NULL;
    *leaf = slash + 1;
    if (slash == path) return g_root;

    char buf[4096];
    size_t n = (size_t)(slash - path);
    if (n >= sizeof(buf)) return NULL;
    memcpy(buf, path, n);
    buf[n] = '\0';
    return node_lookup(buf);
}

static node_t *node_new(node_t *parent, const char *name, int is_dir) {
    node_t *n = (node_t *)calloc(1, sizeof(node_t));
    if (!n) return NULL;
    strcpy_s(n->name, sizeof(n->name), name);
    n->is_dir = is_dir;
    n->mode = is_dir ? 0777 : 0666;
    n->atime = n->mtime = n->ctime = time(NULL);
    n->parent = parent;
    n->next = parent->children;
    parent->children = n;
    return n;
}

static void node_detach(node_t *parent, node_t *child) {
    node_t **pp = &parent->children;
    while (*pp) {
        if (*pp == child) { *pp = child->next; child->next = NULL; return; }
        pp = &(*pp)->next;
    }
}

// --- FUSE operations ------------------------------------------------------

static void fill_stat(node_t *n, struct fuse_stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode  = (n->is_dir ? MODE_DIR : MODE_REG) | (n->mode & 0777);
    st->st_nlink = 1;
    st->st_size  = (fuse_off_t)(n->is_dir ? 0 : n->size);
    st->st_atim.tv_sec = n->atime;
    st->st_mtim.tv_sec = n->mtime;
    st->st_ctim.tv_sec = n->ctime;
    st->st_birthtim.tv_sec = n->ctime;
    st->st_blksize = 4096;
    st->st_blocks  = (fuse_blkcnt_t)((n->size + 511) / 512);
}

static int op_getattr(const char *path, struct fuse_stat *st) {
    EnterCriticalSection(&g_lock);
    node_t *n = node_lookup(path);
    int r = 0;
    if (!n) r = -ENOENT;
    else fill_stat(n, st);
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_fgetattr(const char *path, struct fuse_stat *st, struct fuse_file_info *fi) {
    (void)fi;
    return op_getattr(path, st);
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      fuse_off_t off, struct fuse_file_info *fi) {
    (void)off; (void)fi;
    EnterCriticalSection(&g_lock);
    node_t *n = node_lookup(path);
    if (!n)          { LeaveCriticalSection(&g_lock); return -ENOENT; }
    if (!n->is_dir)  { LeaveCriticalSection(&g_lock); return -ENOTDIR; }

    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    for (node_t *c = n->children; c; c = c->next)
        filler(buf, c->name, NULL, 0);
    LeaveCriticalSection(&g_lock);
    return 0;
}

static int op_mkdir(const char *path, fuse_mode_t mode) {
    const char *leaf;
    EnterCriticalSection(&g_lock);
    node_t *parent = parent_lookup(path, &leaf);
    int r = 0;
    if (!parent || !parent->is_dir)       r = -ENOENT;
    else if (strlen(leaf) > NAME_MAX_LEN) r = -ENAMETOOLONG;
    else if (node_child(parent, leaf))    r = -EEXIST;
    else {
        node_t *n = node_new(parent, leaf, 1);
        if (n) n->mode = mode & 0777;
        else   r = -ENOMEM;
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_rmdir(const char *path) {
    const char *leaf;
    EnterCriticalSection(&g_lock);
    node_t *parent = parent_lookup(path, &leaf);
    node_t *n = parent ? node_child(parent, leaf) : NULL;
    int r = 0;
    if (!n)              r = -ENOENT;
    else if (!n->is_dir) r = -ENOTDIR;
    else if (n->children) r = -ENOTEMPTY;
    else { node_detach(parent, n); free(n); }
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_create(const char *path, fuse_mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    const char *leaf;
    EnterCriticalSection(&g_lock);
    node_t *parent = parent_lookup(path, &leaf);
    int r = 0;
    if (!parent || !parent->is_dir) { r = -ENOENT; goto out; }
    if (strlen(leaf) > NAME_MAX_LEN) { r = -ENAMETOOLONG; goto out; }

    node_t *ex = node_child(parent, leaf);
    if (ex) {
        if (ex->is_dir) { r = -EISDIR; goto out; }
        // Truncate the existing file (CREATE_ALWAYS semantics).
        g_used -= ex->size;
        blob_free(ex);
        ex->size = 0;
        ex->mtime = time(NULL);
        goto out;
    }
    node_t *n = node_new(parent, leaf, 0);
    if (!n) r = -ENOMEM;
    else    n->mode = mode & 0777;
out:
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_open(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    EnterCriticalSection(&g_lock);
    node_t *n = node_lookup(path);
    int r = !n ? -ENOENT : (n->is_dir ? -EISDIR : 0);
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_read(const char *path, char *buf, size_t size, fuse_off_t off,
                   struct fuse_file_info *fi) {
    (void)fi;
    EnterCriticalSection(&g_lock);
    node_t *n = node_lookup(path);
    if (!n || n->is_dir) { LeaveCriticalSection(&g_lock); return -ENOENT; }

    int ret = 0;
    if ((size_t)off < n->size) {
        size_t avail = n->size - (size_t)off;
        size_t len = size < avail ? size : avail;
        blob_read(n, (size_t)off, buf, len);
        n->atime = time(NULL);
        ret = (int)len;
    }
    LeaveCriticalSection(&g_lock);
    return ret;
}

static int op_write(const char *path, const char *buf, size_t size, fuse_off_t off,
                    struct fuse_file_info *fi) {
    (void)fi;
    EnterCriticalSection(&g_lock);
    node_t *n = node_lookup(path);
    if (!n || n->is_dir) { LeaveCriticalSection(&g_lock); return -ENOENT; }

    size_t end = (size_t)off + size;
    int r = (int)size;
    if (end > n->size) {
        unsigned long long grow = end - n->size;
        if (g_used + grow > g_capacity)  { r = -ENOSPC; goto out; }
        if (!blob_ensure(n, end))        { r = -ENOSPC; goto out; }
        // Zero any gap created by a sparse write past the old end.
        if ((size_t)off > n->size)
            blob_zero(n, n->size, (size_t)off - n->size);
        g_used += grow;
        n->size = end;
    }
    blob_write(n, (size_t)off, buf, size);
    n->mtime = time(NULL);
out:
    LeaveCriticalSection(&g_lock);
    return r;
}

static int do_truncate(node_t *n, size_t newsize) {
    if (newsize > n->size) {
        unsigned long long grow = newsize - n->size;
        if (g_used + grow > g_capacity) return -ENOSPC;
        if (!blob_ensure(n, newsize))   return -ENOSPC;
        blob_zero(n, n->size, newsize - n->size);
        g_used += grow;
    } else {
        g_used -= (n->size - newsize);
    }
    n->size = newsize;
    n->mtime = time(NULL);
    return 0;
}

static int op_truncate(const char *path, fuse_off_t size) {
    EnterCriticalSection(&g_lock);
    node_t *n = node_lookup(path);
    int r = (!n || n->is_dir) ? -ENOENT : do_truncate(n, (size_t)size);
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_ftruncate(const char *path, fuse_off_t size, struct fuse_file_info *fi) {
    (void)fi;
    return op_truncate(path, size);
}

static int op_unlink(const char *path) {
    const char *leaf;
    EnterCriticalSection(&g_lock);
    node_t *parent = parent_lookup(path, &leaf);
    node_t *n = parent ? node_child(parent, leaf) : NULL;
    int r = 0;
    if (!n)             r = -ENOENT;
    else if (n->is_dir) r = -EISDIR;
    else {
        g_used -= n->size;
        blob_free(n);
        node_detach(parent, n);
        free(n);
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_rename(const char *from, const char *to) {
    const char *fleaf, *tleaf;
    EnterCriticalSection(&g_lock);
    int r = 0;
    node_t *fp = parent_lookup(from, &fleaf);
    node_t *fn = fp ? node_child(fp, fleaf) : NULL;
    if (!fn) { r = -ENOENT; goto out; }

    node_t *tp = parent_lookup(to, &tleaf);
    if (!tp || !tp->is_dir)       { r = -ENOENT; goto out; }
    if (strlen(tleaf) > NAME_MAX_LEN) { r = -ENAMETOOLONG; goto out; }

    node_t *tn = node_child(tp, tleaf);
    if (tn) {
        if (tn->is_dir && tn->children) { r = -ENOTEMPTY; goto out; }
        if (!tn->is_dir) g_used -= tn->size;
        blob_free(tn);
        node_detach(tp, tn);
        free(tn);
    }
    node_detach(fp, fn);
    strcpy_s(fn->name, sizeof(fn->name), tleaf);
    fn->parent = tp;
    fn->next = tp->children;
    tp->children = fn;
out:
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_chmod(const char *path, fuse_mode_t mode) {
    EnterCriticalSection(&g_lock);
    node_t *n = node_lookup(path);
    int r = 0;
    if (!n) r = -ENOENT;
    else n->mode = mode & 0777;
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_chown(const char *path, fuse_uid_t uid, fuse_gid_t gid) {
    (void)uid; (void)gid;
    EnterCriticalSection(&g_lock);
    int r = node_lookup(path) ? 0 : -ENOENT;
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_utimens(const char *path, const struct fuse_timespec tv[2]) {
    EnterCriticalSection(&g_lock);
    node_t *n = node_lookup(path);
    int r = 0;
    if (!n) r = -ENOENT;
    else if (tv) { n->atime = tv[0].tv_sec; n->mtime = tv[1].tv_sec; }
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_statfs(const char *path, struct fuse_statvfs *st) {
    (void)path;
    EnterCriticalSection(&g_lock);
    memset(st, 0, sizeof(*st));
    st->f_bsize  = 4096;
    st->f_frsize = 4096;
    st->f_blocks = (fuse_fsblkcnt_t)(g_capacity / 4096);
    fuse_fsblkcnt_t used_blocks = (fuse_fsblkcnt_t)((g_used + 4095) / 4096);
    fuse_fsblkcnt_t free_blocks = st->f_blocks > used_blocks ? st->f_blocks - used_blocks : 0;
    st->f_bfree  = free_blocks;
    st->f_bavail = free_blocks;
    st->f_files  = 1000000;
    st->f_ffree  = 1000000;
    st->f_favail = 1000000;
    st->f_namemax = NAME_MAX_LEN;
    LeaveCriticalSection(&g_lock);
    return 0;
}

static int op_access(const char *path, int mask) {
    (void)mask;
    EnterCriticalSection(&g_lock);
    int r = node_lookup(path) ? 0 : -ENOENT;
    LeaveCriticalSection(&g_lock);
    return r;
}

static int op_flush(const char *path, struct fuse_file_info *fi)  { (void)path; (void)fi; return 0; }
static int op_release(const char *path, struct fuse_file_info *fi){ (void)path; (void)fi; return 0; }
static int op_fsync(const char *path, int d, struct fuse_file_info *fi) { (void)path; (void)d; (void)fi; return 0; }

// --- entry point ----------------------------------------------------------

int fs_run(int use_vram, unsigned long long size_mb, char drive_letter, const char *fs_name) {
    g_use_vram = use_vram;
    g_capacity = size_mb * 1024ULL * 1024ULL;
    g_used = 0;
    InitializeCriticalSection(&g_lock);

    if (use_vram) {
        vram_info_t vi;
        if (!vram_init(&vi)) return 2; // CUDA/VRAM unavailable
    }

    g_root = (node_t *)calloc(1, sizeof(node_t));
    if (!g_root) return 3;
    g_root->is_dir = 1;
    g_root->mode = 0777;
    g_root->atime = g_root->mtime = g_root->ctime = time(NULL);

    struct fuse_operations ops;
    memset(&ops, 0, sizeof(ops));
    ops.getattr   = op_getattr;
    ops.fgetattr  = op_fgetattr;
    ops.readdir   = op_readdir;
    ops.mkdir     = op_mkdir;
    ops.rmdir     = op_rmdir;
    ops.create    = op_create;
    ops.open      = op_open;
    ops.read      = op_read;
    ops.write     = op_write;
    ops.unlink    = op_unlink;
    ops.rename    = op_rename;
    ops.truncate  = op_truncate;
    ops.ftruncate = op_ftruncate;
    ops.chmod     = op_chmod;
    ops.chown     = op_chown;
    ops.utimens   = op_utimens;
    ops.statfs    = op_statfs;
    ops.access    = op_access;
    ops.flush     = op_flush;
    ops.release   = op_release;
    ops.fsync     = op_fsync;

    char mountpoint[8];
    snprintf(mountpoint, sizeof(mountpoint), "%c:", drive_letter);

    char opts[128];
    snprintf(opts, sizeof(opts), "uid=-1,gid=-1,FileSystemName=%s",
             (fs_name && *fs_name) ? fs_name : "NTFS");

    char a0[] = "vdisk";
    char a1[] = "-f";                       // foreground: block in this process
    char a2[] = "-o";
    char *argv[] = { a0, a1, a2, opts, mountpoint, NULL };
    int argc = 5;

    return fuse_main_real(argc, argv, &ops, sizeof(ops), NULL);
}
