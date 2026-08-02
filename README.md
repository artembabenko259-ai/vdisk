# vdisk - Temporary RAM & VRAM Disk CLI Utility

A high-performance command-line utility written in **C** for creating, managing, and removing temporary **RAM disks** and **GPU VRAM disks** on Windows.

Disks created by `vdisk` appear as real Windows block storage devices (`R:\`, `V:\`, etc.) and are fully compatible with File Explorer, CMD, PowerShell, and disk benchmarking/testing tools (CrystalDiskMark, HD Tune, DiskPart, Victoria, etc.).

---

## Features

- **RAM Disks**: Ultra-fast virtual disks created directly in System RAM.
- **GPU VRAM Disks**: Hardware-accelerated virtual disks allocated directly in VRAM on NVIDIA GPUs via **CUDA API** (`nvcuda.dll`).
- **Real Windows Block Drives**: Exposed as native SCSI/Block storage devices (`\\.\PhysicalDriveX` / Drive Letters) formatted with NTFS/FAT32.
- **Mandatory Administrator Privilege Enforcement**: Automatically detects privileges and prompts for UAC elevation if needed.
- **Comprehensive CLI**: Includes `create`, `remove`, `list`, `status`, and `help` commands.

---

## Installation & PATH

Add the directory containing `vdisk.exe` to your Windows `PATH` environment variable:
```powershell
[Environment]::SetEnvironmentVariable("PATH", $env:PATH + ";C:\path\to\vdisk", "User")
```

---

## Usage

> **Note:** Administrative privileges are required by Windows to mount/unmount block storage devices.

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

:: List all active mounted disks
vdisk list

:: Remove/unmount virtual disk R:
vdisk remove R:
```

---

## Building from Source

### Prerequisites
- Visual Studio / MSVC C Compiler (`cl.exe`)
- Windows 10/11 64-bit

### Build Command
Run `build.bat` or compile with `cl.exe`:
```cmd
cl.exe /O2 /W3 main.c vram_allocator.c vram_proxy.c imdisk_driver.c vhd_driver.c disk_manager.c advapi32.lib user32.lib shell32.lib virtdisk.lib /Fe:vdisk.exe
```

---

## License

MIT License.
