#include <windows.h>
#include <winfsp/winfsp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disk_manager.h"
#include "fs_memfs.h"

static void print_help(void) {
    printf("\n");
    printf("=====================================================================\n");
    printf("     vdisk - Temporary RAM & VRAM Disk CLI Utility (WinFsp)\n");
    printf("=====================================================================\n");
    printf("COMMANDS:\n");
    printf("  vdisk create ram  <SIZE> [DRIVE_LETTER]   Create high-speed RAM disk\n");
    printf("  vdisk create vram <SIZE> [DRIVE_LETTER]   Create GPU VRAM disk (NVIDIA CUDA)\n");
    printf("  vdisk remove <DRIVE_LETTER>               Remove and unmount a virtual disk\n");
    printf("  vdisk clear                               Remove ALL vdisk-managed disks\n");
    printf("  vdisk list                                List all active RAM & VRAM disks\n");
    printf("  vdisk status                              Show RAM and GPU VRAM hardware info\n");
    printf("  vdisk help                                Display this help menu\n\n");
    printf("EXAMPLES:\n");
    printf("  vdisk create ram 512M R:         Creates 512 MB RAM disk on drive R:\n");
    printf("  vdisk create vram 1024M V:       Creates 1 GB VRAM disk on drive V:\n");
    printf("  vdisk create ram 2G              Creates 2 GB RAM disk on a free drive letter\n");
    printf("  vdisk remove R:                  Removes drive R:\n");
    printf("  vdisk clear                      Removes every mounted vdisk\n");
    printf("=====================================================================\n\n");
}

int main(int argc, char *argv[]) {
    // winfsp-x64.dll lives in the WinFsp install dir (not on PATH); FspLoad
    // finds it via the registry and loads it so the delay-loaded WinFsp/FUSE
    // imports resolve. Must run before any WinFsp or FUSE call.
    if (FspLoad(0) != STATUS_SUCCESS) {
        fprintf(stderr, "[vdisk] Error: WinFsp is not installed. Get it from https://winfsp.dev\n");
        return 1;
    }

    if (argc < 2) {
        print_help();
        return 0;
    }

    const char *action = argv[1];

    // Internal: persistent WinFsp worker hosting one disk.
    // Usage: vdisk --fs-worker <ram|vram> <SIZE_MB> <DRIVE_LETTER>
    if (_stricmp(action, "--fs-worker") == 0) {
        if (argc < 5) return 1;
        int use_vram = (_stricmp(argv[2], "vram") == 0);
        unsigned long long size_mb = _strtoui64(argv[3], NULL, 10);
        char drive = argv[4][0];
        if (size_mb == 0 || !drive) return 1;
        return fs_run(use_vram, size_mb, drive);
    }

    disk_mgr_init();

    if (_stricmp(action, "help") == 0 || _stricmp(action, "-h") == 0 ||
        _stricmp(action, "--help") == 0 || _stricmp(action, "/?") == 0) {
        print_help();
        return 0;
    }

    if (_stricmp(action, "list") == 0 || _stricmp(action, "ls") == 0) {
        disk_mgr_list();
        return 0;
    }

    if (_stricmp(action, "status") == 0 || _stricmp(action, "info") == 0) {
        disk_mgr_status();
        return 0;
    }

    if (_stricmp(action, "clear") == 0 || _stricmp(action, "clean") == 0) {
        disk_mgr_clear();
        return 0;
    }

    if (_stricmp(action, "create") == 0 || _stricmp(action, "add") == 0) {
        if (argc < 4) {
            printf("Error: Missing parameters for 'create'.\nUsage: vdisk create <ram|vram> <SIZE> [DRIVE_LETTER]\n");
            return 1;
        }
        const char *type_arg = argv[2];
        const char *size_arg = argv[3];
        char drive_letter = (argc >= 5) ? argv[4][0] : 0;

        int use_vram;
        if (_stricmp(type_arg, "ram") == 0)       use_vram = 0;
        else if (_stricmp(type_arg, "vram") == 0) use_vram = 1;
        else {
            printf("Error: Unknown disk type '%s'. Use 'ram' or 'vram'.\n", type_arg);
            return 1;
        }

        size_t size_bytes = parse_size_string(size_arg);
        if (size_bytes == 0) {
            printf("Error: Invalid size '%s'. Examples: 512M, 1G, 2048M, 100MB\n", size_arg);
            return 1;
        }
        return disk_mgr_create(use_vram, size_bytes, drive_letter) ? 0 : 1;
    }

    if (_stricmp(action, "remove") == 0 || _stricmp(action, "rm") == 0 ||
        _stricmp(action, "delete") == 0 || _stricmp(action, "del") == 0) {
        if (argc < 3) {
            printf("Error: Missing drive letter for 'remove'.\nUsage: vdisk remove <DRIVE_LETTER>\n");
            return 1;
        }
        return disk_mgr_remove(argv[2][0]) ? 0 : 1;
    }

    printf("Unknown command '%s'. Run 'vdisk help' for usage.\n", action);
    print_help();
    return 1;
}
