// WinFsp/FUSE filesystem module that relays every operation over NFSv3 to a
// real Linux guest's exported root (see nfsclient.c) -- the guest's own
// ext4 kernel driver stays the sole owner of the bytes; this is purely a
// network client, so it's safe to run at the same time the VM is live.

#define FUSE_USE_VERSION 29

#include <fuse/fuse.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include "fs_nfsclient.h"
#include "nfsclient.h"

#define NF3REG 1
#define NF3DIR 2
#define NF3LNK 5

static nfsclient_t *g_nc;

static void fill_stat(const nfs_attr_t *a, struct fuse_stat *st) {
    memset(st, 0, sizeof(*st));
    unsigned int type_bits = a->type == NF3DIR ? 0040000 : a->type == NF3LNK ? 0120000 : 0100000;
    st->st_mode = type_bits | (a->mode & 07777);
    st->st_nlink = a->nlink ? a->nlink : 1;
    st->st_size = (fuse_off_t)a->size;
    st->st_atim.tv_sec = a->atime;
    st->st_mtim.tv_sec = a->mtime;
    st->st_ctim.tv_sec = a->ctime;
    st->st_birthtim.tv_sec = a->ctime;
    st->st_blksize = 4096;
    st->st_blocks = (fuse_blkcnt_t)((a->size + 511) / 512);
}

static int op_getattr(const char *path, struct fuse_stat *st) {
    nfs_fh_t fh; nfs_attr_t a;
    int rc = nfsc_lookup_path(g_nc, path, &fh, &a);
    if (rc) return rc;
    fill_stat(&a, st);
    return 0;
}

static int op_fgetattr(const char *path, struct fuse_stat *st, struct fuse_file_info *fi) {
    (void)fi;
    return op_getattr(path, st);
}

typedef struct { void *buf; fuse_fill_dir_t filler; } readdir_ctx_t;
static void readdir_cb(void *ctx0, const char *name) {
    readdir_ctx_t *ctx = (readdir_ctx_t *)ctx0;
    ctx->filler(ctx->buf, name, NULL, 0);
}

static int op_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      fuse_off_t off, struct fuse_file_info *fi) {
    (void)off; (void)fi;
    nfs_fh_t fh; nfs_attr_t a;
    int rc = nfsc_lookup_path(g_nc, path, &fh, &a);
    if (rc) return rc;
    filler(buf, ".", NULL, 0);
    filler(buf, "..", NULL, 0);
    readdir_ctx_t ctx = { buf, filler };
    return nfsc_readdir(g_nc, &fh, readdir_cb, &ctx);
}

static int split_parent(const char *path, char *parent_out, size_t parent_cap, const char **leaf_out) {
    const char *slash = strrchr(path, '/');
    if (!slash) return -EINVAL;
    size_t plen = (size_t)(slash - path);
    if (plen == 0) { parent_out[0] = '/'; parent_out[1] = '\0'; }
    else {
        if (plen >= parent_cap) return -ENAMETOOLONG;
        memcpy(parent_out, path, plen);
        parent_out[plen] = '\0';
    }
    *leaf_out = slash + 1;
    return 0;
}

static int op_mkdir(const char *path, fuse_mode_t mode) {
    char parent[2048]; const char *leaf;
    int r = split_parent(path, parent, sizeof(parent), &leaf);
    if (r) return r;
    nfs_fh_t dir; nfs_attr_t da;
    r = nfsc_lookup_path(g_nc, parent, &dir, &da);
    if (r) return r;
    nfs_fh_t out_fh; nfs_attr_t out_attr;
    return nfsc_mkdir(g_nc, &dir, leaf, mode, &out_fh, &out_attr);
}

static int op_rmdir(const char *path) {
    char parent[2048]; const char *leaf;
    int r = split_parent(path, parent, sizeof(parent), &leaf);
    if (r) return r;
    nfs_fh_t dir; nfs_attr_t da;
    r = nfsc_lookup_path(g_nc, parent, &dir, &da);
    if (r) return r;
    return nfsc_rmdir(g_nc, &dir, leaf);
}

static int op_create(const char *path, fuse_mode_t mode, struct fuse_file_info *fi) {
    (void)fi;
    char parent[2048]; const char *leaf;
    int r = split_parent(path, parent, sizeof(parent), &leaf);
    if (r) return r;
    nfs_fh_t dir; nfs_attr_t da;
    r = nfsc_lookup_path(g_nc, parent, &dir, &da);
    if (r) return r;
    nfs_fh_t out_fh; nfs_attr_t out_attr;
    return nfsc_create(g_nc, &dir, leaf, mode, &out_fh, &out_attr);
}

static int op_open(const char *path, struct fuse_file_info *fi) {
    (void)fi;
    nfs_fh_t fh; nfs_attr_t a;
    return nfsc_lookup_path(g_nc, path, &fh, &a);
}

static int op_read(const char *path, char *buf, size_t size, fuse_off_t off,
                   struct fuse_file_info *fi) {
    (void)fi;
    nfs_fh_t fh; nfs_attr_t a;
    int r = nfsc_lookup_path(g_nc, path, &fh, &a);
    if (r) return r;
    unsigned int total = 0;
    while (total < size) {
        unsigned int want = (unsigned int)(size - total);
        if (want > 65536) want = 65536;
        unsigned int got = 0;
        r = nfsc_read(g_nc, &fh, (unsigned long long)off + total, buf + total, want, &got);
        if (r) return total ? (int)total : r;
        if (got == 0) break;
        total += got;
        if (got < want) break; // short read == EOF
    }
    return (int)total;
}

