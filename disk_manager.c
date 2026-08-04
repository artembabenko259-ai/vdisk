#include "disk_manager.h"
#include "vram_allocator.h"
#include "vdisk_util.h"
#include "linux_shell.h"
#include "block_disk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static disk_manager_t g_mgr;

// State lives in a stable per-user location so it is consistent regardless of
// the working directory a command is launched from.
static const char *data_file(const char *name) {
    static char path[MAX_PATH];
    char dir[MAX_PATH];
    if (get_data_dir(dir, sizeof(dir))) snprintf(path, sizeof(path), "%s\\%s", dir, name);
    else strcpy_s(path, sizeof(path), name);
    return path;
}

static const char *state_file_path(void) {
    static char path[MAX_PATH] = {0};
    if (!path[0]) strcpy_s(path, sizeof(path), data_file("vdisk_state.txt"));
    return path;
}

static const char *config_file_path(void) {
    static char path[MAX_PATH] = {0};
    if (!path[0]) strcpy_s(path, sizeof(path), data_file("vdisk.conf"));
    return path;
}

static void load_state(void) {
    memset(&g_mgr, 0, sizeof(g_mgr));
    FILE *f = fopen(state_file_path(), "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char drive;
        char type_str[16];
        char fs_str[16] = "NTFS";
        size_t size_b;
        DWORD pid;
        if (sscanf_s(line, "%c %15s %llu %lu %15s",
                     &drive, (unsigned)1,
                     type_str, (unsigned)sizeof(type_str),
                     (unsigned long long *)&size_b, &pid,
                     fs_str, (unsigned)sizeof(fs_str)) >= 4) {
            int idx = toupper((unsigned char)drive) - 'A';
            if (idx >= 0 && idx < MAX_DISKS) {
                g_mgr.entries[idx].drive_letter = (char)toupper((unsigned char)drive);
                g_mgr.entries[idx].type = (strcmp(type_str, "VRAM") == 0) ? DISK_TYPE_VRAM : DISK_TYPE_RAM;
                g_mgr.entries[idx].size_bytes = size_b;
                g_mgr.entries[idx].pid = pid;
                strcpy_s(g_mgr.entries[idx].fs_name, sizeof(g_mgr.entries[idx].fs_name), fs_str);
                g_mgr.entries[idx].is_active = 1;
            }
        }
    }
    fclose(f);
}

static void save_state(void) {
    FILE *f = fopen(state_file_path(), "w");
    if (!f) return;
    for (int i = 0; i < MAX_DISKS; i++) {
        if (g_mgr.entries[i].is_active) {
            fprintf(f, "%c %s %llu %lu %s\n",
                    g_mgr.entries[i].drive_letter,
                    (g_mgr.entries[i].type == DISK_TYPE_VRAM) ? "VRAM" : "RAM",
                    (unsigned long long)g_mgr.entries[i].size_bytes,
                    g_mgr.entries[i].pid,
                    g_mgr.entries[i].fs_name[0] ? g_mgr.entries[i].fs_name : "NTFS");
        }
    }
    fclose(f);
}

void disk_mgr_init(void) {
    load_state();
}

size_t parse_size_string(const char *str) {
    if (!str) return 0;
    char *endptr;
    double val = strtod(str, &endptr);
    if (val <= 0) return 0;

    while (*endptr && isspace((unsigned char)*endptr)) endptr++;

    switch (toupper((unsigned char)*endptr)) {
        case 'G': return (size_t)(val * 1024.0 * 1024.0 * 1024.0);
        case 'M': return (size_t)(val * 1024.0 * 1024.0);
        case 'K': return (size_t)(val * 1024.0);
        default:  return (size_t)val;
    }
}

char find_free_drive_letter(void) {
    DWORD drives = GetLogicalDrives();
    for (char c = 'Z'; c >= 'D'; c--) {
        int bit = c - 'A';
        if (!(drives & (1u << bit))) return c;
    }
    return 'R';
}

static int drive_in_use(char letter) {
    return (GetLogicalDrives() & (1u << (letter - 'A'))) != 0;
}

