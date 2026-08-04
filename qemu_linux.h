#ifndef QEMU_LINUX_H
#define QEMU_LINUX_H

// Boots a real Alpine Linux kernel in QEMU, headless, on the current Windows
// console (serial console, like a WSL2-style terminal). The vdisk mounted at
// 'drive_letter' provides a RAM/VRAM-backed data disk to the VM (/dev/vda).
// Downloads Alpine once (cached in the vdisk data dir). Uses software emulation
// (TCG) -- no admin, no reboot, but slow. Blocks until the VM exits.
//
// 'tool_name' selects an optional "tools" profile (e.g. "PostgreSQL") to
// provision automatically; pass NULL for a plain shell. 'tools_net' must be
// non-zero when 'tool_name' is set -- it is the only supported source right
// now: packages are fetched over the network via apk, before the interactive
// console is handed to the caller. The tool's data directory is tmpfs
// (the VM's own RAM), for maximum speed -- it does not persist across reboots.
//
// Returns 1 on success, 0 on failure to launch or provision.
int qemu_run_linux(char drive_letter, const char *tool_name, int tools_net);

#endif // QEMU_LINUX_H
