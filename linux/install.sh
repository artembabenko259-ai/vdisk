#!/bin/bash
# Installs vdisk_drv.ko into the running kernel's module tree and the
# vdisk CLI onto PATH, so 'vdisk create ...' works from anywhere and the
# module loads automatically on every boot (not just this session).
#
# Run as root (or via sudo). Rebuilds first if needed.
set -euo pipefail
cd "$(dirname "$0")"

if [ "$(id -u)" != "0" ]; then
    echo "vdisk: install.sh must run as root (sudo ./install.sh)" >&2
    exit 1
fi

echo "[install] Building..."
make module cli

KVER="$(uname -r)"
MODDIR="/lib/modules/${KVER}/extra"
mkdir -p "$MODDIR"
install -m 0644 vdisk_drv.ko "$MODDIR/vdisk_drv.ko"
depmod -a "$KVER"
echo "[install] Installed vdisk_drv.ko to $MODDIR"

install -m 0755 vdisk /usr/local/bin/vdisk
echo "[install] Installed CLI to /usr/local/bin/vdisk"

echo "vdisk_drv" > /etc/modules-load.d/vdisk.conf
echo "[install] Module will auto-load at boot (/etc/modules-load.d/vdisk.conf)"

if ! modprobe vdisk_drv 2>/tmp/vdisk-modprobe.log; then
    # Only expected on a kernel built/running without matching official
    # headers (e.g. a custom or vendor kernel without its own Module.symvers)
    # -- modprobe enforces the version-magic/modversion check strictly,
    # where a normal distro kernel with linux-headers installed wouldn't
    # hit this at all. insmod --force skips just that check; it taints the
    # kernel (visible in 'dmesg'/'cat /proc/sys/kernel/tainted') but the
    # module itself is unchanged.
    echo "[install] modprobe failed (see /tmp/vdisk-modprobe.log); this kernel" >&2
    echo "[install] likely has no matching Module.symvers -- forcing load instead:" >&2
    insmod -f "$MODDIR/vdisk_drv.ko"
fi
echo "[install] Loaded vdisk_drv now (major $(awk '/vdisk/{print $1}' /proc/devices))"

echo
echo "Done. Try: vdisk create test 64M"
echo "Recreate saved disks at boot too: vdisk save && vdisk autostart on"
