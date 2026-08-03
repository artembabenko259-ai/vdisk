#include "disk_manager.h"
#include "vram_allocator.h"
#include "vdisk_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static disk_manager_t g_mgr;

// State lives in a stable per-user location so it is consistent regardless of
// the working directory a command is launched from.
static const char *state_file_path(void) {
    static char path[MAX_PATH] = {0};
    if (path[0]) return path;
    char dir[MAX_PATH];
    if (get_data_dir(dir, sizeof(dir))) snprintf(path, sizeof(path), "%s\\vdisk_state.txt", dir);
    else strcpy_s(path, sizeof(path), "vdisk_state.txt");
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
        size_t size_b;
        DWORD pid;
        if (sscanf_s(line, "%c %15s %llu %lu",
                     &drive, (unsigned)1,
                     type_str, (unsigned)sizeof(type_str),
                     (unsigned long long *)&size_b, &pid) >= 4) {
            int idx = toupper((unsigned char)drive) - 'A';
            if (idx >= 0 && idx < MAX_DISKS) {
                g_mgr.entries[idx].drive_letter = (char)toupper((unsigned char)drive);
                g_mgr.entries[idx].type = (strcmp(type_str, "VRAM") == 0) ? DISK_TYPE_VRAM : DISK_TYPE_RAM;
                g_mgr.entries[idx].size_bytes = size_b;
                g_mgr.entries[idx].pid = pid;
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
            fprintf(f, "%c %s %llu %lu\n",
                    g_mgr.entries[i].drive_letter,
                    (g_mgr.entries[i].type == DISK_TYPE_VRAM) ? "VRAM" : "RAM",
                    (unsigned long long)g_mgr.entries[i].size_bytes,
                    g_mgr.entries[i].pid);
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
static DWORD spawn_worker(int use_vram, unsigned long long size_mb, char drive_letter) {
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof(exe));

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" --fs-worker %s %llu %c",
             exe, use_vram ? "vram" : "ram", size_mb, drive_letter);

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

int disk_mgr_create(int use_vram, size_t size_bytes, char drive_letter) {
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

    printf("[vdisk] Creating %s disk of %llu MB on drive %c: ...\n",
           use_vram ? "VRAM" : "RAM", size_mb, drive_letter);

    DWORD pid = spawn_worker(use_vram, size_mb, drive_letter);
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
    g_mgr.entries[idx].is_active = 1;
    save_state();

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

    printf("[vdisk] Removing disk %c: ...\n", drive_letter);
    kill_worker(g_mgr.entries[idx].pid);

    memset(&g_mgr.entries[idx], 0, sizeof(g_mgr.entries[idx]));
    save_state();

    printf("[vdisk] Disk %c: successfully removed!\n", drive_letter);
    return 1;
}

int disk_mgr_clear(void) {
    int count = 0;
    for (int i = 0; i < MAX_DISKS; i++) {
        if (g_mgr.entries[i].is_active) {
            printf("[vdisk] Removing disk %c: (PID %lu) ...\n",
                   g_mgr.entries[i].drive_letter, g_mgr.entries[i].pid);
            kill_worker(g_mgr.entries[i].pid);
            memset(&g_mgr.entries[i], 0, sizeof(g_mgr.entries[i]));
            count++;
        }
    }
    save_state();

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
