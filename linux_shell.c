#include "linux_shell.h"
#include "vdisk_util.h"
#include <windows.h>
#include <urlmon.h>
#include <stdio.h>
#include <string.h>

// BusyBox for Windows (frippery.org) is a single self-contained multi-call
// binary; "busybox sh" gives an ash shell whose "/" is the current drive root.
#define BUSYBOX_URL  "https://frippery.org/files/busybox/busybox64.exe"

// Ensures busybox.exe is cached in the vdisk data directory, downloading it the
// first time. Fills 'out_path' with its full path.
static int ensure_busybox(char *out_path, size_t out_sz) {
    char dir[MAX_PATH];
    if (!get_data_dir(dir, sizeof(dir))) return 0;
    snprintf(out_path, out_sz, "%s\\busybox.exe", dir);

    if (GetFileAttributesA(out_path) != INVALID_FILE_ATTRIBUTES) return 1;

    printf("[vdisk] Fetching BusyBox (one-time download)...\n");
    if (URLDownloadToFileA(NULL, BUSYBOX_URL, out_path, 0, NULL) != S_OK) {
        printf("[vdisk] Could not download BusyBox.\n");
        printf("[vdisk] Install it with 'winget install frippery.busybox-w32',\n");
        printf("[vdisk] or place busybox.exe at:\n    %s\n", out_path);
        return 0;
    }
    return 1;
}

int run_linux_shell(char drive_letter) {
    char busybox[MAX_PATH];
    if (!ensure_busybox(busybox, sizeof(busybox))) return 0;

    char root[8];
    snprintf(root, sizeof(root), "%c:\\", drive_letter);

    // Copy BusyBox onto the disk so the whole environment lives there; fall back
    // to the cached copy if the disk is too small.
    char ondisk[MAX_PATH];
    snprintf(ondisk, sizeof(ondisk), "%c:\\busybox.exe", drive_letter);
    const char *exe = ondisk;
    if (!CopyFileA(busybox, ondisk, FALSE)) exe = busybox;

    printf("\n");
    printf("=====================================================================\n");
    printf(" vdisk linux -- BusyBox Unix shell on %c:\\   (\"/\" = %c:\\)\n", drive_letter, drive_letter);
    printf(" Type 'exit' to leave and return to Windows.\n");
    printf("=====================================================================\n\n");

    SetEnvironmentVariableA("HOME", "/");
    SetEnvironmentVariableA("PS1", "[vdisk] \\w \\$ ");
    fflush(stdout); // flush our banner before the child shares the console

    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof(cmd), "\"%s\" sh", exe);

    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    // Inherit the console so the shell is fully interactive; cwd = disk root.
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, root, &si, &pi)) {
        printf("[vdisk] Failed to launch shell (%lu)\n", GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    printf("\n[vdisk] Left the Linux shell (files on %c:\\ remain until the disk is removed).\n", drive_letter);
    return 1;
}