// Launches the detached WinFsp worker process for one disk and waits until the
// drive letter appears (or the worker dies / times out).
static DWORD spawn_worker(int use_vram, unsigned long long size_mb, char drive_letter,
                          const char *fs_name) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" --fs-worker %s %llu %c %s",
             exe, use_vram ? "vram" : "ram", size_mb, drive_letter,
             (fs_name && *fs_name) ? fs_name : "NTFS");

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS,
                        NULL, NULL, &si, &pi)) {
        printf("[vdisk] Failed to launch worker process (%lu)\n", GetLastError());
        return 0;
    }
    CloseHandle(pi.hThread);
    DWORD pid = pi.dwProcessId;

    // Poll up to ~15s for the mount to appear.
    DWORD result = 0;
    for (int i = 0; i < 150; i++) {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break; // worker exited
        if (drive_in_use(drive_letter)) { result = pid; break; }
        Sleep(100);
    }

    if (!result) {
        TerminateProcess(pi.hProcess, 1); // no-op if already gone
    }
    CloseHandle(pi.hProcess);
    return result;
}

// Terminates the worker; WinFsp unmounts the drive automatically on exit.
static void kill_worker(DWORD pid) {
    if (!pid) return;
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (h) {
        TerminateProcess(h, 0);
        WaitForSingleObject(h, 3000);
        CloseHandle(h);
    }
}

int disk_mgr_create(int use_vram, size_t size_bytes, char drive_letter, const char *fs_name) {
    if (!fs_name || !*fs_name) fs_name = "NTFS";
    if (!drive_letter) drive_letter = find_free_drive_letter();
    drive_letter = (char)toupper((unsigned char)drive_letter);
    if (drive_letter < 'A' || drive_letter > 'Z') {
        printf("[vdisk] Error: invalid drive letter.\n");
        return 0;
    }
    if (drive_in_use(drive_letter)) {
        printf("[vdisk] Error: drive %c: is already in use.\n", drive_letter);
        return 0;
    }

    unsigned long long size_mb = (unsigned long long)(size_bytes / (1024 * 1024));
    if (size_mb < 2) {
        printf("[vdisk] Error: minimum disk size is 2 MB.\n");
        return 0;
    }

    if (use_vram) {
        vram_info_t vinfo;
        if (!vram_init(&vinfo)) {
            printf("[vdisk] Error: CUDA VRAM acceleration is not available on this system!\n");
            return 0;
        }
        printf("[vdisk] Target GPU: %s (Free VRAM: %.2f MB / Total: %.2f MB)\n",
               vinfo.device_name,
               (double)vinfo.free_vram_bytes / (1024.0 * 1024.0),
               (double)vinfo.total_vram_bytes / (1024.0 * 1024.0));
        if (size_bytes > vinfo.free_vram_bytes) {
            printf("[vdisk] Error: Requested size (%.2f MB) exceeds free VRAM (%.2f MB)\n",
                   (double)size_bytes / (1024.0 * 1024.0),
                   (double)vinfo.free_vram_bytes / (1024.0 * 1024.0));
            return 0;
        }
    }

    printf("[vdisk] Creating %s disk of %llu MB on drive %c: (%s) ...\n",
           use_vram ? "VRAM" : "RAM", size_mb, drive_letter, fs_name);

    DWORD pid = spawn_worker(use_vram, size_mb, drive_letter, fs_name);
    if (!pid) {
        printf("[vdisk] Failed to mount %s disk on drive %c:\n",
               use_vram ? "VRAM" : "RAM", drive_letter);
        return 0;
    }

    int idx = drive_letter - 'A';
    g_mgr.entries[idx].drive_letter = drive_letter;
    g_mgr.entries[idx].type = use_vram ? DISK_TYPE_VRAM : DISK_TYPE_RAM;
    g_mgr.entries[idx].size_bytes = size_bytes;
    g_mgr.entries[idx].pid = pid;
    strcpy_s(g_mgr.entries[idx].fs_name, sizeof(g_mgr.entries[idx].fs_name), fs_name);
    g_mgr.entries[idx].is_active = 1;
    save_state();

    // Give every fresh disk the same starter layout instead of a bare drive
    // root: Data\ for your own files, VM\ for vdisk-linux's own image files.
    char folder[16];
    snprintf(folder, sizeof(folder), "%c:\\Data", drive_letter);
    CreateDirectoryA(folder, NULL);
    snprintf(folder, sizeof(folder), "%c:\\VM", drive_letter);
    CreateDirectoryA(folder, NULL);

    printf("[vdisk] SUCCESS! %s disk mounted at drive %c:\\ (worker PID %lu)\n",
           use_vram ? "VRAM" : "RAM", drive_letter, pid);
    return 1;
}

