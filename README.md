# vdisk - Temporary RAM & VRAM Disk CLI Utility

A command-line utility written in **C** for creating, managing, and removing
temporary **RAM disks** and **GPU VRAM disks** on Windows.

Disks created by `vdisk` appear as real Windows drive letters (`R:\`, `V:\`,
etc.) and work with File Explorer, CMD, PowerShell, and ordinary applications.
They are backed by a user-mode filesystem built on **[WinFsp](https://winfsp.dev)**,
so **no Administrator privileges are required** to mount or unmount them.

---

## Features

- **RAM Disks**: File contents are stored directly in system RAM.
- **GPU VRAM Disks**: File contents are stored in NVIDIA GPU VRAM via the **CUDA API** (`nvcuda.dll`), verified with bit-exact read-back.
- **No Admin Required**: WinFsp mounts drive letters in user mode — no UAC prompt.
- **Persistent While Mounted**: Each disk is served by a lightweight background worker process that lives until the disk is removed.
- **Portable**: Locates WinFsp via the registry and stores its state in `%LOCALAPPDATA%\vdisk`; runs from any folder.
- **Real Physical Disks**: `vdisk disk` attaches a RAM/VRAM-backed VHD via the native virtdisk API as a genuine `\\.\PhysicalDrive` — partitionable, testable, disposable — for safely exercising destructive disk tools.
- **Real Linux VM**: `vdisk linux -s <DRIVE>` boots a real Alpine Linux kernel in QEMU, headless in your console, using the vdisk as storage (no admin, no reboot).
- **Comprehensive CLI**: `create`, `disk`, `remove`, `clear`, `list`, `status`, `mount`, `autostart`, `linux`, and `help`.

---

## Install (easy)

Grab the [latest release](https://github.com/artembabenko259-ai/vdisk/releases/latest),
unzip it anywhere, and run **`install.bat`**. It installs the dependencies via
`winget` (WinFsp — required; QEMU — for the Linux VM) and adds `vdisk` to your
`PATH`. Then open a new terminal and run `vdisk help`.

```powershell
# or do it by hand:
winget install WinFsp.WinFsp                       # required
winget install SoftwareFreedomConservancy.QEMU     # for 'vdisk linux -s'
```

Everything else `vdisk` needs (the virtdisk API, and the BusyBox / Alpine
downloads) is built in or fetched automatically on first use.

---

## Prerequisites

- **[WinFsp](https://winfsp.dev)** must be installed (runtime is required to run;
  the Developer/SDK feature is required to build). Install with:
  ```powershell
  winget install WinFsp.WinFsp
  ```
  To build from source you also need the SDK headers/libs — reinstall selecting
  all features if `C:\Program Files (x86)\WinFsp\inc` is missing:
  ```powershell
  msiexec /i winfsp.msi ADDLOCAL=ALL
  ```
- An **NVIDIA GPU with drivers** (only needed for `vram` disks).

---

## Usage

```cmd
:: Show help & commands
vdisk help

:: Check hardware memory & GPU VRAM status
vdisk status

:: Create a 512 MB RAM disk on drive R:
vdisk create ram 512M R:

:: Create a 1 GB VRAM disk in GPU memory on drive V:
vdisk create vram 1024M V:

:: Create a 2 GB RAM disk on the next available drive letter
vdisk create ram 2G

:: Create a disk reported to Windows as FAT32 (default is NTFS)
vdisk create ram 512M R: -f FAT32

:: List all active mounted disks
vdisk list

:: Remove/unmount virtual disk R:
vdisk remove R:

:: Remove ALL vdisk-managed disks at once
vdisk clear
```

Sizes accept `K`, `M`, `G` suffixes (e.g. `512M`, `1G`, `2048M`). If no drive
letter is given, the next free one (from `Z:` down) is used. `-f <name>` sets
the filesystem name Windows reports for the drive (default `NTFS`).

---

## Auto-mount at login

Set up the disks you want once, remember them, and have them mounted
automatically every time you log in — no need to run the exe by hand:

```cmd
:: 1. Create the disks you want
vdisk create ram 1G R:
vdisk create vram 512M V:

:: 2. Remember the current set of disks
vdisk save

:: 3. Mount everything from the saved config in one shot (any time)
vdisk mount

:: 4. Have 'vdisk mount' run automatically at each login
vdisk autostart on          ::  autostart off  /  autostart status

:: Inspect / hand-edit the config
vdisk config
```

The config lives at `%LOCALAPPDATA%\vdisk\vdisk.conf`, one disk per line
(`<ram|vram> <size> <letter> [fsname]`), and can be edited directly. Autostart
uses the per-user `Run` registry key (no admin required).

---

## Real physical disk from RAM/VRAM (for testing disk tools)

A normal `vdisk` disk is a *filesystem* (drive letter) — great for file-level
work, but low-level disk utilities (partition managers, sector editors,
`diskpart clean`, disk testers) want a real **physical disk** (`\\.\PhysicalDriveN`,
visible in Disk Management). `vdisk disk` provides exactly that, backed by
RAM/VRAM, so you can throw destructive disk tools at a disposable disk without
risking real data.

```cmd
:: 1. Create a RAM (or VRAM) disk to hold the backing image
vdisk create ram 1G M:

:: 2. Attach a REAL raw physical disk backed by it (needs admin -> UAC)
vdisk disk M: 512M

::    -> a new raw disk appears in Disk Management / 'diskpart list disk'.
::    Partition it, format it, run your (dangerous) disk tools on it.

:: 3. When done: detach, then remove the backing RAM disk
vdisk disk remove M:
vdisk remove M:

:: list attached physical block disks
vdisk disk list
```

It works by creating a fixed VHD on the vdisk and attaching it with the native
Windows **virtdisk API** (`CreateVirtualDisk`/`AttachVirtualDisk`) — no
third-party driver. The disk is attached **raw** (unformatted) on purpose, so
you exercise partitioning/formatting yourself. `vdisk remove` / `vdisk clear`
refuse to drop a disk that still backs an attached physical disk, so you can't
orphan it by accident.

> Requires Administrator (attaching physical disks is a privileged operation);
> `vdisk disk` self-elevates. Detaching uses `DetachVirtualDisk`, so removal is
> clean — no leftover phantom disks.

---

## Linux shell on a disk

```cmd
:: Open a bare Unix (BusyBox) shell whose root "/" is a vdisk
vdisk linux R:      :: use disk R:
vdisk linux         :: use the first active disk, or auto-create a RAM disk
```

`vdisk linux` drops you into an interactive [BusyBox](https://frippery.org/busybox/)
`sh` (ash) shell — `ls`, `vi`, `grep`, `wget`, `tar`, `awk`, and ~175 other
applets — rooted on the RAM/VRAM disk (`/` maps to the drive). BusyBox is
downloaded once to `%LOCALAPPDATA%\vdisk` and copied onto the disk, so the whole
environment lives on the vdisk and vanishes when the disk is removed. No admin
required.

> This is a BusyBox userland (Unix-like tools), **not** a real Linux kernel — no
> `apt`/`apk`, and Linux ELF binaries won't run. For a real kernel, use
> `vdisk linux -s` below.

### Real Linux (QEMU)

```cmd
:: mount a vdisk as usual, then boot a real Alpine Linux in it
vdisk create ram 2G D:
vdisk linux -s D
```

`vdisk linux -s <DRIVE>` boots a **real Alpine Linux kernel** in QEMU, headless,
right in your Windows console (serial console, WSL2-style). Log in as `root`
(no password); the vdisk provides a RAM/VRAM-backed data disk to the VM as
`/dev/vda`. Type `poweroff` (or press `Ctrl-A` then `X`) to leave.

- Real kernel, `apk`, ELF binaries, networking — a genuine Linux VM.
- **No admin, no reboot** — uses QEMU software emulation (TCG).
- Alpine (~60 MB) is downloaded once into `%LOCALAPPDATA%\vdisk`.
- Requires QEMU: `winget install SoftwareFreedomConservancy.QEMU`.

> Because it is software emulation (TCG), boot takes ~1–2 minutes and it runs
> slowly. For near-native speed, enable "Windows Hypervisor Platform" (admin +
> reboot) — QEMU then uses `whpx` acceleration.

---

## How it works

Each disk is hosted by a detached worker process (`vdisk --fs-worker …`) that
runs an in-memory WinFsp/FUSE filesystem for the lifetime of the disk:

- `create` launches the worker, waits for the drive letter to appear, and records the worker's PID (shown in `vdisk list`, stored in `%LOCALAPPDATA%\vdisk\vdisk_state.txt`).
- `remove` / `clear` terminate the worker; WinFsp unmounts the drive automatically.

For `vram` disks, file data lives in GPU memory (allocated with `cuMemAlloc`);
reads and writes stream through the CUDA driver API. The CUDA primary context is
bound per worker thread so WinFsp's thread pool can access VRAM safely.

---

## Building from Source

### Prerequisites
- Visual Studio / MSVC C compiler (`cl.exe`)
- WinFsp with the SDK (see above)
- Windows 10/11 64-bit

### Build
Run `build.bat` (auto-detects MSVC via `vswhere` and WinFsp via the registry), or
compile manually:
```cmd
cl.exe /O2 /W3 /I "C:\Program Files (x86)\WinFsp\inc" ^
    main.c disk_manager.c vram_allocator.c vdisk_util.c fs_memfs.c ^
    "C:\Program Files (x86)\WinFsp\lib\winfsp-x64.lib" ^
    /Fe:vdisk.exe ^
    /link /DELAYLOAD:winfsp-x64.dll delayimp.lib advapi32.lib
```

---

## License

MIT License. See `LICENSE.md`.

WinFsp is a separate dependency installed by the user and is licensed under its
own terms (GPLv3 with a commercial option) — it is not redistributed here.
