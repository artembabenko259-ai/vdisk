#ifndef QEMU_LINUX_H
#define QEMU_LINUX_H

// Boots a real Alpine Linux kernel in QEMU, headless, on the current Windows
// console (serial console, like a WSL2-style terminal). The vdisk mounted at
// 'drive_letter' provides a RAM/VRAM-backed data disk to the VM (/dev/vda).
// Downloads Alpine once (cached in the vdisk data dir). Uses software emulation
// (TCG) -- no admin, no reboot, but slow. Blocks until the VM exits.
// Returns 1 on success, 0 on failure to launch.
int qemu_run_linux(char drive_letter);

#endif // QEMU_LINUX_H
