#include "vdisk_util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

BOOL get_exe_dir(char *out, size_t out_sz) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return FALSE;

    char *slash = strrchr(path, '\\');
    if (!slash) return FALSE;
    *slash = '\0';

    if (strlen(path) >= out_sz) return FALSE;
    strcpy_s(out, out_sz, path);
    return TRUE;
}

BOOL get_data_dir(char *out, size_t out_sz) {
    const char *base = getenv("LOCALAPPDATA");
    if (base && *base) {
        char dir[MAX_PATH];
        if (_snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\vdisk", base) > 0) {
            // Succeeds or already exists; either way the path is usable.
            CreateDirectoryA(dir, NULL);
            if (GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES && strlen(dir) < out_sz) {
                strcpy_s(out, out_sz, dir);
                return TRUE;
            }
        }
    }
    return get_exe_dir(out, out_sz);
}
