#define _CRT_SECURE_NO_WARNINGS
#include "qemu_linux.h"
#include "vdisk_util.h"
#include <windows.h>
#include <urlmon.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

// Alpine "virt" ISO -- small, VM-tuned. Its kernel+initramfs (extracted from the
// ISO) boot headless to a serial login when given console=ttyS0, and the ISO
// itself (as a CD-ROM) provides the modloop + full userland.
#define ALPINE_ISO_URL \
    "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-virt-3.20.3-x86_64.iso"

#define DATA_IMG_MB 512

static int file_exists(const char *p) {
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

// Locate qemu-system-x86_64.exe: default install dir, then PATH.
static const char *qemu_path(void) {
    static char p[MAX_PATH] = {0};
    if (p[0]) return p;
    const char *cand = "C:\\Program Files\\qemu\\qemu-system-x86_64.exe";
    if (file_exists(cand)) { strcpy_s(p, sizeof(p), cand); return p; }
    char found[MAX_PATH];
    if (SearchPathA(NULL, "qemu-system-x86_64.exe", NULL, sizeof(found), found, NULL)) {
        strcpy_s(p, sizeof(p), found);
        return p;
    }
    p[0] = '\0';
    return p;
}

static int run_wait(const char *cmdline) {
    char buf[1024];
    strncpy_s(buf, sizeof(buf), cmdline, _TRUNCATE);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, buf, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

int qemu_run_linux(char L) {
    L = (char)toupper((unsigned char)L);
    if (L < 'A' || L > 'Z') {
        printf("[vdisk] Specify a drive: 'vdisk linux -s <DRIVE>' (mount it first with 'vdisk create ram 2G <DRIVE>').\n");
        return 0;
    }
    if (!(GetLogicalDrives() & (1u << (L - 'A')))) {
        printf("[vdisk] No vdisk mounted at %c:. Mount one first: 'vdisk create ram 2G %c:'.\n", L, L);
        return 0;
    }

    const char *q = qemu_path();
    if (!q[0]) {
        printf("[vdisk] QEMU not found. Install it: 'winget install SoftwareFreedomConservancy.QEMU'.\n");
        return 0;
    }

    char dir[MAX_PATH];
    if (!get_data_dir(dir, sizeof(dir))) { printf("[vdisk] Data dir error.\n"); return 0; }

    char iso[MAX_PATH], bootdir[MAX_PATH], kernel[MAX_PATH], initrd[MAX_PATH];
    snprintf(iso, sizeof(iso), "%s\\alpine-virt.iso", dir);
    snprintf(bootdir, sizeof(bootdir), "%s\\alpine-boot", dir);
    snprintf(kernel, sizeof(kernel), "%s\\boot\\vmlinuz-virt", bootdir);
    snprintf(initrd, sizeof(initrd), "%s\\boot\\initramfs-virt", bootdir);

    // Download Alpine ISO once.
    if (!file_exists(iso)) {
        printf("[vdisk] Downloading Alpine Linux (~60 MB, one time)...\n");
        if (URLDownloadToFileA(NULL, ALPINE_ISO_URL, iso, 0, NULL) != S_OK) {
            printf("[vdisk] Failed to download Alpine ISO.\n");
            return 0;
        }
    }

    // Extract the ISO's own kernel + initramfs (they know how to mount the CD).
    if (!file_exists(kernel) || !file_exists(initrd)) {
        printf("[vdisk] Preparing kernel (first run)...\n");
        CreateDirectoryA(bootdir, NULL);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "tar -xf \"%s\" -C \"%s\" boot", iso, bootdir);
        run_wait(cmd);
        if (!file_exists(kernel) || !file_exists(initrd)) {
            printf("[vdisk] Failed to extract the kernel from the ISO.\n");
            return 0;
        }
    }

    // A RAM/VRAM-backed data disk (on the vdisk) exposed to Linux as /dev/vda.
    char img[MAX_PATH];
    snprintf(img, sizeof(img), "%c:\\linux.img", L);
    int have_img = file_exists(img);
    if (!have_img) {
        printf("[vdisk] Creating a %d MB data disk %c:\\linux.img ...\n", DATA_IMG_MB, L);
        HANDLE hf = CreateFileA(img, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hf != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            sz.QuadPart = (LONGLONG)DATA_IMG_MB * 1024 * 1024;
            if (SetFilePointerEx(hf, sz, NULL, FILE_BEGIN) && SetEndOfFile(hf)) have_img = 1;
            CloseHandle(hf);
        }
        if (!have_img) {
            DeleteFileA(img);
            printf("[vdisk] (Not enough room on %c: for a data disk; booting without one.)\n", L);
        }
    }

    char cmd[2048];
    if (have_img) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -accel tcg -M pc -m 1024 -smp 2 -nographic "
                 "-kernel \"%s\" -initrd \"%s\" "
                 "-append \"console=ttyS0 modloop=/boot/modloop-virt quiet\" "
                 "-cdrom \"%s\" -drive file=\"%s\",format=raw,if=virtio",
                 q, kernel, initrd, iso, img);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -accel tcg -M pc -m 1024 -smp 2 -nographic "
                 "-kernel \"%s\" -initrd \"%s\" "
                 "-append \"console=ttyS0 modloop=/boot/modloop-virt quiet\" "
                 "-cdrom \"%s\"",
                 q, kernel, initrd, iso);
    }

    printf("\n");
    printf("=====================================================================\n");
    printf(" vdisk linux -s %c  --  REAL Alpine Linux in QEMU (headless console)\n", L);
    printf(" Login: root  (no password).%s\n",
           have_img ? "  Disk is /dev/vda inside Linux." : "");
    printf(" To leave: type 'poweroff'  (or press Ctrl-A then X to kill QEMU).\n");
    printf(" NOTE: software emulation (TCG) -- boot takes ~1-2 min and runs slowly.\n");
    printf("=====================================================================\n\n");
    fflush(stdout);

    // Inherit the console so the VM's serial console is fully interactive.
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("[vdisk] Failed to launch QEMU (error %lu).\n", GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    printf("\n[vdisk] Linux VM exited.\n");
    return 1;
}
