#include <windows.h>
#include <winfsp/winfsp.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "disk_manager.h"
#include "fs_memfs.h"
#include "fs_nfsclient.h"
#include "block_disk.h"
#include "qemu_linux.h"
#include "save_disk.h"
#include "accel.h"
#include "packages.h"

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
    printf("=====================================================================\n\n");

    printf("-- DISKS (create/remove/list a RAM or VRAM drive letter) --------------\n");
    printf("  vdisk create ram  <SIZE> [DRIVE] [-f FSNAME]  Create a RAM disk\n");
    printf("  vdisk create vram <SIZE> [DRIVE] [-f FSNAME]  Create a GPU VRAM disk\n");
    printf("  vdisk remove <DRIVE>                          Remove/unmount one disk\n");
    printf("  vdisk clear                                   Remove ALL vdisk disks\n");
    printf("  vdisk list                                    List active disks (drive, size, PID)\n");
    printf("  vdisk status                                  Show free RAM / GPU VRAM available\n");
    printf("    SIZE takes K/M/G suffixes (512M, 1G, 2048M). No DRIVE = next free letter\n");
    printf("    from Z: down. -f sets the filesystem name Windows reports (default NTFS).\n");
    printf("    Contents are ephemeral: gone on 'remove'/'clear' or reboot -- see 'save' below\n");
    printf("    to keep data, or 'linux -s --persist' below for a disk that self-persists.\n\n");

    printf("-- SAVE, AUTO-MOUNT & CONFIG -------------------------------------------\n");
    printf("  vdisk save                                    Remember current disks for auto-mount\n");
    printf("  vdisk save <DISK> \"<DEST>\" [ITEM ...]          Copy data OUT to a real folder now\n");
    printf("  vdisk save <DISK> [\"<DEST>\"] --ev <time>       Repeat that copy on a loop (30sec/10min/hour/day)\n");
    printf("  vdisk mount                                    (Re)create every disk listed in the config\n");
    printf("  vdisk config                                   Show the config file path + contents\n");
    printf("  vdisk autostart <on|off|status>                Run 'vdisk mount' automatically at login\n");
    printf("    Typical setup: create your disks once, 'vdisk save' to remember them, then\n");
    printf("    'vdisk autostart on' so they're back every login. 'save <DISK> <DEST>' is a\n");
    printf("    ONE-OFF export (do it before 'remove'/shutdown); '--ev' automates that export\n");
    printf("    on a timer while it keeps running -- neither makes the disk itself durable.\n\n");

    printf("-- LINUX ON A DISK ------------------------------------------------------\n");
    printf("  vdisk linux [DRIVE]                            Instant BusyBox shell (no VM, no kernel)\n");
    printf("  vdisk linux -s <DRIVE>                          REAL Alpine kernel in QEMU (headless, WSL2-style)\n");
    printf("  vdisk linux -s <DRIVE> --persist                 ...same, but installed ONTO the disk so it\n");
    printf("                                                   survives separate runs (see below)\n");
    printf("  vdisk linux -s <DRIVE> --distro debian --persist  ...install a real Debian instead of Alpine\n");
    printf("  vdisk linux -s <DRIVE> --tools \"<pkg>\"            ...auto-install one apk package (offline, from\n");
    printf("                                                   the local boot image) before handing you the shell\n");
    printf("  vdisk linux -s <DRIVE> --tools \"<pkg>\" --tools-net  ...same, from the full network apk repos\n");
    printf("  vdisk linux -s <DRIVE> --share                   Bridge a Windows folder into the VM at /mnt/win (SMB)\n");
    printf("  vdisk linux -s <DRIVE> -image <path.iso>          Boot any ISO with its own bootloader instead\n");
    printf("  vdisk linux --package [list|add <n>|del <n>|set-defaults]   Manage your apk favorites list\n");
    printf("\n");
    printf("  BusyBox ('linux' with no '-s') is instant but is only Unix tools (ls, vi, grep,\n");
    printf("  ...) -- no real kernel, no apk, can't run Linux ELF binaries. For that you need\n");
    printf("  '-s', a real Alpine kernel booted in QEMU (~1-2 min first boot under software\n");
    printf("  emulation; run 'vdisk accel enable' once, as admin, for near-native speed).\n");
    printf("\n");
    printf("  Without --persist, '-s' is fully diskless every run: apk installs and any files\n");
    printf("  outside the attached data disk vanish the moment you 'poweroff'. --persist\n");
    printf("  installs a small real system straight onto the vdisk's data disk instead (~30-90s,\n");
    printf("  offline, the first time only); every run after that boots directly off that disk,\n");
    printf("  so apk packages and files under / survive across separate invocations, right up\n");
    printf("  until you 'vdisk remove <DRIVE>' and wipe the RAM/VRAM disk it lives on.\n");
    printf("  --distro debian (implies --persist) debootstraps a real Debian 12 instead of\n");
    printf("  Alpine onto that same disk -- needs network for that one-time install (~3-8 min);\n");
    printf("  Alpine stays the default and is the only offline-capable option.\n\n");
    printf("  LIVE EXPLORER ACCESS (like \\\\wsl$\\): every '--persist' session also tries to\n");
    printf("  pick a free drive letter and mount the VM's live '/' onto it for\n");
    printf("  real, read/write access from Explorer/any Windows app while the VM is running --\n");
    printf("  reliable on Alpine; on Debian it's best-effort and can fall back to a plain\n");
    printf("  shell (see the warning note below).\n");
    printf("  no extra flag needed. Look for 'X:\\ now mirrors this VM's / live' in the banner\n");
    printf("  for which letter it picked. Under the hood: a tiny NFSv3 server is built with the\n");
    printf("  guest's own gcc/apt-gcc the first time it's needed (cached after, on the same\n");
    printf("  persisted disk) and the Windows side is a plain NFS network client (via WinFsp) --\n");
    printf("  the guest's own ext4 driver stays the only thing that ever touches the disk\n");
    printf("  bytes, so it's safe to use at the same time you're inside the VM's own shell.\n");
    printf("  If it doesn't come up in time (needs outbound network the first time, for gcc/\n");
    printf("  build-essential), you'll see a warning but the normal interactive shell still\n");
    printf("  works -- check /tmp/vdisk-nfs*.log inside the VM for why.\n");
    printf("  Always exit with 'poweroff', not Ctrl-A X: a clean poweroff flushes writes to the\n");
    printf("  disk, killing the VM can lose the last few seconds of unsynced writes -- same as\n");
    printf("  unplugging a real machine. --persist is NOT compatible with --tools/--share/-image\n");
    printf("  (those assume the plain diskless Alpine).\n\n");

    printf("-- REAL PHYSICAL DISKS (for disk-management tools, diskpart, etc.) -----\n");
    printf("  vdisk disk ram  <SIZE> [DRIVE]                 REAL \\\\.\\PhysicalDrive, RAM-backed (one step)\n");
    printf("  vdisk disk vram <SIZE> [DRIVE]                 REAL \\\\.\\PhysicalDrive, VRAM-backed (one step)\n");
    printf("  vdisk disk <DRIVE> <SIZE> [--format]           Attach one onto an EXISTING vdisk instead\n");
    printf("  vdisk disk remove <DRIVE>                      Detach it (+ its backing, if it was one-step)\n");
    printf("  vdisk disk list                                 List attached physical block disks\n");
    printf("    A plain vdisk is a filesystem (drive letter); this is a genuine raw block device\n");
    printf("    (visible in Disk Management/'diskpart list disk') for exercising partitioning or\n");
    printf("    destructive disk tools safely. Requires Administrator (self-elevates).\n\n");

    printf("-- HARDWARE VM ACCELERATION ---------------------------------------------\n");
    printf("  vdisk accel [status]                           Check Windows Hypervisor Platform (no admin)\n");
    printf("  vdisk accel enable                             Turn it on (Administrator + one-time reboot)\n");
    printf("    Speeds up every 'linux -s' VM from ~1-2 min software-emulated boots to near-native.\n\n");

    printf("  vdisk help                                     Display this help menu\n");
    printf("=====================================================================\n");
    printf("EXAMPLES:\n");
    printf("  vdisk create ram 512M R:                       512 MB RAM disk on R:\n");
    printf("  vdisk save && vdisk autostart on                Remember disks, remount them every login\n");
    printf("  vdisk create ram 2G D: && vdisk linux -s D: --persist\n");
    printf("                                                  A real Linux that keeps its state around\n");
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

    // Internal: WinFsp worker that relays a Windows drive letter to a live
    // NFSv3 export inside a running 'vdisk linux -s --persist' guest.
    // Usage: vdisk --nfs-worker <HOST> <NFS_PORT> <MOUNT_PORT> <EXPORT_PATH> <DRIVE_LETTER> [FSNAME]
    if (_stricmp(action, "--nfs-worker") == 0) {
        if (argc < 7) return 1;
        const char *host = argv[2];
        unsigned short nfs_port = (unsigned short)atoi(argv[3]);
        unsigned short mount_port = (unsigned short)atoi(argv[4]);
        const char *export_path = argv[5];
        char drive = argv[6][0];
        const char *fs = (argc >= 8) ? argv[7] : "NTFS";
        if (!nfs_port || !mount_port || !drive) return 1;
        return fs_nfs_run(host, nfs_port, mount_port, export_path, drive, fs);
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
        // <DISK> [DEST] --ev <time> -> periodic auto-save loop (see below).
        // <DISK> <DEST> [ITEM ...] -> copy data OUT of a disk to a real path.
        if (argc <= 2) {
            disk_mgr_save_config();
            return 0;
        }
        char disk = argv[2][0];

        // vdisk save <DISK> [DEST] --ev <10min|hour|30sec|day|...>
        //   -> not true persistence (RAM is still wiped on reboot), but a
        //      periodic snapshot to a real folder while this keeps running;
        //      DEST defaults to %LOCALAPPDATA%\vdisk\autosave\<DRIVE> (see
        //      save_disk.c). Blocks until Ctrl-C or the disk is unmounted.
        const char *ev_time = NULL;
        const char *ev_dest = NULL;
        for (int i = 3; i < argc; i++) {
            if (_stricmp(argv[i], "--ev") == 0 && i + 1 < argc) {
                ev_time = argv[++i];
            } else if (!ev_dest) {
                ev_dest = argv[i];
            }
        }
        if (ev_time) {
            return disk_save_periodic(disk, ev_dest, ev_time) ? 0 : 1;
        }

        if (argc < 4) {
            printf("Usage:\n");
            printf("  vdisk save                              remember active disks for auto-mount\n");
            printf("  vdisk save <DISK> \"<DEST>\" [ITEM ...]   copy data out of a disk to <DEST>\n");
            printf("                                          (no ITEM or '.' = the whole disk)\n");
            printf("  vdisk save <DISK> [\"<DEST>\"] --ev <time>  periodic auto-save loop (30sec, 10min, hour, day, ...)\n");
            return 1;
        }
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

    if (_stricmp(action, "accel") == 0) {
        const char *sub = (argc >= 3) ? argv[2] : "status";
        if (_stricmp(sub, "enable") == 0) {
            if (!is_user_admin()) return relaunch_as_admin(argc, argv) ? 0 : 1;
            return accel_enable() ? 0 : 1;
        }
        accel_status();
        return 0;
    }

    if (_stricmp(action, "linux") == 0 || _stricmp(action, "sh") == 0 || _stricmp(action, "shell") == 0) {
        // vdisk linux -s <DRIVE> [--tools "<name>"] [--tools-net] [--share]
        //   -> real Alpine Linux in QEMU (headless console); --tools-net
        //      auto-provisions the named tool via apk before handing over
        //      the interactive shell (see qemu_linux.c for supported tools);
        //      --share bridges a stable Windows folder into the VM over SMB
        //      (see bridge.h -- not the disk itself, which Windows' SMB
        //      server can't see across the admin/non-admin session split).
        // vdisk linux --package list|add <name>|del <name>|set-defaults
        //   -> manage the personal favorites list (see packages.h); empty by
        //      default, purely a reminder/convenience -- --tools works with
        //      any package name regardless of what's saved here.
        if (argc >= 3 && _stricmp(argv[2], "--package") == 0) {
            const char *sub = (argc >= 4) ? argv[3] : "list";
            const char *arg = (argc >= 5) ? argv[4] : NULL;
            if (_stricmp(sub, "add") == 0) packages_add(arg);
            else if (_stricmp(sub, "del") == 0 || _stricmp(sub, "remove") == 0) packages_del(arg);
            else if (_stricmp(sub, "set-defaults") == 0) packages_set_defaults();
            else packages_list();
            return 0;
        }
        if (argc >= 3 && _stricmp(argv[2], "-s") == 0) {
            char drive = 0;
            const char *tool = NULL;
            const char *image = NULL;
            const char *distro = NULL;
            int tools_net = 0;
            int want_share = 0;
            int persist = 0;
            for (int i = 3; i < argc; i++) {
                if (_stricmp(argv[i], "--tools") == 0 && i + 1 < argc) {
                    tool = argv[++i];
                } else if (_stricmp(argv[i], "--tools-net") == 0) {
                    tools_net = 1;
                } else if (_stricmp(argv[i], "--share") == 0) {
                    want_share = 1;
                } else if (_stricmp(argv[i], "-image") == 0 && i + 1 < argc) {
                    image = argv[++i];
                } else if (_stricmp(argv[i], "--persist") == 0) {
                    persist = 1;
                } else if (_stricmp(argv[i], "--distro") == 0 && i + 1 < argc) {
                    distro = argv[++i];
                } else if (!drive) {
                    drive = argv[i][0];
                }
            }
            if (image && (tool || want_share)) {
                printf("[vdisk] -image can't be combined with --tools/--tools-net/--share "
                       "(those assume the built-in Alpine image).\n");
                return 1;
            }
            if (persist && (tool || want_share || image)) {
                printf("[vdisk] --persist can't be combined with --tools/--tools-net/--share/-image.\n");
                return 1;
            }
            // --share needs Administrator for ONE step (creating the SMB
            // share) -- but elevating this WHOLE process would make it lose
            // sight of the vdisk (RAM disk letters are only visible in the
            // session that mounted them, not an elevated one of the same
            // user). So we deliberately stay unelevated here; bridge.c
            // elevates only that one narrow step, in a separate short-lived
            // process, then hands control straight back.
            return qemu_run_linux(drive, tool, tools_net, want_share, image, persist, distro) ? 0 : 1;
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
