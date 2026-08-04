#ifndef SAVE_DISK_H
#define SAVE_DISK_H

// Copies data OUT of a (RAM/VRAM) vdisk to a real destination folder, so you can
// persist results before the disk is wiped. If 'item_count' is 0 (or the single
// item "."), the whole disk is copied; otherwise each item (a file or folder
// path relative to the disk root) is copied, preserving its relative structure.
// No admin required. Returns 1 on success.
int disk_save(char disk_letter, const char *dest, const char **items, int item_count);

// Repeats a whole-disk disk_save() forever, every 'interval_str' (e.g.
// "30sec", "10min", "hour", "day" -- a bare unit with no leading number means
// 1). 'dest' may be NULL/empty, which picks a default folder under the vdisk
// data dir (%LOCALAPPDATA%\vdisk\autosave\<DRIVE>). This is NOT real
// persistence -- the RAM disk is still wiped on reboot -- it's a periodic
// snapshot to a real folder that you restore from after remounting. Blocks
// until the disk is unmounted or the process is interrupted (Ctrl-C).
// Returns 1 normally (stopped because the disk went away), 0 on a bad
// interval string.
int disk_save_periodic(char disk_letter, const char *dest, const char *interval_str);

#endif // SAVE_DISK_H
