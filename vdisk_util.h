#ifndef VDISK_UTIL_H
#define VDISK_UTIL_H

#include <windows.h>
#include <stddef.h>

// Fills 'out' with the directory containing the running executable
// (no trailing backslash). Returns FALSE on failure.
BOOL get_exe_dir(char *out, size_t out_sz);

// Fills 'out' with the vdisk data directory (%LOCALAPPDATA%\vdisk),
// creating it if needed. Falls back to the executable directory.
BOOL get_data_dir(char *out, size_t out_sz);

#endif // VDISK_UTIL_H
