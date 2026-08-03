#define _CRT_SECURE_NO_WARNINGS
#include "block_disk.h"
#include "disk_manager.h"
#include "vdisk_util.h"
#include <initguid.h>
#include <virtdisk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// A "block disk" is a dynamic VHD placed on a backing vdisk (RAM/VRAM), attached
// via the native virtdisk API so Windows exposes it as a real \\.\PhysicalDrive.
// We use the API (not diskpart) because diskpart cannot reliably re-select a VHD
// living on a WinFsp volume for detach, and because this machine's Storage WMI
// is broken. Dynamic VHDs are created instantly and grow in RAM as written.

#define VHD_NAME "vdisk_block.vhd"
#define BACKING_HEADROOM_MB 16   // extra room on the backing vdisk for VHD metadata

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

// --- state file: "<LETTER> <SIZE_MB> <AUTO>" per attached block disk ----------
// AUTO=1 means we also created the backing vdisk, so remove tears it down too.

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

static int state_auto(char L) {
    L = (char)toupper((unsigned char)L);
    FILE *f = fopen(state_path(), "r");
    if (!f) return 0;
    char line[128];
    int is_auto = 0;
    while (fgets(line, sizeof(line), f)) {
        char c; unsigned long long mb; int a = 0;
        if (sscanf_s(line, "%c %llu %d", &c, (unsigned)1, &mb, &a) >= 2 &&
            toupper((unsigned char)c) == L) { is_auto = a; break; }
    }
    fclose(f);
    return is_auto;
}

static void state_add(char L, unsigned long long size_mb, int is_auto) {
    if (block_disk_is_backing(L)) return;
    FILE *f = fopen(state_path(), "a");
    if (!f) return;
    fprintf(f, "%c %llu %d\n", (char)toupper((unsigned char)L), size_mb, is_auto ? 1 : 0);
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

// --- virtdisk attach/detach ------------------------------------------------

// Creates a dynamic VHD on 'vhd' and attaches it as a physical disk. 1 on OK.
static int attach_vhd(const char *vhd, unsigned long long size_mb) {
    wchar_t wpath[MAX_PATH];
    to_wide(vhd, wpath, MAX_PATH);
    VIRTUAL_STORAGE_TYPE st;
    storage_type(&st);

    CREATE_VIRTUAL_DISK_PARAMETERS cp;
    memset(&cp, 0, sizeof(cp));
    cp.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    cp.Version2.MaximumSize = size_mb * 1024ull * 1024ull;
    cp.Version2.SectorSizeInBytes = 512;

    HANDLE h = INVALID_HANDLE_VALUE;
    // FLAG_NONE = dynamic (instant create, grows on write). Fast.
    DWORD res = CreateVirtualDisk(&st, wpath, VIRTUAL_DISK_ACCESS_NONE, NULL,
                                  CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &cp, NULL, &h);
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
    return 1;
}

static void detach_vhd(const char *vhd) {
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
}

// --- public API -----------------------------------------------------------

// Explicit: attach a block disk onto an already-mounted vdisk (backing kept).
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
    if (do_format)
        printf("[vdisk] Note: auto-format is disabled for safety; attaching a RAW disk instead.\n");

    printf("[vdisk] Creating a %llu MB dynamic VHD on %c: and attaching it as a real physical disk...\n",
           size_mb, L);
    if (!attach_vhd(vhd, size_mb)) return 0;

    state_add(L, size_mb, 0);
    printf("[vdisk] SUCCESS! A real RAW physical disk (backed by the vdisk on %c:) is now attached.\n", L);
    printf("[vdisk] It appears in Disk Management / 'diskpart list disk', ready to partition/format/test.\n");
    printf("[vdisk] When done: 'vdisk disk remove %c' (detaches; the %c: vdisk stays).\n", L, L);
    return 1;
}

// Convenience: create the backing vdisk AND attach the physical disk in one go.
int block_disk_create_auto(int use_vram, unsigned long long size_mb, char letter) {
    if (size_mb < 3) { printf("[vdisk] Minimum block-disk size is 3 MB.\n"); return 0; }

    if (!letter) letter = find_free_drive_letter();
    letter = (char)toupper((unsigned char)letter);
    if (letter < 'A' || letter > 'Z') { printf("[vdisk] Invalid drive letter.\n"); return 0; }
    if (GetLogicalDrives() & (1u << (letter - 'A'))) {
        printf("[vdisk] Drive %c: is already in use; choose a free letter.\n", letter);
        return 0;
    }

    unsigned long long backing_mb = size_mb + BACKING_HEADROOM_MB;
    printf("[vdisk] Preparing %s backing store on %c: (%llu MB)...\n",
           use_vram ? "VRAM" : "RAM", letter, backing_mb);
    if (!disk_mgr_create(use_vram, (size_t)(backing_mb * 1024ull * 1024ull), letter, "NTFS")) {
        printf("[vdisk] Failed to create the backing store.\n");
        return 0;
    }

    char vhd[MAX_PATH];
    vhd_path_for(letter, vhd, sizeof(vhd));
    printf("[vdisk] Creating a %llu MB dynamic VHD and attaching it as a real physical disk...\n", size_mb);
    if (!attach_vhd(vhd, size_mb)) {
        disk_mgr_remove(letter); // roll back the backing store
        return 0;
    }

    state_add(letter, size_mb, 1);
    printf("[vdisk] SUCCESS! A real RAW physical disk (%llu MB, %s-backed) is attached.\n",
           size_mb, use_vram ? "VRAM" : "RAM");
    printf("[vdisk] It appears in Disk Management / 'diskpart list disk', ready to partition/format/test.\n");
    printf("[vdisk] Remove everything (disk + backing) with a single: 'vdisk disk remove %c'.\n", letter);
    return 1;
}

int block_disk_detach(char L) {
    L = (char)toupper((unsigned char)L);
    if (L < 'A' || L > 'Z') return 0;

    char vhd[MAX_PATH];
    vhd_path_for(L, vhd, sizeof(vhd));

    int is_auto = state_auto(L);
    if (GetFileAttributesA(vhd) == INVALID_FILE_ATTRIBUTES && !block_disk_is_backing(L)) {
        printf("[vdisk] No block disk on %c:.\n", L);
        return 0;
    }

    detach_vhd(vhd);
    DeleteFileA(vhd);
    state_remove(L);
    printf("[vdisk] Physical disk on %c: detached.\n", L);

    if (is_auto) {
        // We created the backing store, so tear it down too (state cleared above,
        // so disk_mgr_remove no longer refuses).
        disk_mgr_remove(L);
    }
    return 1;
}

void block_disk_list(void) {
    printf("\n=== Physical block disks (RAM/VRAM-backed VHDs) ===\n");
    printf("%-10s %-12s %-8s %-26s\n", "Backing", "Size", "Owned", "VHD");
    printf("------------------------------------------------------------\n");
    FILE *f = fopen(state_path(), "r");
    int count = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            char L; unsigned long long mb; int a = 0;
            if (sscanf_s(line, "%c %llu %d", &L, (unsigned)1, &mb, &a) >= 2) {
                char vhd[MAX_PATH];
                vhd_path_for((char)toupper((unsigned char)L), vhd, sizeof(vhd));
                char sz[32];
                snprintf(sz, sizeof(sz), "%llu MB", mb);
                printf("%c:         %-12s %-8s %-26s\n",
                       (char)toupper((unsigned char)L), sz, a ? "auto" : "manual", vhd);
                count++;
            }
        }
        fclose(f);
    }
    if (!count) printf("No physical block disks active.\n");
    printf("------------------------------------------------------------\n\n");
}
