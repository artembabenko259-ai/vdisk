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
- **Comprehensive CLI**: `create`, `remove`, `clear`, `list`, `status`, and `help`.

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
