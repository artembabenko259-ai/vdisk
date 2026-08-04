#include <windows.h>
#include <winfsp/winfsp.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disk_manager.h"
#include "fs_memfs.h"
#include "block_disk.h"
#include "qemu_linux.h"
#include "save_disk.h"

#define RUN_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"

// Enable/disable running "vdisk mount" automatically at user login via the
// per-user Run key (no admin required).
static int autostart_set(int on) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    int ok;
    if (on) {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof(exe));
        char val[MAX_PATH + 16];
        snprintf(val, sizeof(val), "\"%s\" mount", exe);
        ok = (RegSetValueExA(k, "vdisk", 0, REG_SZ,
                             (const BYTE *)val, (DWORD)strlen(val) + 1) == ERROR_SUCCESS);
    } else {
        RegDeleteValueA(k, "vdisk");
        ok = 1;
    }
    RegCloseKey(k);
    return ok;
}

static int autostart_status(void) {
    HKEY k;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return 0;
    char buf[512];
    DWORD sz = sizeof(buf), type = 0;
    int on = (RegQueryValueExA(k, "vdisk", 0, &type, (BYTE *)buf, &sz) == ERROR_SUCCESS);
    RegCloseKey(k);
    return on;
}

static BOOL is_user_admin(void) {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

// Relaunches the current command elevated (for 'vdisk disk', which drives
// diskpart). Returns 1 if the elevated process was started.
static int relaunch_as_admin(int argc, char *argv[]) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));

    char args[2048] = "";
    for (int i = 1; i < argc; i++) {
        strcat_s(args, sizeof(args), "\"");
        strcat_s(args, sizeof(args), argv[i]);
        strcat_s(args, sizeof(args), "\" ");
    }

    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.lpVerb = "runas";
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_NORMAL;

    printf("[vdisk] Administrator rights required -- requesting elevation...\n");
    if (!ShellExecuteExA(&sei)) {
        DWORD err = GetLastError();
        if (err == ERROR_CANCELLED) printf("[vdisk] Elevation was cancelled.\n");
        else printf("[vdisk] Failed to elevate (error %lu).\n", err);
        return 0;
    }
    return 1;
}

