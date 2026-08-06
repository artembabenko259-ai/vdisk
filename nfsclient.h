#ifndef NFSCLIENT_H
#define NFSCLIENT_H

// Minimal NFSv3 + MOUNTv3 client over TCP, talking to a real unfsd/nfsd
// running inside the Alpine/Debian guest (reachable via a QEMU hostfwd port
// mapping). The guest's real ext4 kernel driver is the sole owner of the
// filesystem bytes -- this client only ever speaks the network protocol, it
// never touches the disk image directly, so there is no dual-writer risk.
//
// No portmapper/rpcbind involved on either side: both the port for MOUNT and
// the port for NFS are passed in explicitly (matching how the guest's unfsd
// is started with fixed -n/-m ports).

typedef struct nfsclient nfsclient_t;

typedef struct {
    unsigned char data[64];
    unsigned int len;
} nfs_fh_t;

typedef struct {
    unsigned int type; // 1=regular file, 2=directory, 5=symlink, etc. (NF3xxx)
    unsigned int mode;
    unsigned int nlink;
    unsigned int uid, gid;
    unsigned long long size;
    unsigned long long used;
    unsigned long long fileid;
    long atime, mtime, ctime;
} nfs_attr_t;

// Connects to host:nfs_port / host:mount_port and MNTs 'export_path'.
// Returns NULL on any connection/mount failure.
nfsclient_t *nfsc_connect(const char *host, unsigned short nfs_port,
                           unsigned short mount_port, const char *export_path);
void nfsc_close(nfsclient_t *c);

nfs_fh_t nfsc_root_fh(nfsclient_t *c);

// Resolves a '/'-separated path (relative to the export root) by walking
// LOOKUP one component at a time. path="" or "/" resolves to the root itself.
// Returns 0 on success, a negative errno-style code on failure (-ENOENT etc).
int nfsc_lookup_path(nfsclient_t *c, const char *path, nfs_fh_t *out_fh, nfs_attr_t *out_attr);

int nfsc_getattr(nfsclient_t *c, const nfs_fh_t *fh, nfs_attr_t *out);
int nfsc_readdir(nfsclient_t *c, const nfs_fh_t *dir,
                  void (*cb)(void *ctx, const char *name), void *ctx);
int nfsc_read(nfsclient_t *c, const nfs_fh_t *fh, unsigned long long offset,
              void *buf, unsigned int count, unsigned int *out_read);
int nfsc_write(nfsclient_t *c, const nfs_fh_t *fh, unsigned long long offset,
               const void *buf, unsigned int count, unsigned int *out_written);
int nfsc_create(nfsclient_t *c, const nfs_fh_t *dir, const char *name, unsigned int mode,
                 nfs_fh_t *out_fh, nfs_attr_t *out_attr);
int nfsc_mkdir(nfsclient_t *c, const nfs_fh_t *dir, const char *name, unsigned int mode,
               nfs_fh_t *out_fh, nfs_attr_t *out_attr);
int nfsc_remove(nfsclient_t *c, const nfs_fh_t *dir, const char *name);
int nfsc_rmdir(nfsclient_t *c, const nfs_fh_t *dir, const char *name);
int nfsc_rename(nfsclient_t *c, const nfs_fh_t *fromdir, const char *fromname,
                 const nfs_fh_t *todir, const char *toname);
int nfsc_setattr_size(nfsclient_t *c, const nfs_fh_t *fh, unsigned long long size);

// Real filesystem usage from the guest (NFSPROC_FSSTAT), not a fake constant.
int nfsc_statfs(nfsclient_t *c, const nfs_fh_t *fh, unsigned long long *total_bytes,
                 unsigned long long *free_bytes, unsigned long long *avail_bytes);

#endif // NFSCLIENT_H
