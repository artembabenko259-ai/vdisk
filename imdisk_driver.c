#include "imdisk_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_imdisk_exe_path[MAX_PATH] = "imdisk.exe";

static int is_admin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin;
}

static int execute_cmd(const char *cmd) {
    printf("[ImDisk] Executing: %s\n", cmd);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    char cmd_copy[1024];
    strncpy(cmd_copy, cmd, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    if (!CreateProcessA(NULL, cmd_copy, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        printf("[ImDisk] CreateProcess failed (%lu)\n", GetLastError());
        return 0;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (exit_code == 0);
}

int imdisk_is_installed() {
    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    if (!hSCM) return 0;

    SC_HANDLE hService = OpenServiceA(hSCM, "ImDisk", SERVICE_QUERY_STATUS);
    if (!hService) {
        CloseServiceHandle(hSCM);
        return 0;
    }

    SERVICE_STATUS status;
    int running = 0;
    if (QueryServiceStatus(hService, &status)) {
        running = (status.dwCurrentState == SERVICE_RUNNING);
    }

    CloseServiceHandle(hService);
    CloseServiceHandle(hSCM);
    return running;
}

int imdisk_ensure_installed() {
    // Determine path to imdisk.exe CLI tool
    if (GetFileAttributesA("C:\\Windows\\System32\\imdisk.exe") != INVALID_FILE_ATTRIBUTES) {
        snprintf(g_imdisk_exe_path, sizeof(g_imdisk_exe_path), "C:\\Windows\\System32\\imdisk.exe");
    } else if (GetFileAttributesA("C:\\Users\\User\\Desktop\\diskC\\cli\\amd64\\imdisk.exe") != INVALID_FILE_ATTRIBUTES) {
        snprintf(g_imdisk_exe_path, sizeof(g_imdisk_exe_path), "C:\\Users\\User\\Desktop\\diskC\\cli\\amd64\\imdisk.exe");
    } else {
        snprintf(g_imdisk_exe_path, sizeof(g_imdisk_exe_path), "imdisk.exe");
    }

    if (imdisk_is_installed()) {
        return 1;
    }

    if (!is_admin()) {
        printf("[ImDisk] NOTE: Installing kernel virtual disk driver on Windows requires Administrator rights.\n");
        printf("[ImDisk] Please run CMD or PowerShell as Administrator once (or run install_driver.bat as Admin).\n");
        return 0;
    }

    printf("[ImDisk] Installing driver files to System32...\n");
    CopyFileA("C:\\Users\\User\\Desktop\\diskC\\sys\\amd64\\imdisk.sys", "C:\\Windows\\System32\\drivers\\imdisk.sys", FALSE);
    CopyFileA("C:\\Users\\User\\Desktop\\diskC\\cli\\amd64\\imdisk.exe", "C:\\Windows\\System32\\imdisk.exe", FALSE);
    CopyFileA("C:\\Users\\User\\Desktop\\diskC\\svc\\amd64\\imdsksvc.exe", "C:\\Windows\\System32\\imdsksvc.exe", FALSE);
    CopyFileA("C:\\Users\\User\\Desktop\\diskC\\cpl\\amd64\\imdisk.cpl", "C:\\Windows\\System32\\imdisk.cpl", FALSE);

    snprintf(g_imdisk_exe_path, sizeof(g_imdisk_exe_path), "C:\\Windows\\System32\\imdisk.exe");

    SC_HANDLE hSCM = OpenSCManagerA(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (hSCM) {
        SC_HANDLE hService = OpenServiceA(hSCM, "ImDisk", SERVICE_ALL_ACCESS);
        if (!hService) {
            hService = CreateServiceA(
                hSCM, "ImDisk", "ImDisk Virtual Disk Driver",
                SERVICE_ALL_ACCESS, SERVICE_KERNEL_DRIVER, SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL, "C:\\Windows\\System32\\drivers\\imdisk.sys",
                NULL, NULL, NULL, NULL, NULL
            );
        }
        if (hService) {
            StartServiceA(hService, 0, NULL);
            CloseServiceHandle(hService);
        }

        SC_HANDLE hSvc = OpenServiceA(hSCM, "ImDskSvc", SERVICE_ALL_ACCESS);
        if (!hSvc) {
            hSvc = CreateServiceA(
                hSCM, "ImDskSvc", "ImDisk Virtual Disk Driver Helper",
                SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                SERVICE_ERROR_NORMAL, "C:\\Windows\\System32\\imdsksvc.exe",
                NULL, NULL, NULL, NULL, NULL
            );
        }
        if (hSvc) {
            StartServiceA(hSvc, 0, NULL);
            CloseServiceHandle(hSvc);
        }

        CloseServiceHandle(hSCM);
    }

    return imdisk_is_installed();
}

int imdisk_create_ram_disk(size_t size_bytes, char drive_letter, const char *fs_type) {
    if (!imdisk_ensure_installed()) {
        return 0;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" -a -s %llu -m %c: -p \"/fs:%s /q /y\"",
             g_imdisk_exe_path, (unsigned long long)size_bytes, drive_letter, fs_type ? fs_type : "NTFS");

    return execute_cmd(cmd);
}

int imdisk_create_vram_proxy(size_t size_bytes, char drive_letter, const char *pipe_name, const char *fs_type) {
    if (!imdisk_ensure_installed()) {
        return 0;
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" -a -t proxy -o pipe -f \"%s\" -s %llu -m %c: -p \"/fs:%s /q /y\"",
             g_imdisk_exe_path, pipe_name, (unsigned long long)size_bytes, drive_letter, fs_type ? fs_type : "NTFS");

    return execute_cmd(cmd);
}

int imdisk_remove_disk(char drive_letter) {
    if (!imdisk_ensure_installed()) {
        return 0;
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "\"%s\" -D -m %c:", g_imdisk_exe_path, drive_letter);

    return execute_cmd(cmd);
}
