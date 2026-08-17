#define _CRT_SECURE_NO_WARNINGS
#include "nfsclient.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define PROG_MOUNT 100005u
#define PROG_NFS   100003u

#define MOUNTPROC_MNT 1u

#define NFSPROC_GETATTR  1u
#define NFSPROC_SETATTR  2u
#define NFSPROC_LOOKUP   3u
#define NFSPROC_READ     6u
#define NFSPROC_WRITE    7u
#define NFSPROC_CREATE   8u
#define NFSPROC_MKDIR    9u
#define NFSPROC_REMOVE   12u
#define NFSPROC_RMDIR    13u
#define NFSPROC_RENAME   14u
#define NFSPROC_READDIR  16u
#define NFSPROC_FSSTAT   18u

#define NFS3_OK 0u

// ---------------------------------------------------------------------------
// XDR helpers (same conventions as the earlier server prototype: big-endian
// u32/u64, 4-byte-length-prefixed + zero-padded opaque/strings).
// ---------------------------------------------------------------------------

typedef struct { unsigned char *data; size_t len, cap; } xwriter_t;

static void w_ensure(xwriter_t *w, size_t extra) {
    if (w->len + extra <= w->cap) return;
    size_t newcap = w->cap ? w->cap * 2 : 4096;
    while (newcap < w->len + extra) newcap *= 2;
    w->data = (unsigned char *)realloc(w->data, newcap);
    w->cap = newcap;
}
static void w_u32(xwriter_t *w, unsigned int v) {
    w_ensure(w, 4);
    w->data[w->len++] = (unsigned char)(v >> 24);
    w->data[w->len++] = (unsigned char)(v >> 16);
    w->data[w->len++] = (unsigned char)(v >> 8);
    w->data[w->len++] = (unsigned char)v;
}
static void w_u64(xwriter_t *w, unsigned long long v) {
    w_u32(w, (unsigned int)(v >> 32));
    w_u32(w, (unsigned int)v);
}
static void w_bytes(xwriter_t *w, const void *data, unsigned int n) {
    w_u32(w, n);
    w_ensure(w, ((size_t)n + 3) & ~(size_t)3);
    memcpy(w->data + w->len, data, n);
    w->len += n;
    unsigned int pad = (unsigned int)((4 - (n & 3)) & 3);
    for (unsigned int i = 0; i < pad; i++) w->data[w->len++] = 0;
}
static void w_str(xwriter_t *w, const char *s) { w_bytes(w, s, (unsigned int)strlen(s)); }
static void w_fh(xwriter_t *w, const nfs_fh_t *fh) { w_bytes(w, fh->data, fh->len); }

typedef struct { const unsigned char *data; size_t len, pos; int error; } xreader_t;

static unsigned int rd_u32(xreader_t *r) {
    if (r->error || r->pos + 4 > r->len) { r->error = 1; return 0; }
    unsigned int v = ((unsigned int)r->data[r->pos] << 24) | ((unsigned int)r->data[r->pos+1] << 16) |
                      ((unsigned int)r->data[r->pos+2] << 8) | (unsigned int)r->data[r->pos+3];
    r->pos += 4;
    return v;
}
static unsigned long long rd_u64(xreader_t *r) {
    unsigned long long hi = rd_u32(r), lo = rd_u32(r);
    return (hi << 32) | lo;
}
static unsigned int rd_bytes(xreader_t *r, unsigned char *out, unsigned int outcap) {
    unsigned int n = rd_u32(r);
    if (r->error) return 0;
    // 64-bit: n comes from the peer and near UINT_MAX wraps (n+3) back to
    // ~0 in 32-bit arithmetic, defeating the bounds check and letting
    // memcpy read up to outcap bytes past the real received buffer.
    unsigned long long padded = ((unsigned long long)n + 3) & ~3ull;
    if ((unsigned long long)r->pos + padded > (unsigned long long)r->len) { r->error = 1; return 0; }
    unsigned int copy = n < outcap ? n : outcap;
    memcpy(out, r->data + r->pos, copy);
    r->pos += (size_t)padded;
    return copy;
}
static void rd_skip_bytes(xreader_t *r) {
    unsigned int n = rd_u32(r);
    if (r->error) return;
    unsigned long long padded = ((unsigned long long)n + 3) & ~3ull;
    if ((unsigned long long)r->pos + padded > (unsigned long long)r->len) { r->error = 1; return; }
    r->pos += (size_t)padded;
}
static void rd_fh(xreader_t *r, nfs_fh_t *fh) {
    fh->len = rd_bytes(r, fh->data, sizeof(fh->data));
}
static int rd_bool(xreader_t *r) { return rd_u32(r) != 0; }

