#define _CRT_SECURE_NO_WARNINGS
#include "block_disk.h"
#include "vdisk_util.h"
#include <initguid.h>
#include <virtdisk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// A fixed VHD placed on the backing vdisk, attached via the native virtdisk API
// so Windows exposes it as a real \\.\PhysicalDrive. We use the API (not
// diskpart) because diskpart cannot reliably re-select a VHD that lives on a
// WinFsp volume for detach, and because this machine's Storage WMI is broken.

#define VHD_NAME "vdisk_block.vhd"

static const char *state_path(void) {
    static char path[MAX_PATH] = {0};
    if (path[0]) return path;
    char dir[MAX_PATH];
    if (get_data_dir(dir, sizeof(dir))) snprintf(path, sizeof(path), "%s\\vdisk_blocks.txt", dir);
    else strcpy_s(path, sizeof(path), "vdisk_blocks.txt");
    return path;
}

static void vhd_path_for(char L, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%c:\\%s", L, VHD_NAME);
}

static void to_wide(const char *s, wchar_t *w, int wcount) {
    MultiByteToWideChar(CP_ACP, 0, s, -1, w, wcount);
}

static void storage_type(VIRTUAL_STORAGE_TYPE *st) {
    st->DeviceId = VIRTUAL_STORAGE_TYPE_DEVICE_VHD;
    st->VendorId = VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT;
}

// --- tiny state file: "<LETTER> <SIZE_MB>" per attached block disk -----------

int block_disk_is_backing(char L) {
    L = (char)toupper((unsigned char)L);
    FILE *f = fopen(state_path(), "r");
    if (!f) return 0;
    char line[128];
    int found = 0;
    while (fgets(line, sizeof(line), f))
        if (toupper((unsigned char)line[0]) == L) { found = 1; break; }
    fclose(f);
    return found;
}

static void state_add(char L, unsigned long long size_mb) {
    if (block_disk_is_backing(L)) return;
    FILE *f = fopen(state_path(), "a");
    if (!f) return;
    fprintf(f, "%c %llu\n", (char)toupper((unsigned char)L), size_mb);
    fclose(f);
}

