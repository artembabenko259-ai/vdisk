#ifndef ACCEL_H
#define ACCEL_H

// Reports whether Windows Hypervisor Platform (QEMU's WHPX accelerator) looks
// enabled, via a heuristic that needs no admin rights (driver service
// presence). Not authoritative -- 'vdisk accel enable' does the real check.
void accel_status(void);

// Enables the "Windows Hypervisor Platform" optional feature via DISM.
// Requires Administrator; caller must already be elevated (self-elevation is
// handled in main.c, matching 'vdisk disk'). Prompts to reboot now or later,
// since the feature only takes effect after a restart. Returns 1 on success.
int accel_enable(void);

#endif // ACCEL_H
