#ifndef BLOCK_DISK_H
#define BLOCK_DISK_H

#include <windows.h>

// A "block disk" is a native VHD whose file lives on a vdisk (RAM/VRAM), then
// attached so Windows exposes it as a REAL physical disk (\\.\PhysicalDriveN,
// visible in Disk Management / diskpart, partitionable). This is what lets you
// safely test destructive disk tools against RAM/VRAM-backed storage.
//
// All operations require Administrator rights and drive the built-in diskpart.

// Creates a fixed VHD of 'size_mb' MB on the vdisk mounted at 'backing_letter'
// and attaches it as a physical disk. If 'do_format' is set, it is also
// initialized (MBR), partitioned and NTFS-formatted with a drive letter.
// Returns 1 on success.
int block_disk_attach(char backing_letter, unsigned long long size_mb, int do_format);

// Detaches the block disk backed by 'backing_letter' (its VHD is on that vdisk).
// Safe to call even if nothing is attached. Returns 1 on success.
int block_disk_detach(char backing_letter);

// Lists tracked block disks.
void block_disk_list(void);

// Returns non-zero if the vdisk at 'backing_letter' currently backs an attached
// physical block disk (used to prevent orphaning it on remove/clear).
int block_disk_is_backing(char backing_letter);

#endif // BLOCK_DISK_H