int disk_mgr_remove(char drive_letter) {
    drive_letter = (char)toupper((unsigned char)drive_letter);
    if (drive_letter < 'A' || drive_letter > 'Z') {
        printf("[vdisk] Error: invalid drive letter.\n");
        return 0;
    }
    int idx = drive_letter - 'A';

    if (!g_mgr.entries[idx].is_active) {
        printf("[vdisk] Disk %c: is not managed by vdisk.\n", drive_letter);
        return 0;
    }

    if (block_disk_is_backing(drive_letter)) {
        printf("[vdisk] Disk %c: currently backs an attached physical disk.\n", drive_letter);
        printf("[vdisk] Detach it first: 'vdisk disk remove %c' (needs admin), then remove %c:.\n",
               drive_letter, drive_letter);
        return 0;
    }

    printf("[vdisk] Removing disk %c: ...\n", drive_letter);
    kill_worker(g_mgr.entries[idx].pid);

    memset(&g_mgr.entries[idx], 0, sizeof(g_mgr.entries[idx]));
    save_state();

    printf("[vdisk] Disk %c: successfully removed!\n", drive_letter);
    return 1;
}

int disk_mgr_clear(void) {
    int count = 0, skipped = 0;
    for (int i = 0; i < MAX_DISKS; i++) {
        if (g_mgr.entries[i].is_active) {
            if (block_disk_is_backing(g_mgr.entries[i].drive_letter)) {
                printf("[vdisk] Skipping %c: (backs a physical disk; run 'vdisk disk remove %c' first).\n",
                       g_mgr.entries[i].drive_letter, g_mgr.entries[i].drive_letter);
                skipped++;
                continue;
            }
            printf("[vdisk] Removing disk %c: (PID %lu) ...\n",
                   g_mgr.entries[i].drive_letter, g_mgr.entries[i].pid);
            kill_worker(g_mgr.entries[i].pid);
            memset(&g_mgr.entries[i], 0, sizeof(g_mgr.entries[i]));
            count++;
        }
    }
    save_state();
    (void)skipped;

    if (count == 0) printf("[vdisk] No active disks to clear.\n");
    else            printf("[vdisk] Cleared %d disk(s).\n", count);
    return count;
}

void disk_mgr_list(void) {
    load_state();
    printf("\n=== Active Temporary Virtual Disks (vdisk) ===\n");
    printf("%-8s %-8s %-14s %-14s %-10s\n", "Drive", "Type", "Size", "Backing", "Status");
    printf("--------------------------------------------------------\n");

    int count = 0;
    for (int i = 0; i < MAX_DISKS; i++) {
        if (g_mgr.entries[i].is_active) {
            count++;
            char size_buf[64];
            snprintf(size_buf, sizeof(size_buf), "%.2f MB",
                     (double)g_mgr.entries[i].size_bytes / (1024.0 * 1024.0));
            char backing_buf[64];
            snprintf(backing_buf, sizeof(backing_buf), "PID %lu", g_mgr.entries[i].pid);

            printf("%c:       %-8s %-14s %-14s %-10s\n",
                   g_mgr.entries[i].drive_letter,
                   (g_mgr.entries[i].type == DISK_TYPE_VRAM) ? "VRAM" : "RAM",
                   size_buf, backing_buf, "MOUNTED");
        }
    }

    if (count == 0) printf("No active RAM/VRAM disks managed by vdisk.\n");
    printf("--------------------------------------------------------\n\n");
}

void disk_mgr_status(void) {
    printf("\n=== Hardware & Memory Status ===\n");

    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        printf("System RAM: Free %.2f GB / Total %.2f GB\n",
               (double)memStatus.ullAvailPhys / (1024.0 * 1024.0 * 1024.0),
               (double)memStatus.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
    }

    vram_info_t vinfo;
    if (vram_init(&vinfo)) {
        printf("GPU Device: %s\n", vinfo.device_name);
        printf("GPU VRAM:   Free %.2f MB / Total %.2f MB\n",
               (double)vinfo.free_vram_bytes / (1024.0 * 1024.0),
               (double)vinfo.total_vram_bytes / (1024.0 * 1024.0));
    } else {
        printf("GPU VRAM:   CUDA Acceleration Unavailable\n");
    }
    printf("================================\n\n");
}

