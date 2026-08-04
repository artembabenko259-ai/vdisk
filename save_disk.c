#define _CRT_SECURE_NO_WARNINGS
#include "save_disk.h"
#include "vdisk_util.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

static unsigned long long g_files;
static unsigned long long g_bytes;
static unsigned long long g_failed;

static int path_exists(const char *p) {
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}
static int path_is_dir(const char *p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Creates every directory along 'path'.
static void make_dirs(const char *path) {
    char tmp[MAX_PATH];
    strcpy_s(tmp, sizeof(tmp), path);
    size_t start = 0;
    if (tmp[0] && tmp[1] == ':') start = 3; // skip "X:\"
    for (size_t i = start; tmp[i]; i++) {
        if (tmp[i] == '\\' || tmp[i] == '/') {
            char c = tmp[i];
            tmp[i] = '\0';
            if (tmp[0]) CreateDirectoryA(tmp, NULL);
            tmp[i] = c;
        }
    }
    CreateDirectoryA(tmp, NULL);
}

static void copy_one_file(const char *src, const char *dst) {
    char parent[MAX_PATH];
    strcpy_s(parent, sizeof(parent), dst);
    char *slash = strrchr(parent, '\\');
    if (slash) { *slash = '\0'; make_dirs(parent); }

    if (CopyFileA(src, dst, FALSE)) {
        g_files++;
        HANDLE h = CreateFileA(dst, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER sz;
            if (GetFileSizeEx(h, &sz)) g_bytes += (unsigned long long)sz.QuadPart;
            CloseHandle(h);
        }
    } else {
        g_failed++;
        printf("[vdisk]  ! could not copy: %s\n", src);
    }
}

static void copy_dir(const char *src_dir, const char *dst_dir) {
    make_dirs(dst_dir);
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", src_dir);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        char s[MAX_PATH], d[MAX_PATH];
        snprintf(s, sizeof(s), "%s\\%s", src_dir, fd.cFileName);
        snprintf(d, sizeof(d), "%s\\%s", dst_dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copy_dir(s, d);
        else copy_one_file(s, d);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int disk_save(char L, const char *dest, const char **items, int item_count) {
    L = (char)toupper((unsigned char)L);
    if (L < 'A' || L > 'Z') { printf("[vdisk] Invalid disk letter.\n"); return 0; }
    if (!(GetLogicalDrives() & (1u << (L - 'A')))) {
        printf("[vdisk] No disk mounted at %c:.\n", L);
        return 0;
    }
    if (!dest || !*dest) { printf("[vdisk] Specify a destination folder.\n"); return 0; }

    g_files = g_bytes = g_failed = 0;
    make_dirs(dest);

    int whole = (item_count <= 0) || (item_count == 1 && strcmp(items[0], ".") == 0);

    if (whole) {
        printf("[vdisk] Saving ALL of %c:\\  ->  %s ...\n", L, dest);
        char src_root[8];
        snprintf(src_root, sizeof(src_root), "%c:", L); // "R:" -> pattern "R:\*"
        copy_dir(src_root, dest);
    } else {
        for (int i = 0; i < item_count; i++) {
            char s[MAX_PATH], d[MAX_PATH];
            snprintf(s, sizeof(s), "%c:\\%s", L, items[i]);
            snprintf(d, sizeof(d), "%s\\%s", dest, items[i]);
            if (!path_exists(s)) { printf("[vdisk]  ! not found on %c:: %s\n", L, items[i]); g_failed++; continue; }
            printf("[vdisk] Saving %s  ->  %s\n", s, d);
            if (path_is_dir(s)) copy_dir(s, d);
            else copy_one_file(s, d);
        }
    }

    printf("[vdisk] Done: %llu file(s), %.2f MB copied to %s%s.\n",
           g_files, (double)g_bytes / (1024.0 * 1024.0), dest,
           g_failed ? " (some items failed)" : "");
    return g_failed == 0;
}

// Parses e.g. "30sec", "10min", "hour", "2d" into seconds. A bare unit with
// no leading digits means 1. Returns 0 on an unrecognized unit.
static int parse_interval_seconds(const char *s, long *out_sec) {
    if (!s || !*s) return 0;
    char buf[64];
    strcpy_s(buf, sizeof(buf), s);

    char *p = buf;
    long num = 0;
    int has_digits = 0;
    while (*p && isdigit((unsigned char)*p)) { num = num * 10 + (*p - '0'); p++; has_digits = 1; }
    if (!has_digits) num = 1;
    for (char *u = p; *u; u++) *u = (char)tolower((unsigned char)*u);

    long mult;
    if (strcmp(p, "s") == 0 || strncmp(p, "sec", 3) == 0) mult = 1;
    else if (strcmp(p, "m") == 0 || strncmp(p, "min", 3) == 0) mult = 60;
    else if (strcmp(p, "h") == 0 || strncmp(p, "hour", 4) == 0) mult = 3600;
    else if (strcmp(p, "d") == 0 || strncmp(p, "day", 3) == 0) mult = 86400;
    else return 0;

    if (num <= 0) return 0;
    *out_sec = num * mult;
    return 1;
}

int disk_save_periodic(char L, const char *dest_arg, const char *interval_str) {
    L = (char)toupper((unsigned char)L);
    if (L < 'A' || L > 'Z') { printf("[vdisk] Invalid disk letter.\n"); return 0; }
    if (!(GetLogicalDrives() & (1u << (L - 'A')))) {
        printf("[vdisk] No disk mounted at %c:.\n", L);
        return 0;
    }

    long interval_sec = 0;
    if (!parse_interval_seconds(interval_str, &interval_sec)) {
        printf("[vdisk] Bad --ev interval '%s'. Use e.g. 30sec, 10min, hour, 2day.\n", interval_str);
        return 0;
    }

    char dest[MAX_PATH];
    if (dest_arg && *dest_arg) {
        strcpy_s(dest, sizeof(dest), dest_arg);
    } else {
        char datadir[MAX_PATH];
        if (!get_data_dir(datadir, sizeof(datadir))) { printf("[vdisk] Data dir error.\n"); return 0; }
        snprintf(dest, sizeof(dest), "%s\\autosave\\%c", datadir, L);
    }

    printf("[vdisk] Auto-saving %c:\\  ->  %s  every %ld sec.\n", L, dest, interval_sec);
    printf("[vdisk] NOT true persistence -- the disk is still wiped on reboot; this is a\n");
    printf("[vdisk] periodic snapshot. After remounting, copy %s\n", dest);
    printf("[vdisk] back onto the fresh disk to restore it.\n");
    printf("[vdisk] Leave this window running. Ctrl-C (or close it) to stop.\n\n");
    fflush(stdout);

    for (;;) {
        if (!(GetLogicalDrives() & (1u << (L - 'A')))) {
            printf("[vdisk] %c: is no longer mounted; stopping auto-save.\n", L);
            fflush(stdout);
            return 1;
        }
        time_t now = time(NULL);
        struct tm tmv;
        char tbuf[16] = {0};
        if (localtime_s(&tmv, &now) == 0) strftime(tbuf, sizeof(tbuf), "%H:%M:%S", &tmv);
        printf("[vdisk] [%s] auto-save tick...\n", tbuf);
        disk_save(L, dest, NULL, 0);
        fflush(stdout);
        Sleep((DWORD)(interval_sec * 1000));
    }
}
