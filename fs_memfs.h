#ifndef FS_MEMFS_H
#define FS_MEMFS_H

#include <stddef.h>

// Runs an in-memory WinFsp/FUSE filesystem mounted at 'drive_letter' with a
// soft capacity of 'size_mb' megabytes. File contents are stored in system RAM
// when 'use_vram' is 0, or in GPU VRAM (via CUDA) when 'use_vram' is non-zero.
// Blocks until the filesystem is unmounted or the process is terminated.
// Returns 0 on clean exit, non-zero on startup failure.
int fs_run(int use_vram, unsigned long long size_mb, char drive_letter);

#endif // FS_MEMFS_H
