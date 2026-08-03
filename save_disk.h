#ifndef SAVE_DISK_H
#define SAVE_DISK_H

// Copies data OUT of a (RAM/VRAM) vdisk to a real destination folder, so you can
// persist results before the disk is wiped. If 'item_count' is 0 (or the single
// item "."), the whole disk is copied; otherwise each item (a file or folder
// path relative to the disk root) is copied, preserving its relative structure.
// No admin required. Returns 1 on success.
int disk_save(char disk_letter, const char *dest, const char **items, int item_count);

#endif // SAVE_DISK_H