static void rd_fattr3(xreader_t *r, nfs_attr_t *a) {
    a->type = rd_u32(r);
    a->mode = rd_u32(r);
    a->nlink = rd_u32(r);
    a->uid = rd_u32(r);
    a->gid = rd_u32(r);
    a->size = rd_u64(r);
    a->used = rd_u64(r);
    rd_u32(r); rd_u32(r); // rdev
    rd_u64(r); // fsid
    a->fileid = rd_u64(r);
    a->atime = (long)rd_u32(r); rd_u32(r);
    a->mtime = (long)rd_u32(r); rd_u32(r);
    a->ctime = (long)rd_u32(r); rd_u32(r);
}

// post_op_attr: bool + fattr3 if present. Returns 1 if present.
static int rd_post_op_attr(xreader_t *r, nfs_attr_t *out) {
    if (rd_bool(r)) { rd_fattr3(r, out); return 1; }
    return 0;
}
static void rd_skip_wcc_data(xreader_t *r) {
    if (rd_bool(r)) { rd_u64(r); rd_u32(r); rd_u32(r); } // pre_op_attr (size,mtime,ctime)
    nfs_attr_t tmp;
    rd_post_op_attr(r, &tmp);
}
static int rd_post_op_fh3(xreader_t *r, nfs_fh_t *fh) {
    if (rd_bool(r)) { rd_fh(r, fh); return 1; }
    return 0;
}

// ---------------------------------------------------------------------------

struct nfsclient {
    SOCKET nfs_sock;
    nfs_fh_t root;
    CRITICAL_SECTION lock;
    unsigned int xid;
    char host[64];
    unsigned short nfs_port;
};

static int recv_all(SOCKET s, unsigned char *buf, int n) {
    int got = 0;
    while (got < n) {
        int r = recv(s, (char *)buf + got, n - got, 0);
        if (r <= 0) return 0;
        got += r;
    }
    return 1;
}
static int send_all(SOCKET s, const unsigned char *buf, int n) {
    int sent = 0;
    while (sent < n) {
        int r = send(s, (const char *)buf + sent, n - sent, 0);
        if (r <= 0) return 0;
        sent += r;
    }
    return 1;
}

static SOCKET connect_to(const char *host, unsigned short port) {
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) { closesocket(s); return INVALID_SOCKET; }

    // A plain blocking connect() has no timeout of its own and can hang for
    // the OS's default TCP connect-retry window (can run tens of seconds to
    // minutes) if the hostfwd port is in a half-dead state. nfs_call()'s
    // reconnect-on-failure path calls this from inside a live FUSE dispatch,
    // so a stuck connect() here blocks whatever Explorer/WinFsp thread is
    // waiting on it -- exactly the kind of stall reported as "Explorer died,
    // had to reboot the PC". Bound every attempt to a few seconds instead.
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    int rc = connect(s, (struct sockaddr *)&addr, sizeof(addr));
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) { closesocket(s); return INVALID_SOCKET; }
    if (rc != 0) {
        fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
        struct timeval tv = { 3, 0 };
        int sel = select(0, NULL, &wf, NULL, &tv);
        int err = 0; int errlen = (int)sizeof(err);
        if (sel <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &errlen) != 0 || err != 0) {
            closesocket(s);
            return INVALID_SOCKET;
        }
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);

    int nodelay = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
    // Without this, a stalled/lost reply blocks recv() forever -- observed
    // empirically right after a fresh hostfwd/NAT mapping is created, the
    // very first request can hang even though the same server answers
    // instantly moments later. A bounded timeout + retry (see nfsc_connect)
    // turns that into a brief, self-healing delay instead of a permanent hang.
    DWORD rcvtimeo = 8000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&rcvtimeo, sizeof(rcvtimeo));
    // A wedged connection can block send() just as easily as recv() (e.g. a
    // full send buffer with a peer that stopped acking) -- without this, a
    // single stuck WRITE call would hang the calling Explorer/WinFsp thread
    // forever instead of failing over to nfs_call()'s reconnect-and-retry.
    DWORD sndtimeo = 8000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&sndtimeo, sizeof(sndtimeo));
    return s;
}

