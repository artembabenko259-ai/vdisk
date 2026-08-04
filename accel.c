#define _CRT_SECURE_NO_WARNINGS
#include "accel.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>

// Runs 'cmdline', captures its combined stdout+stderr into 'out' (best
// effort, truncated to out_sz), and returns its exit code (or -1 on launch
// failure).
static int run_capture(const char *cmdline, char *out, size_t out_sz) {
    if (out && out_sz) out[0] = '\0';

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hReadPipe, hWritePipe;
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return -1;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
    PROCESS_INFORMATION pi = {0};

    char buf[2048];
    strncpy_s(buf, sizeof(buf), cmdline, _TRUNCATE);
    if (!CreateProcessA(NULL, buf, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(hReadPipe);
        CloseHandle(hWritePipe);
        return -1;
    }
    CloseHandle(hWritePipe);

    size_t used = 0;
    char chunk[512];
    DWORD n;
    while (ReadFile(hReadPipe, chunk, sizeof(chunk), &n, NULL) && n > 0) {
        if (out && used + n < out_sz) {
            memcpy(out + used, chunk, n);
            used += n;
            out[used] = '\0';
        }
    }
    CloseHandle(hReadPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

void accel_status(void) {
    // Querying the real feature state (DISM/Get-WindowsOptionalFeature) needs
    // admin rights even just to read it. As a no-admin hint, check whether the
    // Windows Hypervisor Platform driver is registered at all -- it isn't
    // until the feature has been enabled at least once.
    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    int driver_present = 0;
    if (hSCM) {
        SC_HANDLE hSvc = OpenServiceA(hSCM, "winhvr", SERVICE_QUERY_STATUS);
        if (hSvc) { driver_present = 1; CloseServiceHandle(hSvc); }
        CloseServiceHandle(hSCM);
    }

    printf("\n=== QEMU acceleration ===\n");
    if (driver_present) {
        printf("Windows Hypervisor Platform: looks ENABLED (whpx driver is registered).\n");
        printf("vdisk linux -s will use hardware acceleration automatically.\n");
    } else {
        printf("Windows Hypervisor Platform: looks DISABLED (no admin rights needed for this check,\n");
        printf("so this is a hint, not a guarantee).\n");
        printf("vdisk linux -s currently runs in software emulation (slow). Run 'vdisk accel enable'\n");
        printf("to turn on hardware acceleration (needs Administrator + a one-time reboot).\n");
    }
    printf("==========================\n\n");
}

int accel_enable(void) {
    printf("[vdisk] Enabling Windows Hypervisor Platform (this speeds up 'vdisk linux -s' a lot)...\n");

    char out[2048];
    int code = run_capture(
        "dism /online /Enable-Feature /FeatureName:HypervisorPlatform /All /NoRestart /Quiet",
        out, sizeof(out));

    if (code != 0 && code != 3010) { // 3010 = success, reboot required (normal here)
        printf("[vdisk] Failed to enable the feature (dism exit code %d).\n", code);
        if (out[0]) printf("[vdisk] dism output:\n%s\n", out);
        return 0;
    }

    printf("[vdisk] Windows Hypervisor Platform is enabled -- it needs a reboot to actually turn on.\n");
    printf("[vdisk] Reboot now? [y/N] ");
    fflush(stdout);

    int ch = getchar();
    if (ch == 'y' || ch == 'Y') {
        printf("[vdisk] Rebooting in 10 seconds... (Ctrl-C to cancel, then run 'shutdown /a')\n");
        char cmd[128];
        snprintf(cmd, sizeof(cmd), "shutdown /r /t 10 /c \"vdisk: enabling hardware VM acceleration\"");
        system(cmd);
    } else {
        printf("[vdisk] OK -- reboot whenever you like; 'vdisk linux -s' will pick up the\n");
        printf("[vdisk] acceleration automatically afterwards (falls back to software emulation until then).\n");
    }
    return 1;
}
