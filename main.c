#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "disk_manager.h"
#include "vram_allocator.h"

BOOL IsUserAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

int RelaunchAsAdmin(int argc, char *argv[]) {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);

    char args[2048] = "";
    for (int i = 1; i < argc; i++) {
        strcat_s(args, sizeof(args), "\"");
        strcat_s(args, sizeof(args), argv[i]);
        strcat_s(args, sizeof(args), "\" ");
    }

    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.cbSize = sizeof(sei);
    sei.lpVerb = "runas";
    sei.lpFile = exePath;
    sei.lpParameters = args;
    sei.nShow = SW_NORMAL;

    printf("[vdisk] Mandatory Administrator privileges required.\n");
    printf("[vdisk] Requesting UAC elevation...\n");

    if (!ShellExecuteExA(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) {
            printf("[vdisk] Error: Operation canceled by user (UAC prompt declined).\n");
        } else {
            printf("[vdisk] Error: Failed to elevate privileges (Error code: %lu).\n", err);
        }
        return 0;
    }
    return 1;
}

void print_help() {
    printf("\n");
    printf("=====================================================================\n");
    printf("     vdisk - Temporary RAM & VRAM Disk CLI Utility (C Language)\n");
    printf("=====================================================================\n");
    printf("REQUIRED: Run in Administrator Command Prompt/PowerShell or allow UAC.\n\n");
    printf("COMMANDS:\n");
    printf("  vdisk create ram  <SIZE> [DRIVE_LETTER]   Create high-speed RAM disk\n");
    printf("  vdisk create vram <SIZE> [DRIVE_LETTER]   Create GPU VRAM disk (NVIDIA CUDA)\n");
    printf("  vdisk remove <DRIVE_LETTER>               Remove and unmount virtual disk\n");
    printf("  vdisk list                                List all active RAM & VRAM disks\n");
    printf("  vdisk status                              Show RAM and GPU VRAM hardware info\n");
    printf("  vdisk help                                Display this help menu\n\n");
    printf("EXAMPLES:\n");
    printf("  vdisk create ram 512M R:         Creates 512 MB RAM disk on drive R:\n");
    printf("  vdisk create vram 1024M V:       Creates 1 GB VRAM disk on drive V:\n");
    printf("  vdisk create ram 2G              Creates 2 GB RAM disk on free drive letter\n");
    printf("  vdisk remove R:                  Removes drive R:\n");
    printf("  vdisk list                       Shows list of all mounted vdisks\n");
    printf("  vdisk status                     Displays available System RAM & VRAM\n");
    printf("=====================================================================\n\n");
}

int main(int argc, char *argv[]) {
    disk_mgr_init();

    if (argc < 2) {
        print_help();
        return 0;
    }

    const char *action = argv[1];

    if (_stricmp(action, "help") == 0 || _stricmp(action, "-h") == 0 || _stricmp(action, "--help") == 0 || _stricmp(action, "/?") == 0) {
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

    // Commands requiring mandatory Admin privileges (create, remove)
    if (_stricmp(action, "create") == 0 || _stricmp(action, "add") == 0 ||
        _stricmp(action, "remove") == 0 || _stricmp(action, "rm") == 0 || _stricmp(action, "delete") == 0 || _stricmp(action, "del") == 0) {

        if (!IsUserAdmin()) {
            printf("[vdisk] This command requires Administrator privileges.\n");
            if (!RelaunchAsAdmin(argc, argv)) {
                return 1;
            }
            return 0;
        }
    }

    if (_stricmp(action, "create") == 0 || _stricmp(action, "add") == 0) {
        if (argc < 4) {
            printf("Error: Missing parameters for 'create'.\nUsage: vdisk create <ram|vram> <SIZE> [DRIVE_LETTER]\n");
            print_help();
            return 1;
        }

        const char *type_arg = argv[2];
        const char *size_arg = argv[3];
        char drive_letter = 0;

        if (argc >= 5) {
            drive_letter = argv[4][0];
        }

        size_t size_bytes = parse_size_string(size_arg);
        if (size_bytes == 0) {
            printf("Error: Invalid size '%s'. Examples: 512M, 1G, 2048M, 100MB\n", size_arg);
            return 1;
        }

        if (_stricmp(type_arg, "ram") == 0) {
            return disk_mgr_create_ram(size_bytes, drive_letter) ? 0 : 1;
        } else if (_stricmp(type_arg, "vram") == 0) {
            return disk_mgr_create_vram(size_bytes, drive_letter) ? 0 : 1;
        } else {
            printf("Error: Unknown disk type '%s'. Use 'ram' or 'vram'.\n", type_arg);
            return 1;
        }
    }

    if (_stricmp(action, "remove") == 0 || _stricmp(action, "rm") == 0 || _stricmp(action, "delete") == 0 || _stricmp(action, "del") == 0) {
        if (argc < 3) {
            printf("Error: Missing drive letter for 'remove'.\nUsage: vdisk remove <DRIVE_LETTER>\n");
            print_help();
            return 1;
        }
        char drive_letter = argv[2][0];
        return disk_mgr_remove(drive_letter) ? 0 : 1;
    }

    printf("Unknown command '%s'. Run 'vdisk help' for usage.\n", action);
    print_help();
    return 1;
}