static int op_write(const char *path, const char *buf, size_t size, fuse_off_t off,
                    struct fuse_file_info *fi) {
    (void)fi;
    nfs_fh_t fh; nfs_attr_t a;
    int r = nfsc_lookup_path(g_nc, path, &fh, &a);
    if (r) return r;
    unsigned int total = 0;
    while (total < size) {
        unsigned int want = (unsigned int)(size - total);
        if (want > 65536) want = 65536;
        unsigned int wrote = 0;
        r = nfsc_write(g_nc, &fh, (unsigned long long)off + total, buf + total, want, &wrote);
        if (r) return total ? (int)total : r;
        if (wrote == 0) break;
        total += wrote;
    }
    return (int)total;
}

static int op_unlink(const char *path) {
    char parent[2048]; const char *leaf;
    int r = split_parent(path, parent, sizeof(parent), &leaf);
    if (r) return r;
    nfs_fh_t dir; nfs_attr_t da;
    r = nfsc_lookup_path(g_nc, parent, &dir, &da);
    if (r) return r;
    return nfsc_remove(g_nc, &dir, leaf);
}

static int op_rename(const char *from, const char *to) {
    char fparent[2048]; const char *fleaf;
    char tparent[2048]; const char *tleaf;
    int r = split_parent(from, fparent, sizeof(fparent), &fleaf);
    if (r) return r;
    r = split_parent(to, tparent, sizeof(tparent), &tleaf);
    if (r) return r;
    nfs_fh_t fdir, tdir; nfs_attr_t da;
    r = nfsc_lookup_path(g_nc, fparent, &fdir, &da);
    if (r) return r;
    r = nfsc_lookup_path(g_nc, tparent, &tdir, &da);
    if (r) return r;
    return nfsc_rename(g_nc, &fdir, fleaf, &tdir, tleaf);
}

static int op_truncate(const char *path, fuse_off_t size) {
    nfs_fh_t fh; nfs_attr_t a;
    int r = nfsc_lookup_path(g_nc, path, &fh, &a);
    if (r) return r;
    return nfsc_setattr_size(g_nc, &fh, (unsigned long long)size);
}

static int op_ftruncate(const char *path, fuse_off_t size, struct fuse_file_info *fi) {
    (void)fi;
    return op_truncate(path, size);
}

static int op_chmod(const char *path, fuse_mode_t mode) {
    (void)path; (void)mode;
    return 0; // not wired to SETATTR mode field in v1 -- single-user local VM
}

static int op_chown(const char *path, fuse_uid_t uid, fuse_gid_t gid) {
    (void)path; (void)uid; (void)gid;
    return 0;
}

static int op_utimens(const char *path, const struct fuse_timespec tv[2]) {
    (void)path; (void)tv;
    return 0;
}

static int op_statfs(const char *path, struct fuse_statvfs *st) {
    (void)path;
    memset(st, 0, sizeof(*st));
    st->f_bsize = 4096;
    st->f_frsize = 4096;
    nfs_fh_t root = nfsc_root_fh(g_nc);
    unsigned long long total = 0, free_ = 0, avail = 0;
    int rc = nfsc_statfs(g_nc, &root, &total, &free_, &avail);
    if (rc) return rc;
    st->f_blocks = (fuse_fsblkcnt_t)(total / 4096);
    st->f_bfree  = (fuse_fsblkcnt_t)(free_ / 4096);
    st->f_bavail = (fuse_fsblkcnt_t)(avail / 4096);
    st->f_files = 1000000;
    st->f_ffree = 1000000;
    st->f_favail = 1000000;
    st->f_namemax = 255;
    return 0;
}

static int op_access(const char *path, int mask) {
    (void)mask;
    nfs_fh_t fh; nfs_attr_t a;
    return nfsc_lookup_path(g_nc, path, &fh, &a);
}

static int op_flush(const char *path, struct fuse_file_info *fi)  { (void)path; (void)fi; return 0; }
static int op_release(const char *path, struct fuse_file_info *fi){ (void)path; (void)fi; return 0; }
static int op_fsync(const char *path, int d, struct fuse_file_info *fi) { (void)path; (void)d; (void)fi; return 0; }

int fs_nfs_run(const char *host, unsigned short nfs_port, unsigned short mount_port,
               const char *export_path, char drive_letter, const char *fs_name) {
    fprintf(stderr, "[nfs-worker] connecting to %s nfs=%u mount=%u export='%s'\n",
            host, nfs_port, mount_port, export_path);
    fflush(stderr);
    g_nc = nfsc_connect(host, nfs_port, mount_port, export_path);
    if (!g_nc) {
        fprintf(stderr, "[nfs-worker] nfsc_connect failed\n");
        fflush(stderr);
        return 2;
    }
    fprintf(stderr, "[nfs-worker] connected, mounting %c:\n", drive_letter);
    fflush(stderr);

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
    char a1[] = "-f";
    char a2[] = "-o";
    char *argv[] = { a0, a1, a2, opts, mountpoint, NULL };
    int argc = 5;

    int rc = fuse_main_real(argc, argv, &ops, sizeof(ops), NULL);
    nfsc_close(g_nc);
    g_nc = NULL;
    return rc;
}
