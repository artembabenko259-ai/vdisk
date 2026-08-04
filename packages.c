#define _CRT_SECURE_NO_WARNINGS
#include "packages.h"
#include "vdisk_util.h"
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#define MAX_PACKAGES 128
#define NAME_LEN 64

// A modest, generically-installable starter set (plain CLI tools, no special
// bootstrap needed) -- not the curated recipes like postgresql (those need
// real setup code, see qemu_linux.c, and work via --tools regardless of this
// list).
static const char *DEFAULTS[] = {
    "git", "curl", "wget", "htop", "vim", "tmux", "mc", "jq", "python3", "nodejs",
};
#define DEFAULTS_COUNT (sizeof(DEFAULTS) / sizeof(DEFAULTS[0]))

static const char *list_path(void) {
    static char path[MAX_PATH] = {0};
    if (path[0]) return path;
    char dir[MAX_PATH];
    if (get_data_dir(dir, sizeof(dir))) snprintf(path, sizeof(path), "%s\\packages.txt", dir);
    else strcpy_s(path, sizeof(path), "packages.txt");
    return path;
}

static int load(char names[MAX_PACKAGES][NAME_LEN]) {
    int n = 0;
    FILE *f = fopen(list_path(), "r");
    if (!f) return 0;
    char line[NAME_LEN];
    while (n < MAX_PACKAGES && fgets(line, sizeof(line), f)) {
        size_t len = strcspn(line, "\r\n");
        line[len] = '\0';
        if (len == 0) continue;
        strcpy_s(names[n], NAME_LEN, line);
        n++;
    }
    fclose(f);
    return n;
}

static void save(char names[MAX_PACKAGES][NAME_LEN], int n) {
    FILE *f = fopen(list_path(), "w");
    if (!f) return;
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", names[i]);
    fclose(f);
}

static int find(char names[MAX_PACKAGES][NAME_LEN], int n, const char *name) {
    for (int i = 0; i < n; i++)
        if (_stricmp(names[i], name) == 0) return i;
    return -1;
}

void packages_list(void) {
    char names[MAX_PACKAGES][NAME_LEN];
    int n = load(names);
    printf("\n=== Saved packages (vdisk linux -s <DRIVE> --tools \"<name>\") ===\n");
    if (n == 0) {
        printf("(empty) -- add one with 'vdisk linux --package add <name>',\n");
        printf("or load a starter set with 'vdisk linux --package set-defaults'.\n");
    } else {
        for (int i = 0; i < n; i++) printf("  %s\n", names[i]);
    }
    printf("==================================================================\n\n");
}

void packages_add(const char *name) {
    if (!name || !*name) { printf("[vdisk] Specify a package name.\n"); return; }
    char names[MAX_PACKAGES][NAME_LEN];
    int n = load(names);
    if (find(names, n, name) >= 0) {
        printf("[vdisk] '%s' is already in the list.\n", name);
        return;
    }
    if (n >= MAX_PACKAGES) {
        printf("[vdisk] List is full (%d entries).\n", MAX_PACKAGES);
        return;
    }
    strcpy_s(names[n], NAME_LEN, name);
    n++;
    save(names, n);
    printf("[vdisk] Added '%s'.\n", name);
}

void packages_del(const char *name) {
    if (!name || !*name) { printf("[vdisk] Specify a package name.\n"); return; }
    char names[MAX_PACKAGES][NAME_LEN];
    int n = load(names);
    int idx = find(names, n, name);
    if (idx < 0) {
        printf("[vdisk] '%s' is not in the list.\n", name);
        return;
    }
    for (int i = idx; i < n - 1; i++) strcpy_s(names[i], NAME_LEN, names[i + 1]);
    n--;
    save(names, n);
    printf("[vdisk] Removed '%s'.\n", name);
}

void packages_set_defaults(void) {
    char names[MAX_PACKAGES][NAME_LEN];
    int n = load(names);
    int added = 0;
    for (size_t d = 0; d < DEFAULTS_COUNT && n < MAX_PACKAGES; d++) {
        if (find(names, n, DEFAULTS[d]) < 0) {
            strcpy_s(names[n], NAME_LEN, DEFAULTS[d]);
            n++;
            added++;
        }
    }
    save(names, n);
    printf("[vdisk] Added %d default package(s) to the list.\n", added);
}
