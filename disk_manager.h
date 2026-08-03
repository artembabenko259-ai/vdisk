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
    char        drive_letter;
    disk_type_t type;
    size_t      size_bytes;
    DWORD       pid;          // PID of the WinFsp worker hosting this disk
    char        fs_name[16];  // reported filesystem name (e.g. "NTFS")
    int         is_active;
} disk_entry_t;

typedef struct {
    disk_entry_t entries[MAX_DISKS];
} disk_manager_t;

void disk_mgr_init(void);
int  disk_mgr_create(int use_vram, size_t size_bytes, char drive_letter, const char *fs_name);
int  disk_mgr_remove(char drive_letter);
int  disk_mgr_clear(void);
void disk_mgr_list(void);
void disk_mgr_status(void);

// Config-driven auto-mounting. The config file lists one disk per line:
//   <ram|vram> <size> <letter> [fsname]      e.g.  ram 512M R NTFS
int  disk_mgr_mount_config(void);   // create every disk listed in the config
int  disk_mgr_save_config(void);    // write the currently-active disks to config
void disk_mgr_show_config(void);    // print the config path and its contents

// Opens a BusyBox Unix shell rooted on a vdisk. Uses the given drive (must be an
// active vdisk), else the first active disk, else auto-creates a RAM disk.
int  disk_mgr_linux(char drive_letter);

size_t parse_size_string(const char *str);
char   find_free_drive_letter(void);

#endif // DISK_MANAGER_H
