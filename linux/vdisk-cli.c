/*
 * vdisk - CLI for the vdisk_drv kernel module (RAM-backed block devices).
 * Talks to the driver purely through /proc/vdisk (create/remove commands,
 * plain-text listing on read) -- no ioctls, no shared header with the
 * kernel module.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#define PROC_PATH "/proc/vdisk"

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s create <name> <size>   Create a RAM disk (/dev/<name>)\n"
        "       %s remove <name>          Remove a RAM disk\n"
        "       %s list                   List active RAM disks\n"
        "\n"
        "SIZE accepts K/M/G suffixes (e.g. 512M, 1G, 2048M).\n"
        "Requires the vdisk_drv module to be loaded and read/write access\n"
        "to " PROC_PATH " (root, unless permissions were relaxed).\n",
        prog, prog, prog);
}

static int parse_size(const char *s, unsigned long long *out) {
    char *end;
    double val = strtod(s, &end);
    if (end == s || val < 0)
        return -1;

    unsigned long long mult = 1;
    if (*end) {
        switch (tolower((unsigned char)*end)) {
            case 'k': mult = 1024ULL; break;
            case 'm': mult = 1024ULL * 1024; break;
            case 'g': mult = 1024ULL * 1024 * 1024; break;
            default: return -1;
        }
        if (end[1] != '\0')
            return -1;
    }
    *out = (unsigned long long)(val * (double)mult);
    return 0;
}

static void human_size(unsigned long long v, char *buf, size_t buflen) {
    const char *units[] = {"B", "K", "M", "G", "T"};
    double d = (double)v;
    int u = 0;
    while (d >= 1024.0 && u < 4) { d /= 1024.0; u++; }
    if (u == 0) snprintf(buf, buflen, "%llu%s", v, units[u]);
    else snprintf(buf, buflen, "%.1f%s", d, units[u]);
}

static int do_create(const char *name, const char *size_str) {
    unsigned long long size_bytes;
    if (parse_size(size_str, &size_bytes) != 0 || size_bytes == 0) {
        fprintf(stderr, "vdisk: invalid size '%s'\n", size_str);
        return 1;
    }

    FILE *f = fopen(PROC_PATH, "w");
    if (!f) {
        fprintf(stderr, "vdisk: cannot open %s: %s (is the module loaded? are you root?)\n",
                PROC_PATH, strerror(errno));
        return 1;
    }
    // fopen()'d files are fully buffered by default, so a short write like
    // this one doesn't actually reach the kernel (and the -EBUSY/-EEXIST/
    // etc it might return) until the buffer is flushed -- which only
    // happens at fclose(). Checking ferror() right after fprintf() (as
    // this used to) always sees a clean stream and reports false success.
    int rc = fprintf(f, "create %s %llu\n", name, size_bytes);
    int close_err = fclose(f);
    if (rc < 0 || close_err != 0) {
        fprintf(stderr, "vdisk: failed to create '%s': %s\n", name, strerror(errno));
        return 1;
    }
    printf("[vdisk] Created RAM disk '%s' (%s) -> /dev/%s\n", name, size_str, name);
    return 0;
}

static int do_remove(const char *name) {
    FILE *f = fopen(PROC_PATH, "w");
    if (!f) {
        fprintf(stderr, "vdisk: cannot open %s: %s\n", PROC_PATH, strerror(errno));
        return 1;
    }
    int rc = fprintf(f, "remove %s\n", name);
    int close_err = fclose(f); // see do_create(): the real error surfaces here, not before
    if (rc < 0 || close_err != 0) {
        fprintf(stderr, "vdisk: failed to remove '%s': %s\n", name, strerror(errno));
        return 1;
    }
    printf("[vdisk] Removed '%s'\n", name);
    return 0;
}

static int do_list(void) {
    FILE *f = fopen(PROC_PATH, "r");
    if (!f) {
        fprintf(stderr, "vdisk: cannot open %s: %s (is the module loaded?)\n",
                PROC_PATH, strerror(errno));
        return 1;
    }

    char line[256];
    int header = 1;
    int any = 0;
    while (fgets(line, sizeof(line), f)) {
        if (header) { header = 0; continue; } /* skip kernel's raw header, we print our own */
        if (!strncmp(line, "commands:", 9)) break;

        char name[64];
        unsigned long long size_bytes, ram_used;
        if (sscanf(line, "%63s %llu %llu", name, &size_bytes, &ram_used) != 3)
            continue;
        if (!any) {
            printf("%-24s %10s %10s\n", "NAME", "SIZE", "RAM USED");
            any = 1;
        }
        char sz[32], ru[32];
        human_size(size_bytes, sz, sizeof(sz));
        human_size(ram_used, ru, sizeof(ru));
        printf("%-24s %10s %10s\n", name, sz, ru);
    }
    fclose(f);
    if (!any)
        printf("(no active vdisk RAM disks)\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    if (!strcmp(argv[1], "create")) {
        if (argc != 4) { usage(argv[0]); return 1; }
        return do_create(argv[2], argv[3]);
    } else if (!strcmp(argv[1], "remove")) {
        if (argc != 3) { usage(argv[0]); return 1; }
        return do_remove(argv[2]);
    } else if (!strcmp(argv[1], "list")) {
        return do_list();
    } else {
        usage(argv[0]);
        return 1;
    }
}
