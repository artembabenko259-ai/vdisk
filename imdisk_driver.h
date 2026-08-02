#ifndef IMDISK_DRIVER_H
#define IMDISK_DRIVER_H

#include <windows.h>
#include <stddef.h>

int imdisk_is_installed();
int imdisk_ensure_installed();
int imdisk_create_ram_disk(size_t size_bytes, char drive_letter, const char *fs_type);
int imdisk_create_vram_proxy(size_t size_bytes, char drive_letter, const char *pipe_name, const char *fs_type);
int imdisk_remove_disk(char drive_letter);

#endif // IMDISK_DRIVER_H
