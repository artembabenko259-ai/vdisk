#include "disk_manager.h"
#include "vram_allocator.h"
#include "vram_proxy.h"
#include "imdisk_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *STATE_FILE = "vdisk_state.txt";

static disk_manager_t g_mgr;

static void load_state() {
    memset(&g_mgr, 0, sizeof(g_mgr));
    FILE *f = fopen(STATE_FILE, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char drive;
        char type_str[16];
        size_t size_b, dptr;
        DWORD pid;
        if (sscanf(line, "%c %15s %llu %llu %lu", &drive, type_str, (unsigned long long *)&size_b, (unsigned long long *)&dptr, &pid) >= 4) {
            int idx = toupper(drive) - 'A';
            if (idx >= 0 && idx < MAX_DISKS) {
                g_mgr.entries[idx].drive_letter = toupper(drive);
                g_mgr.entries[idx].type = (strcmp(type_str, "VRAM") == 0) ? DISK_TYPE_VRAM : DISK_TYPE_RAM;
                g_mgr.entries[idx].size_bytes = size_b;
                g_mgr.entries[idx].vram_dptr = dptr;
                g_mgr.entries[idx].pid = pid;
                g_mgr.entries[idx].is_active = 1;
            }
        }
    }
    fclose(f);
}

static void save_state() {
    FILE *f = fopen(STATE_FILE, "w");
    if (!f) return;

    for (int i = 0; i < MAX_DISKS; i++) {
        if (g_mgr.entries[i].is_active) {
            fprintf(f, "%c %s %llu %llu %lu\n",
                    g_mgr.entries[i].drive_letter,
                    (g_mgr.entries[i].type == DISK_TYPE_VRAM) ? "VRAM" : "RAM",
                    (unsigned long long)g_mgr.entries[i].size_bytes,
                    (unsigned long long)g_mgr.entries[i].vram_dptr,
                    g_mgr.entries[i].pid);
        }
    }
    fclose(f);
}

void disk_mgr_init() {
    load_state();
}

size_t parse_size_string(const char *str) {
    if (!str) return 0;
    char *endptr;
    double val = strtod(str, &endptr);
    if (val <= 0) return 0;

    while (*endptr && isspace(*endptr)) endptr++;

    if (toupper(*endptr) == 'G') {
        return (size_t)(val * 1024.0 * 1024.0 * 1024.0);
    } else if (toupper(*endptr) == 'M') {
        return (size_t)(val * 1024.0 * 1024.0);
    } else if (toupper(*endptr) == 'K') {
        return (size_t)(val * 1024.0);
    }
    return (size_t)val;
}

char find_free_drive_letter() {
    DWORD drives = GetLogicalDrives();
    // Try from Z down to D
    for (char c = 'Z'; c >= 'D'; c--) {
        int bit = c - 'A';
        if (!(drives & (1 << bit))) {
            return c;
        }
    }
    return 'R';
}

int disk_mgr_create_ram(size_t size_bytes, char drive_letter) {
    if (!drive_letter) drive_letter = find_free_drive_letter();
    drive_letter = toupper(drive_letter);

    printf("[vdisk] Creating RAM disk of size %.2f MB on drive %c: ...\n",
           (double)size_bytes / (1024.0 * 1024.0), drive_letter);

    if (!imdisk_create_ram_disk(size_bytes, drive_letter, "NTFS")) {
        printf("[vdisk] Failed to create RAM disk on drive %c:\n", drive_letter);
        return 0;
    }

    int idx = drive_letter - 'A';
    g_mgr.entries[idx].drive_letter = drive_letter;
    g_mgr.entries[idx].type = DISK_TYPE_RAM;
    g_mgr.entries[idx].size_bytes = size_bytes;
    g_mgr.entries[idx].vram_dptr = 0;
    g_mgr.entries[idx].pid = GetCurrentProcessId();
    g_mgr.entries[idx].is_active = 1;
    save_state();

    printf("[vdisk] SUCCESS! RAM disk created and mounted at drive %c:\\\n", drive_letter);
    return 1;
}