// Sends one RPC CALL (prog/vers/proc + AUTH_NONE cred/verf + 'args' body) on
// 's' and returns the reply body (positioned after accept_stat, which must
// have been SUCCESS) via 'out'. Returns 0 on any transport/RPC-level failure.
static int rpc_call(SOCKET s, unsigned int xid, unsigned int prog, unsigned int vers,
                     unsigned int proc, const xwriter_t *args, unsigned char **reply_buf,
                     xreader_t *out) {
    xwriter_t w; memset(&w, 0, sizeof(w));
    w_u32(&w, xid);
    w_u32(&w, 0); // CALL
    w_u32(&w, 2); // rpcvers
    w_u32(&w, prog);
    w_u32(&w, vers);
    w_u32(&w, proc);
    w_u32(&w, 0); w_u32(&w, 0); // cred: AUTH_NONE
    w_u32(&w, 0); w_u32(&w, 0); // verf: AUTH_NONE
    if (args) { w_ensure(&w, args->len); memcpy(w.data + w.len, args->data, args->len); w.len += args->len; }

    unsigned int marker = 0x80000000u | (unsigned int)w.len;
    unsigned char mhdr[4] = { (unsigned char)(marker >> 24), (unsigned char)(marker >> 16),
                               (unsigned char)(marker >> 8), (unsigned char)marker };
    int ok = send_all(s, mhdr, 4) && send_all(s, w.data, (int)w.len);
    free(w.data);
    if (!ok) {
        fprintf(stderr, "[nfsclient] rpc_call: send failed (prog=%u proc=%u)\n", prog, proc);
        fflush(stderr);
        return 0;
    }

    // Read the reply (record-marked, possibly multiple fragments).
    size_t total = 0, cap = 0;
    unsigned char *buf = NULL;
    for (;;) {
        unsigned char hdr[4];
        if (!recv_all(s, hdr, 4)) {
            fprintf(stderr, "[nfsclient] rpc_call: recv_all(hdr) failed/EOF (prog=%u proc=%u)\n", prog, proc);
            fflush(stderr);
            free(buf); return 0;
        }
        unsigned int m = ((unsigned int)hdr[0] << 24) | ((unsigned int)hdr[1] << 16) |
                          ((unsigned int)hdr[2] << 8) | hdr[3];
        int last = (m & 0x80000000u) != 0;
        unsigned int flen = m & 0x7FFFFFFFu;
        if (total + flen > cap) {
            size_t newcap = cap ? cap * 2 : 65536;
            while (newcap < total + flen) newcap *= 2;
            buf = (unsigned char *)realloc(buf, newcap);
            cap = newcap;
        }
        if (flen && !recv_all(s, buf + total, (int)flen)) { free(buf); return 0; }
        total += flen;
        if (last) break;
    }

    xreader_t r = { buf, total, 0, 0 };
    unsigned int rxid = rd_u32(&r);
    unsigned int msgtype = rd_u32(&r);
    unsigned int reply_stat = rd_u32(&r);
    rd_u32(&r); rd_u32(&r); // verf flavor/len (assume 0 len)
    unsigned int accept_stat = rd_u32(&r);
    if (r.error || rxid != xid || msgtype != 1 || reply_stat != 0 || accept_stat != 0) {
        free(buf);
        return 0;
    }
    *reply_buf = buf;
    *out = r;
    return 1;
}

// Wraps rpc_call with a single transparent reconnect-and-retry: a mid-session
// TCP drop (observed empirically under QEMU hostfwd/SLIRP with the guest CPU
// under heavy TCG-emulation load -- dozens of requests succeed, then one
// hiccups) used to be permanent, since only the initial nfsc_connect() had
// retry logic. Any later rpc_call() failure killed the mount for good. This
// gives every NFS-program call one chance to reconnect and re-send before
// giving up for real.
static int nfs_call(nfsclient_t *c, unsigned int proc, const xwriter_t *args,
                     unsigned char **reply_buf, xreader_t *out) {
    if (rpc_call(c->nfs_sock, ++c->xid, PROG_NFS, 3, proc, args, reply_buf, out)) return 1;
    fprintf(stderr, "[nfsclient] nfs_call: proc=%u failed, reconnecting...\n", proc);
    fflush(stderr);
    closesocket(c->nfs_sock);
    SOCKET ns = connect_to(c->host, c->nfs_port);
    if (ns == INVALID_SOCKET) { c->nfs_sock = INVALID_SOCKET; return 0; }
    c->nfs_sock = ns;
    return rpc_call(c->nfs_sock, ++c->xid, PROG_NFS, 3, proc, args, reply_buf, out);
}

