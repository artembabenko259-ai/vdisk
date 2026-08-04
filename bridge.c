#define _CRT_SECURE_NO_WARNINGS
#include "bridge.h"
#include "vdisk_util.h"
#include <windows.h>
#include <conio.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <shellapi.h>

#define SHARE_NAME "vdiskBridge"

// Elevates JUST this one PowerShell invocation (one UAC prompt) rather than
// the whole vdisk.exe process: the caller (vdisk linux -s ... --share) must
// stay unelevated so it keeps seeing the vdisk RAM disk it's about to boot
// with -- an elevated process of the same user can't see drive letters
// mounted in the unprivileged session (confirmed empirically).
static int run_elevated_wait(const char *file, const char *params) {
    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "runas";
    sei.lpFile = file;
    sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExA(&sei)) return 0; // includes UAC being declined
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return code == 0;
}

int bridge_folder_path(char *out, size_t out_sz) {
    char dir[MAX_PATH];
    if (!get_data_dir(dir, sizeof(dir))) return 0;
    snprintf(out, out_sz, "%s\\Bridge", dir);
    CreateDirectoryA(out, NULL);
    return 1;
}

// Plain (unelevated) check: reading share info needs no admin rights, so this
// alone never triggers UAC.
static int share_exists(const char *name) {
    char params[128];
    snprintf(params, sizeof(params),
             "-NoProfile -Command \"if (Get-SmbShare -Name '%s' -ErrorAction SilentlyContinue) { exit 0 } else { exit 1 }\"",
             name);
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi = {0};
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "powershell %s", params);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

int bridge_ensure_share(char *share_name, size_t share_name_sz) {
    strcpy_s(share_name, share_name_sz, SHARE_NAME);

    char path[MAX_PATH];
    if (!bridge_folder_path(path, sizeof(path))) {
        printf("[vdisk] Could not create the bridge folder.\n");
        return 0;
    }

    // Already set up (e.g. from a previous run)? Skip elevation entirely --
    // this check alone needs no admin rights, so repeat uses of --share don't
    // prompt UAC once the share has been created the first time.
    if (share_exists(share_name)) return 1;

    char user[256] = "Everyone";
    DWORD userlen = sizeof(user);
    GetUserNameA(user, &userlen); // best-effort; falls back to "Everyone" on failure

    printf("[vdisk] First use of --share: creating the SMB share (needs Administrator, once)...\n");

    char params[900];
    snprintf(params, sizeof(params),
             "-NoProfile -Command \"New-SmbShare -Name '%s' -Path '%s' -FullAccess '%s' | Out-Null\"",
             share_name, path, user);

    if (!run_elevated_wait("powershell", params)) {
        printf("[vdisk] Failed to create the SMB share '%s' for %s (UAC declined, or an error).\n", share_name, path);
        return 0;
    }
    return 1;
}

int bridge_prompt_password(char *out, size_t out_sz) {
    char user[256] = "your Windows user";
    DWORD userlen = sizeof(user);
    GetUserNameA(user, &userlen);

    printf("[vdisk] Windows password for '%s' (to mount the bridge folder inside the VM; not stored): ", user);
    fflush(stdout);

    size_t i = 0;
    int ch;
    while ((ch = _getch()) != '\r' && ch != '\n' && ch != EOF) {
        if (ch == '\b' || ch == 127) {
            if (i > 0) { i--; printf("\b \b"); fflush(stdout); }
            continue;
        }
        if (i < out_sz - 1) { out[i++] = (char)ch; putchar('*'); fflush(stdout); }
    }
    out[i] = '\0';
    printf("\n");
    return i > 0;
}

void bridge_append_mount_cmd(char *cmd, size_t cmd_sz, const char *share_name,
                              const char *username, const char *password) {
    char extra[1300];
    // Credentials go in a short-lived file (not on the mount command line,
    // which would otherwise sit in `ps` output for as long as the process is
    // visible), and it's deleted immediately after use. The mount's own exit
    // code is saved so the caller can check whether it actually worked.
    snprintf(extra, sizeof(extra),
             "apk add --no-cache cifs-utils >/tmp/vdisk-cifs-install.log 2>&1; "
             "mkdir -p /mnt/win; "
             "printf 'username=%s\\npassword=%s\\n' > /tmp/.smbcred; chmod 600 /tmp/.smbcred; "
             "mount -t cifs //10.0.2.2/%s /mnt/win -o credentials=/tmp/.smbcred,vers=3.0 >/tmp/vdisk-mount.log 2>&1; "
             "echo $? >/tmp/vdisk-mount.rc; "
             "rm -f /tmp/.smbcred; ",
             username, password, share_name);
    strcat_s(cmd, cmd_sz, extra);
}