int disk_mgr_create_vram(size_t size_bytes, char drive_letter) {
    if (!drive_letter) drive_letter = find_free_drive_letter();
    drive_letter = toupper(drive_letter);

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

    printf("[vdisk] Allocating %.2f MB of GPU VRAM...\n", (double)size_bytes / (1024.0 * 1024.0));
    size_t dptr = vram_allocate(size_bytes);
    if (!dptr) {
        printf("[vdisk] Failed to allocate VRAM on GPU!\n");
        return 0;
    }

    char pipe_name[256];
    snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\vdisk_vram_%c", drive_letter);

    vram_proxy_server_t *server = (vram_proxy_server_t *)malloc(sizeof(vram_proxy_server_t));
    memset(server, 0, sizeof(vram_proxy_server_t));

    if (!vram_proxy_start(server, dptr, size_bytes, drive_letter)) {
        printf("[vdisk] Failed to start VRAM proxy server!\n");
        vram_free(dptr);
        free(server);
        return 0;
    }

    // Give proxy server a moment to start named pipe listener
    Sleep(200);

    if (!imdisk_create_vram_proxy(size_bytes, drive_letter, pipe_name, "NTFS")) {
        printf("[vdisk] Failed to mount VRAM disk on drive %c:\n", drive_letter);
        vram_proxy_stop(server);
        vram_free(dptr);
        free(server);
        return 0;
    }

    int idx = drive_letter - 'A';
    g_mgr.entries[idx].drive_letter = drive_letter;
    g_mgr.entries[idx].type = DISK_TYPE_VRAM;
    g_mgr.entries[idx].size_bytes = size_bytes;
    g_mgr.entries[idx].vram_dptr = dptr;
    g_mgr.entries[idx].pid = GetCurrentProcessId();
    g_mgr.entries[idx].is_active = 1;
    save_state();

    printf("[vdisk] SUCCESS! VRAM disk created and mounted at drive %c:\\\n", drive_letter);
    return 1;
}

int disk_mgr_remove(char drive_letter) {
    drive_letter = toupper(drive_letter);
    int idx = drive_letter - 'A';

    if (idx < 0 || idx >= MAX_DISKS || !g_mgr.entries[idx].is_active) {
        printf("[vdisk] Disk %c: is not currently tracked by vdisk. Attempting forced unmount...\n", drive_letter);
    }

    printf("[vdisk] Removing disk %c: ...\n", drive_letter);

    if (!imdisk_remove_disk(drive_letter)) {
        printf("[vdisk] ImDisk unmount returned warning/error for drive %c:\n", drive_letter);
    }

    if (g_mgr.entries[idx].is_active) {
        if (g_mgr.entries[idx].type == DISK_TYPE_VRAM && g_mgr.entries[idx].vram_dptr) {
            vram_free(g_mgr.entries[idx].vram_dptr);
            g_mgr.entries[idx].vram_dptr = 0;
        }
        g_mgr.entries[idx].is_active = 0;
        save_state();
    }

    printf("[vdisk] Disk %c: successfully removed!\n", drive_letter);
    return 1;
}

void disk_mgr_list() {
    load_state();
    printf("\n=== Active Temporary Virtual Disks (vdisk) ===\n");
    printf("%-8s %-8s %-12s %-16s %-10s\n", "Drive", "Type", "Size", "VRAM Address", "Status");
    printf("----------------------------------------------------\n");

    int count = 0;
    for (int i = 0; i < MAX_DISKS; i++) {
        if (g_mgr.entries[i].is_active) {
            count++;
            char size_buf[64];
            snprintf(size_buf, sizeof(size_buf), "%.2f MB", (double)g_mgr.entries[i].size_bytes / (1024.0 * 1024.0));

            char dptr_buf[64];
            if (g_mgr.entries[i].type == DISK_TYPE_VRAM) {
                snprintf(dptr_buf, sizeof(dptr_buf), "0x%llX", (unsigned long long)g_mgr.entries[i].vram_dptr);
            } else {
                snprintf(dptr_buf, sizeof(dptr_buf), "N/A (Kernel)");
            }

            printf("%c:       %-8s %-12s %-16s %-10s\n",
                   g_mgr.entries[i].drive_letter,
                   (g_mgr.entries[i].type == DISK_TYPE_VRAM) ? "VRAM" : "RAM",
                   size_buf, dptr_buf, "MOUNTED");
        }
    }

    if (count == 0) {
        printf("No active RAM/VRAM disks managed by vdisk.\n");
    }
    printf("----------------------------------------------------\n\n");
}

void disk_mgr_status() {
    printf("\n=== Hardware & Memory Status ===\n");
    
    // System RAM status
    MEMORYSTATUSEX memStatus;
    memStatus.dwLength = sizeof(memStatus);
    if (GlobalMemoryStatusEx(&memStatus)) {
        printf("System RAM: Free %.2f GB / Total %.2f GB\n",
               (double)memStatus.ullAvailPhys / (1024.0 * 1024.0 * 1024.0),
               (double)memStatus.ullTotalPhys / (1024.0 * 1024.0 * 1024.0));
    }

    // GPU VRAM status
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
