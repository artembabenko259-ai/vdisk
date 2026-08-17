#ifndef WHPX_VMM_H
#define WHPX_VMM_H

#include <windows.h>

// Checks whether Windows Hypervisor Platform is actually present and usable
// at runtime (calls WHvGetCapability directly -- authoritative, unlike
// accel.c's registry-presence heuristic which only guesses).
int whpx_vmm_available(void);

// Boots 'kernel_path' (a standard x86_64 bzImage, e.g. Alpine's
// vmlinuz-virt) + 'initrd_path' (cpio initramfs) directly via the Windows
// Hypervisor Platform API -- no QEMU, no BIOS, no ACPI, no device models
// beyond what's implemented here. 'img_path' backs a single virtio-blk
// device exposed to the guest as /dev/vda. 'cmdline' is the Linux kernel
// command line (the caller is responsible for including the matching
// 'virtio_mmio.device=...' parameter -- see whpx_vmm_mmio_cmdline_arg()).
//
// The guest's serial console (COM1/ttyS0) is bridged to hSerialIn (bytes
// read from here are fed to the guest as input) and hSerialOut (bytes the
// guest prints go here) -- deliberately the same shape as the pipes
// qemu_linux.c already builds to automate the QEMU child process, so the
// existing pipe_send()/wait_for_nth()/outbuf_t automation code works against
// this VMM unmodified.
//
// No virtio-net in v1 -- callers should skip network-dependent automation
// (the NFS/browse-drive live mount) when using this path; the plain
// login+interactive-shell flow works the same as before.
//
// Blocks until the guest powers off (ACPI-less direct boot has no real
// "poweroff" signal to catch, so callers should still treat this the same
// way as a QEMU child process exiting: the interactive shell's own
// 'poweroff' triggers a guest-side halt loop, which this function reports
// back as a clean return once observed).
//
// Returns 1 on success (VM booted and ran), 0 on failure to launch (WHPX
// unavailable, kernel/initrd/image couldn't be loaded, partition setup
// failed). Failure details are printed to stdout/stderr directly.
int whpx_vmm_run(const char *kernel_path, const char *initrd_path,
                  const char *img_path, const char *cmdline,
                  unsigned long long ram_mb,
                  HANDLE hSerialIn, HANDLE hSerialOut);

#endif // WHPX_VMM_H
