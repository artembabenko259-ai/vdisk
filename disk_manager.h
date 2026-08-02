#ifndef DISK_MANAGER_H
#define DISK_MANAGER_H

#include <windows.h>
#include <stddef.h>

#define MAX_DISKS 26

typedef enum {
    DISK_TYPE_RAM,
    DISK_TYPE_VRAM
} disk_type_t;

typedef struct {
    char drive_letter;
    disk_type_t type;
    size_t size_bytes;
    size_t vram_dptr;
    DWORD pid;
    int is_active;
} disk_entry_t;

typedef struct {
    disk_entry_t entries[MAX_DISKS];
    int count;
} disk_manager_t;

void disk_mgr_init();
int disk_mgr_create_ram(size_t size_bytes, char drive_letter);
int disk_mgr_create_vram(size_t size_bytes, char drive_letter);
int disk_mgr_remove(char drive_letter);
void disk_mgr_list();
void disk_mgr_status();

size_t parse_size_string(const char *str);
char find_free_drive_letter();

#endif // DISK_MANAGER_H