static void state_remove(char L) {
    L = (char)toupper((unsigned char)L);
    FILE *f = fopen(state_path(), "r");
    if (!f) return;
    char lines[26][128];
    int n = 0;
    char line[128];
    while (n < 26 && fgets(line, sizeof(line), f))
        if (toupper((unsigned char)line[0]) != L) strcpy_s(lines[n++], sizeof(lines[0]), line);
    fclose(f);
    f = fopen(state_path(), "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fputs(lines[i], f);
    fclose(f);
}

// --- public API -----------------------------------------------------------

int block_disk_attach(char L, unsigned long long size_mb, int do_format) {
    L = (char)toupper((unsigned char)L);
    if (L < 'A' || L > 'Z') { printf("[vdisk] Invalid drive letter.\n"); return 0; }

    if (!(GetLogicalDrives() & (1u << (L - 'A')))) {
        printf("[vdisk] No vdisk mounted at %c:. Create one first, e.g. 'vdisk create ram 1G %c:'.\n", L, L);
        return 0;
    }

    char vhd[MAX_PATH];
    vhd_path_for(L, vhd, sizeof(vhd));
    if (GetFileAttributesA(vhd) != INVALID_FILE_ATTRIBUTES || block_disk_is_backing(L)) {
        printf("[vdisk] A block disk already exists on %c:. Remove it first: 'vdisk disk remove %c'.\n", L, L);
        return 0;
    }

    if (do_format) {
        printf("[vdisk] Note: auto-format is disabled for safety; attaching a RAW disk instead.\n");
    }

    wchar_t wpath[MAX_PATH];
    to_wide(vhd, wpath, MAX_PATH);
    VIRTUAL_STORAGE_TYPE st;
    storage_type(&st);

    printf("[vdisk] Creating a %llu MB fixed VHD on %c: and attaching it as a real physical disk...\n",
           size_mb, L);

    CREATE_VIRTUAL_DISK_PARAMETERS cp;
    memset(&cp, 0, sizeof(cp));
    cp.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    cp.Version2.MaximumSize = size_mb * 1024ull * 1024ull;
    cp.Version2.SectorSizeInBytes = 512;

    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD res = CreateVirtualDisk(&st, wpath, VIRTUAL_DISK_ACCESS_NONE, NULL,
                                  CREATE_VIRTUAL_DISK_FLAG_FULL_PHYSICAL_ALLOCATION, 0,
                                  &cp, NULL, &h);
    if (res != ERROR_SUCCESS) {
        printf("[vdisk] CreateVirtualDisk failed (error %lu).\n", res);
        DeleteFileA(vhd);
        return 0;
    }

    ATTACH_VIRTUAL_DISK_PARAMETERS ap;
    memset(&ap, 0, sizeof(ap));
    ap.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    // PERMANENT_LIFETIME: stays attached after this process exits (until detach).
    res = AttachVirtualDisk(h, NULL, ATTACH_VIRTUAL_DISK_FLAG_PERMANENT_LIFETIME, 0, &ap, NULL);
    CloseHandle(h);
    if (res != ERROR_SUCCESS) {
        printf("[vdisk] AttachVirtualDisk failed (error %lu).\n", res);
        DeleteFileA(vhd);
        return 0;
    }

    state_add(L, size_mb);
    printf("[vdisk] SUCCESS! A real RAW physical disk (RAM/VRAM-backed via %c:) is now attached.\n", L);
    printf("[vdisk] It appears in Disk Management / 'diskpart list disk', ready to partition/format/test.\n");
    printf("[vdisk] When done: 'vdisk disk remove %c'  (detaches, then you can 'vdisk remove %c').\n", L, L);
    return 1;
}

int block_disk_detach(char L) {
    L = (char)toupper((unsigned char)L);
    if (L < 'A' || L > 'Z') return 0;

    char vhd[MAX_PATH];
    vhd_path_for(L, vhd, sizeof(vhd));

    int had_state = block_disk_is_backing(L);
    if (GetFileAttributesA(vhd) == INVALID_FILE_ATTRIBUTES && !had_state) {
        return 1; // nothing attached
    }

    wchar_t wpath[MAX_PATH];
    to_wide(vhd, wpath, MAX_PATH);
    VIRTUAL_STORAGE_TYPE st;
    storage_type(&st);

    OPEN_VIRTUAL_DISK_PARAMETERS op;
    memset(&op, 0, sizeof(op));
    op.Version = OPEN_VIRTUAL_DISK_VERSION_1;

    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD res = OpenVirtualDisk(&st, wpath, VIRTUAL_DISK_ACCESS_DETACH,
                                OPEN_VIRTUAL_DISK_FLAG_NONE, NULL, &h);
    if (res == ERROR_SUCCESS) {
        res = DetachVirtualDisk(h, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        CloseHandle(h);
        if (res != ERROR_SUCCESS && res != ERROR_NOT_READY)
            printf("[vdisk] DetachVirtualDisk warning (error %lu).\n", res);
    } else {
        printf("[vdisk] OpenVirtualDisk warning (error %lu).\n", res);
    }

    DeleteFileA(vhd);
    state_remove(L);
    printf("[vdisk] Block disk on %c: detached.\n", L);
    return 1;
}

void block_disk_list(void) {
    printf("\n=== Physical block disks (RAM/VRAM-backed VHDs) ===\n");
    printf("%-10s %-12s %-30s\n", "Backing", "Size", "VHD");
    printf("--------------------------------------------------------\n");
    FILE *f = fopen(state_path(), "r");
    int count = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char L;
            unsigned long long mb;
            if (sscanf_s(line, "%c %llu", &L, (unsigned)1, &mb) >= 2) {
                char vhd[MAX_PATH];
                vhd_path_for((char)toupper((unsigned char)L), vhd, sizeof(vhd));
                char sz[32];
                snprintf(sz, sizeof(sz), "%llu MB", mb);
                printf("%c:         %-12s %-30s\n", (char)toupper((unsigned char)L), sz, vhd);
                count++;
            }
        }
        fclose(f);
    }
    if (!count) printf("No physical block disks active.\n");
    printf("--------------------------------------------------------\n\n");
}
