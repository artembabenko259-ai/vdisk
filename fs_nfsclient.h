#ifndef FS_NFSCLIENT_H
#define FS_NFSCLIENT_H

// Mounts a Windows drive letter backed by a live NFSv3 export from a Linux
// guest (see nfsclient.h) via the WinFsp FUSE compatibility layer -- same
// approach as fs_memfs.c, but every operation is relayed over NFS to the
// guest's real ext4 instead of an in-memory tree. Blocks until unmounted.
// Returns 0 on clean exit, non-zero on failure to connect/mount.
int fs_nfs_run(const char *host, unsigned short nfs_port, unsigned short mount_port,
               const char *export_path, char drive_letter, const char *fs_name);

#endif // FS_NFSCLIENT_H
