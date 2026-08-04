#ifndef QEMU_LINUX_H
#define QEMU_LINUX_H

// Boots a real Alpine Linux kernel in QEMU, headless, on the current Windows
// console (serial console, like a WSL2-style terminal). The vdisk mounted at
// 'drive_letter' provides a RAM/VRAM-backed data disk to the VM (/dev/vda).
// Downloads Alpine once (cached in the vdisk data dir). Uses software
// emulation (TCG), falling back automatically from hardware acceleration
// (whpx) if that isn't enabled -- see accel.h. No admin, no reboot needed for
// a plain shell. Blocks until the VM exits.
//
// 'tool_name' selects an apk package (curated recipes get extra setup, e.g.
// PostgreSQL; anything else installs as-is) to provision automatically before
// handing over the console; pass NULL for a plain shell. 'tools_net' selects
// the source: 0 = the small local boot-image repo only (no network needed),
// non-zero = the full network apk repos.
//
// 'want_share' mounts the disk's Data\ folder inside the guest at /mnt/win
// over SMB (host<->guest file bridge, WSL2-style). Requires Administrator
// (self-elevated by the caller before this is invoked) to create the SMB
// share the first time, and prompts on the console for the current Windows
// user's password each run (used only to authenticate that one mount, never
// stored). Either 'tool_name' or 'want_share' (or both) switch this function
// into automated mode; with neither, you get a plain, directly-interactive
// shell.
//
// Returns 1 on success, 0 on failure to launch or provision.
int qemu_run_linux(char drive_letter, const char *tool_name, int tools_net, int want_share);

#endif // QEMU_LINUX_H