// Maps an NFS3ERR_* status to a negative errno for FUSE-style return values.
static int map_errno(unsigned int status) {
    switch (status) {
        case 0: return 0;
        case 1: return -1;      // EPERM
        case 2: return -2;      // ENOENT
        case 5: return -5;      // EIO
        case 13: return -13;    // EACCES
        case 17: return -17;    // EEXIST
        case 20: return -20;    // ENOTDIR
        case 21: return -21;    // EISDIR
        case 28: return -28;    // ENOSPC
        case 66: return -39;    // ENOTEMPTY (linux errno 39)
        default: return -5;     // EIO fallback
    }
}

nfs_fh_t nfsc_root_fh(nfsclient_t *c) { return c->root; }

nfsclient_t *nfsc_connect(const char *host, unsigned short nfs_port,
                           unsigned short mount_port, const char *export_path) {
    static int wsa_inited = 0;
    if (!wsa_inited) {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return NULL;
        wsa_inited = 1;
    }

    // The very first request over a freshly-created hostfwd/NAT mapping can
    // stall (observed empirically -- the same server answers instantly a
    // few seconds later), so each attempt gets a bounded timeout (see
    // connect_to's SO_RCVTIMEO) and a fresh socket/connection is retried a
    // few times rather than giving up (or hanging) on the first stall.
    int ok = 0;
    unsigned int mnt_stat = 0;
    nfs_fh_t root; memset(&root, 0, sizeof(root));
    for (int attempt = 1; attempt <= 5 && !ok; attempt++) {
        fprintf(stderr, "[nfsclient] connect_to(%s, %u) attempt %d...\n", host, mount_port, attempt);
        fflush(stderr);
        SOCKET ms = connect_to(host, mount_port);
        if (ms == INVALID_SOCKET) {
            fprintf(stderr, "[nfsclient] connect to mount port %u failed (WSAGetLastError=%d)\n",
                    mount_port, WSAGetLastError());
            Sleep(1500);
            continue;
        }

        xwriter_t args; memset(&args, 0, sizeof(args));
        w_str(&args, export_path);
        unsigned char *reply_buf = NULL;
        xreader_t r;
        int call_ok = rpc_call(ms, 1, PROG_MOUNT, 3, MOUNTPROC_MNT, &args, &reply_buf, &r);
        free(args.data);
        closesocket(ms);
        if (!call_ok) {
            fprintf(stderr, "[nfsclient] MNT rpc_call attempt %d timed out/failed, retrying...\n", attempt);
            Sleep(1500);
            continue;
        }

        mnt_stat = rd_u32(&r);
        if (mnt_stat == 0) rd_fh(&r, &root);
        int xdr_err = r.error;
        free(reply_buf);
        if (mnt_stat != 0 || xdr_err) {
            fprintf(stderr, "[nfsclient] MNT failed: mnt_stat=%u xdr_error=%d\n", mnt_stat, xdr_err);
            return NULL; // a real MNT3ERR_* is not going to fix itself on retry
        }
        ok = 1;
    }
    if (!ok) {
        fprintf(stderr, "[nfsclient] MNT never succeeded after retries\n");
        return NULL;
    }
    fprintf(stderr, "[nfsclient] MNT ok, root fh len=%u\n", root.len);

    SOCKET ns = connect_to(host, nfs_port);
    if (ns == INVALID_SOCKET) {
        fprintf(stderr, "[nfsclient] connect to nfs port %u failed (WSAGetLastError=%d)\n",
                nfs_port, WSAGetLastError());
        return NULL;
    }

    nfsclient_t *c = (nfsclient_t *)calloc(1, sizeof(nfsclient_t));
    c->nfs_sock = ns;
    c->root = root;
    c->xid = 1000;
    strncpy(c->host, host, sizeof(c->host) - 1);
    c->nfs_port = nfs_port;
    InitializeCriticalSection(&c->lock);
    return c;
}