static void print_help(void) {
    printf("\n");
    printf("=====================================================================\n");
    printf("     vdisk - Temporary RAM & VRAM Disk CLI Utility (WinFsp)\n");
    printf("=====================================================================\n");
    printf("COMMANDS:\n");
    printf("  vdisk create ram  <SIZE> [DRIVE] [-f FSNAME]  Create RAM disk\n");
    printf("  vdisk create vram <SIZE> [DRIVE] [-f FSNAME]  Create GPU VRAM disk\n");
    printf("  vdisk remove <DRIVE>                          Remove one disk\n");
    printf("  vdisk clear                                   Remove ALL disks\n");
    printf("  vdisk list                                    List active disks\n");
    printf("  vdisk status                                  Show RAM/VRAM hardware info\n");
    printf("  vdisk save                                    Remember active disks for auto-mount\n");
    printf("  vdisk save <DISK> \"<DEST>\" [ITEM ...]          Copy data OUT of a disk (persist before wipe)\n");
    printf("  vdisk mount                                   Mount every disk in config\n");
    printf("  vdisk config                                  Show the config file\n");
    printf("  vdisk autostart <on|off|status>               Auto-mount at login\n");
    printf("  vdisk linux [DRIVE]                           Instant BusyBox Unix shell on a disk\n");
    printf("  vdisk linux -s <DRIVE>                        REAL Alpine Linux in QEMU (headless console)\n");
    printf("  vdisk linux -s <DRIVE> --tools \"<pkg>\"          ...and auto-install any apk package (local boot image)\n");
    printf("  vdisk linux -s <DRIVE> --tools \"<pkg>\" --tools-net\n");
    printf("                                                 ...same, but from the full network apk repos\n");
    printf("  vdisk disk ram  <SIZE> [DRIVE]                REAL physical disk, RAM-backed (one step)\n");
    printf("  vdisk disk vram <SIZE> [DRIVE]                REAL physical disk, VRAM-backed (one step)\n");
    printf("  vdisk disk <DRIVE> <SIZE> [--format]          REAL physical disk on an existing vdisk\n");
    printf("  vdisk disk remove <DRIVE>                     Detach it (and its backing if RAM/VRAM one-step)\n");
    printf("  vdisk disk list                               List physical block disks\n");
    printf("  vdisk help                                    Display this help menu\n\n");
    printf("EXAMPLES:\n");
    printf("  vdisk create ram 512M R:            512 MB RAM disk on R:\n");
    printf("  vdisk create vram 1G V: -f FAT32    1 GB VRAM disk on V: reported as FAT32\n");
    printf("  vdisk save                          Remember current disks...\n");
    printf("  vdisk autostart on                  ...and mount them automatically at login\n");
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
    // Usage: vdisk --fs-worker <ram|vram> <SIZE_MB> <DRIVE_LETTER> [FSNAME]
    if (_stricmp(action, "--fs-worker") == 0) {
        if (argc < 5) return 1;
        int use_vram = (_stricmp(argv[2], "vram") == 0);
        unsigned long long size_mb = _strtoui64(argv[3], NULL, 10);
        char drive = argv[4][0];
        const char *fs = (argc >= 6) ? argv[5] : "NTFS";
        if (size_mb == 0 || !drive) return 1;
        return fs_run(use_vram, size_mb, drive, fs);
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

    if (_stricmp(action, "mount") == 0) {
        disk_mgr_mount_config();
        return 0;
    }

    if (_stricmp(action, "save") == 0) {
        // No args  -> remember active disks for auto-mount (config).
        // <DISK> <DEST> [ITEM ...] -> copy data OUT of a disk to a real path.
        if (argc <= 2) {
            disk_mgr_save_config();
            return 0;
        }
        if (argc < 4) {
            printf("Usage:\n");
            printf("  vdisk save                              remember active disks for auto-mount\n");
            printf("  vdisk save <DISK> \"<DEST>\" [ITEM ...]   copy data out of a disk to <DEST>\n");
            printf("                                          (no ITEM or '.' = the whole disk)\n");
            return 1;
        }
        char disk = argv[2][0];
        const char *dest = argv[3];
        const char *items[64];
        int n = 0;
        for (int i = 4; i < argc && n < 64; i++) {
            if (i == 4 && (_stricmp(argv[i], "--path") == 0 || _stricmp(argv[i], "-path") == 0 ||
                           _stricmp(argv[i], "--paths") == 0 || _stricmp(argv[i], "-add") == 0))
                continue; // tolerate an optional keyword before the item list
            items[n++] = argv[i];
        }
        return disk_save(disk, dest, items, n) ? 0 : 1;
    }

    if (_stricmp(action, "config") == 0) {
        disk_mgr_show_config();
        return 0;
    }

    if (_stricmp(action, "linux") == 0 || _stricmp(action, "sh") == 0 || _stricmp(action, "shell") == 0) {
        // vdisk linux -s <DRIVE> [--tools "<name>"] [--tools-net]
        //   -> real Alpine Linux in QEMU (headless console); --tools-net
        //      auto-provisions the named tool via apk before handing over
        //      the interactive shell (see qemu_linux.c for supported tools).
        if (argc >= 3 && _stricmp(argv[2], "-s") == 0) {
            char drive = 0;
            const char *tool = NULL;
            int tools_net = 0;
            for (int i = 3; i < argc; i++) {
                if (_stricmp(argv[i], "--tools") == 0 && i + 1 < argc) {
                    tool = argv[++i];
                } else if (_stricmp(argv[i], "--tools-net") == 0) {
                    tools_net = 1;
                } else if (!drive) {
                    drive = argv[i][0];
                }
            }
            return qemu_run_linux(drive, tool, tools_net) ? 0 : 1;
        }
        // vdisk linux [DRIVE]     -> instant BusyBox shell
        char drive = (argc >= 3) ? argv[2][0] : 0;
        return disk_mgr_linux(drive) ? 0 : 1;
    }

    if (_stricmp(action, "disk") == 0) {
        // 'list' is read-only (no admin needed).
        if (argc >= 3 && _stricmp(argv[2], "list") == 0) {
            block_disk_list();
            return 0;
        }
        if (argc < 3) {
            printf("Usage:\n");
            printf("  vdisk disk <DRIVE> <SIZE> [--format]   attach a real physical disk backed by that vdisk\n");
            printf("  vdisk disk remove <DRIVE>              detach it\n");
            printf("  vdisk disk list                        list block disks\n");
            return 1;
        }
        // Everything else drives diskpart -> needs Administrator.
        if (!is_user_admin()) {
            return relaunch_as_admin(argc, argv) ? 0 : 1;
        }
        if (_stricmp(argv[2], "remove") == 0 || _stricmp(argv[2], "detach") == 0) {
            if (argc < 4) { printf("Usage: vdisk disk remove <DRIVE>\n"); return 1; }
            return block_disk_detach(argv[3][0]) ? 0 : 1;
        }
        // one-step: vdisk disk ram|vram <SIZE> [DRIVE]  (creates backing + attaches)
        if (_stricmp(argv[2], "ram") == 0 || _stricmp(argv[2], "vram") == 0) {
            if (argc < 4) { printf("Usage: vdisk disk %s <SIZE> [DRIVE]\n", argv[2]); return 1; }
            int use_vram = (_stricmp(argv[2], "vram") == 0);
            size_t bytes = parse_size_string(argv[3]);
            if (bytes == 0) { printf("Error: invalid size '%s'.\n", argv[3]); return 1; }
            unsigned long long mb = (unsigned long long)(bytes / (1024 * 1024));
            char letter = (argc >= 5) ? argv[4][0] : 0;
            return block_disk_create_auto(use_vram, mb, letter) ? 0 : 1;
        }
        // explicit: vdisk disk <DRIVE> <SIZE> [--format]  (attach onto existing vdisk)
        if (argc < 4) { printf("Usage: vdisk disk <DRIVE> <SIZE> [--format]\n"); return 1; }
        char L = argv[2][0];
        size_t bytes = parse_size_string(argv[3]);
        if (bytes == 0) { printf("Error: invalid size '%s'.\n", argv[3]); return 1; }
        unsigned long long mb = (unsigned long long)(bytes / (1024 * 1024));
        if (mb < 3) { printf("Error: minimum block-disk size is 3 MB.\n"); return 1; }
        int fmt = 0;
        for (int i = 4; i < argc; i++)
            if (_stricmp(argv[i], "--format") == 0 || _stricmp(argv[i], "-f") == 0) fmt = 1;
        return block_disk_attach(L, mb, fmt) ? 0 : 1;
    }

    if (_stricmp(action, "autostart") == 0) {
        const char *sub = (argc >= 3) ? argv[2] : "status";
        if (_stricmp(sub, "on") == 0) {
            printf(autostart_set(1)
                   ? "[vdisk] Autostart enabled: 'vdisk mount' will run at each login.\n"
                   : "[vdisk] Failed to enable autostart.\n");
        } else if (_stricmp(sub, "off") == 0) {
            autostart_set(0);
            printf("[vdisk] Autostart disabled.\n");
        } else {
            printf("[vdisk] Autostart is %s.\n", autostart_status() ? "ON" : "OFF");
        }
        return 0;
    }

    if (_stricmp(action, "create") == 0 || _stricmp(action, "add") == 0) {
        if (argc < 4) {
            printf("Error: Missing parameters for 'create'.\n"
                   "Usage: vdisk create <ram|vram> <SIZE> [DRIVE] [-f FSNAME]\n");
            return 1;
        }
        const char *type_arg = argv[2];
        const char *size_arg = argv[3];

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

        // Optional [DRIVE] and [-f FSNAME] / trailing FSNAME, any order.
        char drive_letter = 0;
        const char *fs_name = "NTFS";
        for (int i = 4; i < argc; i++) {
            if (_stricmp(argv[i], "-f") == 0 || _stricmp(argv[i], "-F") == 0) {
                if (i + 1 < argc) fs_name = argv[++i];
            } else if (drive_letter == 0) {
                drive_letter = argv[i][0];
            } else {
                fs_name = argv[i];
            }
        }
        return disk_mgr_create(use_vram, size_bytes, drive_letter, fs_name) ? 0 : 1;
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