int disk_mgr_mount_config(void) {
    FILE *f = fopen(config_file_path(), "r");
    if (!f) {
        printf("[vdisk] No config file at %s\n", config_file_path());
        printf("[vdisk] Set up disks and run 'vdisk save', or edit the file (see 'vdisk config').\n");
        return 0;
    }

    int count = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p && isspace((unsigned char)*p)) p++;
        if (*p == '#' || *p == ';' || *p == '\0' || *p == '\n' || *p == '\r') continue;

        char type[16] = "", size[32] = "", letter[8] = "", fs[16] = "NTFS";
        int n = sscanf_s(p, "%15s %31s %7s %15s",
                         type, (unsigned)sizeof(type),
                         size, (unsigned)sizeof(size),
                         letter, (unsigned)sizeof(letter),
                         fs, (unsigned)sizeof(fs));
        if (n < 3) continue;

        int use_vram = (_stricmp(type, "vram") == 0);
        size_t bytes = parse_size_string(size);
        if (bytes == 0) { printf("[vdisk] Skipping line with invalid size '%s'\n", size); continue; }
        char L = (char)toupper((unsigned char)letter[0]);
        if (disk_mgr_create(use_vram, bytes, L, fs)) count++;
    }
    fclose(f);
    printf("[vdisk] Mounted %d disk(s) from config.\n", count);
    return count;
}

int disk_mgr_save_config(void) {
    load_state();
    FILE *f = fopen(config_file_path(), "w");
    if (!f) { printf("[vdisk] Failed to write config file.\n"); return 0; }

    fprintf(f, "# vdisk auto-mount config -- one disk per line:\n");
    fprintf(f, "#   <ram|vram> <size> <letter> [fsname]\n");
    int count = 0;
    for (int i = 0; i < MAX_DISKS; i++) {
        if (g_mgr.entries[i].is_active) {
            fprintf(f, "%s %lluM %c %s\n",
                    g_mgr.entries[i].type == DISK_TYPE_VRAM ? "vram" : "ram",
                    (unsigned long long)(g_mgr.entries[i].size_bytes / (1024 * 1024)),
                    g_mgr.entries[i].drive_letter,
                    g_mgr.entries[i].fs_name[0] ? g_mgr.entries[i].fs_name : "NTFS");
            count++;
        }
    }
    fclose(f);
    printf("[vdisk] Saved %d disk(s) to %s\n", count, config_file_path());
    return count;
}

int disk_mgr_linux(char drive_letter) {
    load_state();

    if (drive_letter) {
        drive_letter = (char)toupper((unsigned char)drive_letter);
        int idx = drive_letter - 'A';
        if (idx < 0 || idx >= MAX_DISKS || !g_mgr.entries[idx].is_active) {
            printf("[vdisk] No vdisk on %c:. Create one first, e.g. 'vdisk create ram 1G %c:'.\n",
                   drive_letter, drive_letter);
            return 0;
        }
    } else {
        // Prefer an existing vdisk; otherwise spin up a default RAM disk.
        drive_letter = 0;
        for (int i = 0; i < MAX_DISKS; i++) {
            if (g_mgr.entries[i].is_active) { drive_letter = g_mgr.entries[i].drive_letter; break; }
        }
        if (!drive_letter) {
            drive_letter = find_free_drive_letter();
            printf("[vdisk] No active vdisk -- creating a 1024 MB RAM disk on %c: for the session...\n",
                   drive_letter);
            if (!disk_mgr_create(0, (size_t)1024 * 1024 * 1024, drive_letter, "NTFS")) return 0;
        }
    }

    return run_linux_shell(drive_letter);
}

void disk_mgr_show_config(void) {
    printf("[vdisk] Config file: %s\n", config_file_path());
    FILE *f = fopen(config_file_path(), "r");
    if (!f) { printf("[vdisk] (does not exist yet -- use 'vdisk save' or create it)\n"); return; }
    printf("--------------------------------------------------------\n");
    char line[256];
    while (fgets(line, sizeof(line), f)) fputs(line, stdout);
    printf("--------------------------------------------------------\n");
    fclose(f);
}