void nfsc_close(nfsclient_t *c) {
    if (!c) return;
    closesocket(c->nfs_sock);
    DeleteCriticalSection(&c->lock);
    free(c);
}

int nfsc_getattr(nfsclient_t *c, const nfs_fh_t *fh, nfs_attr_t *out) {
    EnterCriticalSection(&c->lock);
    xwriter_t args; memset(&args, 0, sizeof(args));
    w_fh(&args, fh);
    unsigned char *rb = NULL; xreader_t r;
    int ok = nfs_call(c, NFSPROC_GETATTR, &args, &rb, &r);
    free(args.data);
    int ret = -5;
    if (ok) {
        unsigned int st = rd_u32(&r);
        if (st == NFS3_OK && !r.error) { rd_fattr3(&r, out); ret = 0; }
        else ret = map_errno(st);
        free(rb);
    }
    LeaveCriticalSection(&c->lock);
    return ret;
}

int nfsc_lookup_path(nfsclient_t *c, const char *path, nfs_fh_t *out_fh, nfs_attr_t *out_attr) {
    nfs_fh_t cur = c->root;
    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        *out_fh = cur;
        return nfsc_getattr(c, &cur, out_attr);
    }
    char buf[2048];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *save = NULL;
    char *tok = strtok_s(buf[0] == '/' ? buf + 1 : buf, "/", &save);
    nfs_attr_t attr; memset(&attr, 0, sizeof(attr));
    int have_attr = 0;
    while (tok) {
        EnterCriticalSection(&c->lock);
        xwriter_t args; memset(&args, 0, sizeof(args));
        w_fh(&args, &cur);
        w_str(&args, tok);
        unsigned char *rb = NULL; xreader_t r;
        int ok = nfs_call(c, NFSPROC_LOOKUP, &args, &rb, &r);
        free(args.data);
        if (!ok) { LeaveCriticalSection(&c->lock); return -5; }
        unsigned int st = rd_u32(&r);
        if (st != NFS3_OK) { free(rb); LeaveCriticalSection(&c->lock); return map_errno(st); }
        rd_fh(&r, &cur);
        have_attr = rd_post_op_attr(&r, &attr);
        free(rb);
        LeaveCriticalSection(&c->lock);
        tok = strtok_s(NULL, "/", &save);
    }
    *out_fh = cur;
    if (have_attr) { *out_attr = attr; return 0; }
    return nfsc_getattr(c, &cur, out_attr);
}

int nfsc_readdir(nfsclient_t *c, const nfs_fh_t *dir, void (*cb)(void *ctx, const char *name), void *ctx) {
    unsigned long long cookie = 0;
    for (;;) {
        EnterCriticalSection(&c->lock);
        xwriter_t args; memset(&args, 0, sizeof(args));
        w_fh(&args, dir);
        w_u64(&args, cookie);
        w_u64(&args, 0); // cookieverf
        w_u32(&args, 8192); // count
        unsigned char *rb = NULL; xreader_t r;
        int ok = nfs_call(c, NFSPROC_READDIR, &args, &rb, &r);
        free(args.data);
        if (!ok) { LeaveCriticalSection(&c->lock); return -5; }
        unsigned int st = rd_u32(&r);
        if (st != NFS3_OK) { free(rb); LeaveCriticalSection(&c->lock); return map_errno(st); }
        nfs_attr_t tmp; rd_post_op_attr(&r, &tmp);
        rd_u64(&r); // cookieverf
        int eof = 0;
        unsigned long long last_cookie = cookie;
        while (rd_bool(&r)) {
            rd_u64(&r); // fileid
            char name[256];
            unsigned int n = rd_bytes(&r, (unsigned char *)name, sizeof(name) - 1);
            name[n] = '\0';
            last_cookie = rd_u64(&r);
            if (cb && strcmp(name, ".") != 0 && strcmp(name, "..") != 0) cb(ctx, name);
        }
        eof = rd_bool(&r);
        free(rb);
        LeaveCriticalSection(&c->lock);
        if (r.error) return -5;
        if (eof || last_cookie == cookie) break;
        cookie = last_cookie;
    }
    return 0;
}

