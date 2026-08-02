#ifndef VHD_DRIVER_H
#define VHD_DRIVER_H

#include <windows.h>
#include <stddef.h>

int vhd_create_and_attach(const char *vhd_path, size_t size_bytes, char drive_letter);
int vhd_detach(const char *vhd_path, char drive_letter);

#endif // VHD_DRIVER_H
