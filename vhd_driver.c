#include "vhd_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BOOL enable_privilege(LPCSTR privilegeName) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return FALSE;
    }
    TOKEN_PRIVILEGES tp;
    LUID luid;
    if (!LookupPrivilegeValueA(NULL, privilegeName, &luid)) {
        CloseHandle(hToken);
        return FALSE;
    }
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL res = AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
    CloseHandle(hToken);
    return res && (GetLastError() == ERROR_SUCCESS);
}

int vhd_create_and_attach(const char *vhd_path, size_t size_bytes, char drive_letter) {
    enable_privilege("SeManageVolumePrivilege");

    size_t size_mb = size_bytes / (1024 * 1024);
    if (size_mb < 3) size_mb = 3; // Minimum VHD size for NTFS formatting

    char ps_cmd[2048];
    snprintf(ps_cmd, sizeof(ps_cmd),
        "powershell -NoProfile -ExecutionPolicy Bypass -Command \""
        "$vhd = '%s'; $size = %lluMB; $drive = '%c';"
        "New-VHD -Path $vhd -SizeBytes $size -Dynamic | Mount-VHD;"
        "Initialize-Disk -Passthru | New-Partition -DriveLetter $drive -UseMaximumSize | Format-Volume -FileSystem NTFS -Confirm:$false"
        "\"", vhd_path, (unsigned long long)size_mb, drive_letter);

    printf("[vhd] Mounting Windows VHD disk...\n");
    int res = system(ps_cmd);
    return (res == 0);
}

int vhd_detach(const char *vhd_path, char drive_letter) {
    char ps_cmd[1024];
    snprintf(ps_cmd, sizeof(ps_cmd),
        "powershell -NoProfile -ExecutionPolicy Bypass -Command \""
        "Dismount-VHD -Path '%s' -ErrorAction SilentlyContinue"
        "\"", vhd_path);

    int res = system(ps_cmd);
    return (res == 0);
}