int nfsc_read(nfsclient_t *c, const nfs_fh_t *fh, unsigned long long offset,
              void *buf, unsigned int count, unsigned int *out_read) {
    EnterCriticalSection(&c->lock);
    xwriter_t args; memset(&args, 0, sizeof(args));
    w_fh(&args, fh);
    w_u64(&args, offset);
    w_u32(&args, count);
    unsigned char *rb = NULL; xreader_t r;
    int ok = nfs_call(c, NFSPROC_READ, &args, &rb, &r);
    free(args.data);
    int ret;
    if (!ok) { ret = -5; goto done; }
    {
        unsigned int st = rd_u32(&r);
        if (st != NFS3_OK) { ret = map_errno(st); free(rb); goto done; }
        nfs_attr_t tmp; rd_post_op_attr(&r, &tmp);
        unsigned int got = rd_u32(&r);
        rd_bool(&r); // eof
        unsigned int n = rd_bytes(&r, (unsigned char *)buf, count);
        *out_read = n < got ? n : got;
        ret = 0;
        free(rb);
    }
done:
    LeaveCriticalSection(&c->lock);
    return ret;
}

int nfsc_write(nfsclient_t *c, const nfs_fh_t *fh, unsigned long long offset,
               const void *buf, unsigned int count, unsigned int *out_written) {
    EnterCriticalSection(&c->lock);
    xwriter_t args; memset(&args, 0, sizeof(args));
    w_fh(&args, fh);
    w_u64(&args, offset);
    w_u32(&args, count);
    w_u32(&args, 2); // FILE_SYNC
    w_bytes(&args, buf, count);
    unsigned char *rb = NULL; xreader_t r;
    int ok = nfs_call(c, NFSPROC_WRITE, &args, &rb, &r);
    free(args.data);
    int ret;
    if (!ok) { ret = -5; goto done; }
    {
        unsigned int st = rd_u32(&r);
        rd_skip_wcc_data(&r);
        if (st != NFS3_OK) { ret = map_errno(st); free(rb); goto done; }
        *out_written = rd_u32(&r);
        ret = 0;
        free(rb);
    }
done:
    LeaveCriticalSection(&c->lock);
    return ret;
}

static int create_or_mkdir(nfsclient_t *c, unsigned int proc, const nfs_fh_t *dir, const char *name,
                            unsigned int mode, nfs_fh_t *out_fh, nfs_attr_t *out_attr) {
    EnterCriticalSection(&c->lock);
    xwriter_t args; memset(&args, 0, sizeof(args));
    w_fh(&args, dir);
    w_str(&args, name);
    if (proc == NFSPROC_CREATE) w_u32(&args, 0); // UNCHECKED
    // sattr3: mode set, rest unset
    w_u32(&args, 1); w_u32(&args, mode & 0777);
    w_u32(&args, 0); w_u32(&args, 0); w_u32(&args, 0); // uid,gid,size unset
    w_u32(&args, 0); w_u32(&args, 0); // atime,mtime DONT_CHANGE
    unsigned char *rb = NULL; xreader_t r;
    int ok = nfs_call(c, proc, &args, &rb, &r);
    free(args.data);
    int ret;
    if (!ok) { ret = -5; goto done; }
    {
        unsigned int st = rd_u32(&r);
        if (st != NFS3_OK) { rd_skip_wcc_data(&r); ret = map_errno(st); free(rb); goto done; }
        int have_fh = rd_post_op_fh3(&r, out_fh);
        int have_attr = rd_post_op_attr(&r, out_attr);
        rd_skip_wcc_data(&r);
        if (!have_fh || !have_attr) { ret = -5; free(rb); goto done; }
        ret = 0;
        free(rb);
    }
done:
    LeaveCriticalSection(&c->lock);
    return ret;
}

int nfsc_create(nfsclient_t *c, const nfs_fh_t *dir, const char *name, unsigned int mode,
                 nfs_fh_t *out_fh, nfs_attr_t *out_attr) {
    return create_or_mkdir(c, NFSPROC_CREATE, dir, name, mode, out_fh, out_attr);
}
int nfsc_mkdir(nfsclient_t *c, const nfs_fh_t *dir, const char *name, unsigned int mode,
               nfs_fh_t *out_fh, nfs_attr_t *out_attr) {
    return create_or_mkdir(c, NFSPROC_MKDIR, dir, name, mode, out_fh, out_attr);
}

