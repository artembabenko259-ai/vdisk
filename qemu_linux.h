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
// 'want_share' mounts a bridge folder inside the guest at /mnt/win over SMB
// (host<->guest file bridge, WSL2-style; see bridge.h). Requires
// Administrator for a one-time SMB share setup (self-elevated narrowly,
// internally -- the caller must stay unelevated) and prompts on the console
// for the current Windows user's password each run (used only to
// authenticate that one mount, never stored).
//
// 'image_path' boots an arbitrary user-supplied ISO instead of Alpine (e.g. a
// different Linux distro, a rescue disk, anything with its own bootloader):
// confirmed empirically that QEMU's -nographic relays standard VGA text-mode
// output (SeaBIOS, isolinux/GRUB menus, kernel console), so you get a fully
// interactive console to whatever the image boots into -- but since it's an
// unknown OS/boot flow, this is NOT compatible with 'tool_name'/'tools_net'/
// 'want_share' (those assume our known Alpine login/shell). Pass NULL to use
// the built-in Alpine image as before.
//
// Any of 'tool_name', 'want_share', or 'image_path' switch this function into
// a mode that requires no further interaction to begin with (automated for
// the first two; a plain interactive console for the third); with none of
// them, you get a plain, directly-interactive Alpine shell.
//
// 'persist' installs a real (small) Alpine system onto the vdisk's data disk
// instead of just handing the VM a blank /dev/vda: the FIRST run formats it,
// installs alpine-base onto it (automated, local boot-image repo only, no
// network needed, ~30-60s), and every run after that boots straight from that
// disk -- so apk-installed packages and any files under / survive across
// separate 'vdisk linux -s <DRIVE> --persist' invocations, until the disk
// itself is removed ('vdisk remove <DRIVE>'). Mutually exclusive with
// 'tool_name'/'want_share'/'image_path' (v1 scope). Exit with 'poweroff' for
// a clean sync; killing the VM (Ctrl-A X) can lose the last unsynced writes,
// same as real hardware losing power.
//
// 'distro' selects what --persist installs: NULL or "alpine" (default) for
// the Alpine path above, or "debian" to debootstrap a real Debian (bookworm)
// userland onto the same disk instead (needs network for the one-time
// install; boots via the SAME Alpine kernel+initramfs either way -- the
// kernel doesn't care whose userland it switch_roots into). Passing "debian"
// implies persist=1 regardless of the 'persist' argument, since an installed
// distro other than Alpine only makes sense as a persistent disk.
//
// Returns 1 on success, 0 on failure to launch or provision.
int qemu_run_linux(char drive_letter, const char *tool_name, int tools_net,
                    int want_share, const char *image_path, int persist,
                    const char *distro);

#endif // QEMU_LINUX_H