static int remove_or_rmdir(nfsclient_t *c, unsigned int proc, const nfs_fh_t *dir, const char *name) {
    EnterCriticalSection(&c->lock);
    xwriter_t args; memset(&args, 0, sizeof(args));
    w_fh(&args, dir);
    w_str(&args, name);
    unsigned char *rb = NULL; xreader_t r;
    int ok = nfs_call(c, proc, &args, &rb, &r);
    free(args.data);
    int ret;
    if (!ok) { ret = -5; goto done; }
    {
        unsigned int st = rd_u32(&r);
        rd_skip_wcc_data(&r);
        ret = (st == NFS3_OK) ? 0 : map_errno(st);
        free(rb);
    }
done:
    LeaveCriticalSection(&c->lock);
    return ret;
}
int nfsc_remove(nfsclient_t *c, const nfs_fh_t *dir, const char *name) {
    return remove_or_rmdir(c, NFSPROC_REMOVE, dir, name);
}
int nfsc_rmdir(nfsclient_t *c, const nfs_fh_t *dir, const char *name) {
    return remove_or_rmdir(c, NFSPROC_RMDIR, dir, name);
}

int nfsc_rename(nfsclient_t *c, const nfs_fh_t *fromdir, const char *fromname,
                 const nfs_fh_t *todir, const char *toname) {
    EnterCriticalSection(&c->lock);
    xwriter_t args; memset(&args, 0, sizeof(args));
    w_fh(&args, fromdir);
    w_str(&args, fromname);
    w_fh(&args, todir);
    w_str(&args, toname);
    unsigned char *rb = NULL; xreader_t r;
    int ok = nfs_call(c, NFSPROC_RENAME, &args, &rb, &r);
    free(args.data);
    int ret;
    if (!ok) { ret = -5; goto done; }
    {
        unsigned int st = rd_u32(&r);
        rd_skip_wcc_data(&r);
        rd_skip_wcc_data(&r);
        ret = (st == NFS3_OK) ? 0 : map_errno(st);
        free(rb);
    }
done:
    LeaveCriticalSection(&c->lock);
    return ret;
}

int nfsc_setattr_size(nfsclient_t *c, const nfs_fh_t *fh, unsigned long long size) {
    EnterCriticalSection(&c->lock);
    xwriter_t args; memset(&args, 0, sizeof(args));
    w_fh(&args, fh);
    w_u32(&args, 0); // mode unset
    w_u32(&args, 0); // uid unset
    w_u32(&args, 0); // gid unset
    w_u32(&args, 1); w_u64(&args, size); // size set
    w_u32(&args, 0); w_u32(&args, 0); // atime,mtime DONT_CHANGE
    w_u32(&args, 0); // guard.check = false
    unsigned char *rb = NULL; xreader_t r;
    int ok = nfs_call(c, NFSPROC_SETATTR, &args, &rb, &r);
    free(args.data);
    int ret;
    if (!ok) { ret = -5; goto done; }
    {
        unsigned int st = rd_u32(&r);
        rd_skip_wcc_data(&r);
        ret = (st == NFS3_OK) ? 0 : map_errno(st);
        free(rb);
    }
done:
    LeaveCriticalSection(&c->lock);
    return ret;
}

int nfsc_statfs(nfsclient_t *c, const nfs_fh_t *fh, unsigned long long *total_bytes,
                 unsigned long long *free_bytes, unsigned long long *avail_bytes) {
    EnterCriticalSection(&c->lock);
    xwriter_t args; memset(&args, 0, sizeof(args));
    w_fh(&args, fh);
    unsigned char *rb = NULL; xreader_t r;
    int ok = nfs_call(c, NFSPROC_FSSTAT, &args, &rb, &r);
    free(args.data);
    int ret;
    if (!ok) { ret = -5; goto done2; }
    {
        unsigned int st = rd_u32(&r);
        nfs_attr_t tmp; rd_post_op_attr(&r, &tmp);
        if (st != NFS3_OK) { ret = map_errno(st); free(rb); goto done2; }
        *total_bytes = rd_u64(&r);
        *free_bytes = rd_u64(&r);
        *avail_bytes = rd_u64(&r);
        ret = r.error ? -5 : 0;
        free(rb);
    }
done2:
    LeaveCriticalSection(&c->lock);
    return ret;
}
