/*
 * vdisk - CLI for the vdisk_drv kernel module (RAM-backed block devices).
 * Talks to the driver purely through /proc/vdisk (create/remove commands,
 * plain-text listing on read) -- no ioctls, no shared header with the
 * kernel module.
 *
 * By default `create` also mkfs.ext4's and mounts the new disk under
 * /mnt/vdisk/<name>, so it's immediately usable the same way a Windows
 * vdisk shows up as a ready drive letter; `--raw` skips that and leaves a
 * bare block device for anyone who wants to partition/format it by hand.
 *
 * External commands (mkfs.ext4, systemctl) are always run via execv with
 * an explicit argv array, never through a shell -- a disk/item name can't
 * inject anything into them regardless of what characters it contains.
 */

#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>

#define PROC_PATH    "/proc/vdisk"
#define MOUNT_BASE   "/mnt/vdisk"
#define CONFIG_DIR   "/etc/vdisk"
#define CONFIG_PATH  "/etc/vdisk/vdisk.conf"
#define SYSTEMD_UNIT "/etc/systemd/system/vdisk.service"
#define NAME_MAX_LEN 24

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s create <name> <size> [--raw]   Create a RAM disk\n"
        "       %s remove <name>                  Remove a RAM disk\n"
        "       %s list                           List active RAM disks\n"
        "       %s clear                          Remove ALL RAM disks\n"
        "       %s status                         Show RAM / vdisk usage\n"
        "       %s save                           Remember current disks for autostart\n"
        "       %s save <name> <dest> [items...]  Copy data off a disk\n"
        "       %s mount                          (Re)create disks from the saved config\n"
        "       %s autostart on|off|status         Recreate saved disks at boot (systemd)\n"
        "\n"
        "SIZE accepts K/M/G suffixes (e.g. 512M, 1G, 2048M). Without --raw, a\n"
        "new disk is formatted ext4 and mounted at " MOUNT_BASE "/<name>.\n"
        "Requires the vdisk_drv module to be loaded and root.\n",
        prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ---- small helpers ---- */

static int valid_name(const char *name) {
    size_t len = strlen(name);
    if (len == 0 || len > NAME_MAX_LEN) return 0;
    if (!strcmp(name, ".") || !strcmp(name, "..")) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '_' && c != '-') return 0;
    }
    return 1;
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

/* Runs argv[0] to completion (no shell involved), returns its exit status
 * (0-255) or -1 if it couldn't even be started/waited for. */
static int run_argv(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
}

/* Looks up /proc/mounts for a mount of /dev/<name>; fills mountpoint and
 * returns 1 if found, 0 if not mounted anywhere. */
static int device_mountpoint(const char *name, char *out, size_t outcap) {
    char devpath[64];
    snprintf(devpath, sizeof(devpath), "/dev/%s", name);

    FILE *f = fopen("/proc/mounts", "r");
    if (!f) return 0;
    char line[512], dev[256], mnt[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%255s %255s", dev, mnt) == 2 && !strcmp(dev, devpath)) {
            snprintf(out, outcap, "%s", mnt);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Blocks (briefly) until /dev/<name> exists -- add_disk() returning
 * doesn't guarantee udev has created the node yet. */
static int wait_for_devnode(const char *name) {
    char devpath[64];
    snprintf(devpath, sizeof(devpath), "/dev/%s", name);
    for (int i = 0; i < 200; i++) { /* up to ~2s */
        if (access(devpath, F_OK) == 0) return 1;
        usleep(10000);
    }
    return 0;
}

/* ---- create / remove ---- */

static int proc_write(const char *cmd) {
    FILE *f = fopen(PROC_PATH, "w");
    if (!f) {
        fprintf(stderr, "vdisk: cannot open %s: %s (is the module loaded? are you root?)\n",
                PROC_PATH, strerror(errno));
        return -1;
    }
    // fopen()'d files are fully buffered by default, so a short command
    // like this doesn't actually reach the kernel (and whatever -EBUSY/
    // -EEXIST/etc it returns) until the buffer is flushed -- which only
    // happens at fclose(). Checking ferror() right after fprintf() (as
    // this used to) always sees a clean stream and reports false success.
    int rc = fprintf(f, "%s", cmd);
    int close_err = fclose(f);
    if (rc < 0 || close_err != 0) {
        errno = close_err ? errno : EIO;
        return -1;
    }
    return 0;
}

static int create_disk_bytes(const char *name, unsigned long long size_bytes, int raw) {
    if (!valid_name(name)) {
        fprintf(stderr, "vdisk: invalid name '%s' (alnum/_/- only, max %d chars)\n",
                name, NAME_MAX_LEN);
        return 1;
    }
    if (size_bytes == 0) {
        fprintf(stderr, "vdisk: invalid size\n");
        return 1;
    }

    char cmd[96];
    snprintf(cmd, sizeof(cmd), "create %s %llu\n", name, size_bytes);
    if (proc_write(cmd) != 0) {
        fprintf(stderr, "vdisk: failed to create '%s': %s\n", name, strerror(errno));
        return 1;
    }

    if (raw) {
        printf("[vdisk] Created RAM disk '%s' -> /dev/%s (raw, not formatted)\n", name, name);
        return 0;
    }

    /* From here on, any failure rolls back the disk we just created
     * (best-effort) via the single cleanup path at the bottom. */
    const char *fail_reason = NULL;
    char devpath[64], mountpoint[128];
    snprintf(devpath, sizeof(devpath), "/dev/%s", name);
    snprintf(mountpoint, sizeof(mountpoint), MOUNT_BASE "/%s", name);

    if (!wait_for_devnode(name)) {
        fail_reason = "device node never appeared (udev not running?)";
        goto rollback;
    }

    char *mkfs_argv[] = { "mkfs.ext4", "-q", devpath, NULL };
    if (run_argv(mkfs_argv) != 0) {
        fail_reason = "mkfs.ext4 failed";
        goto rollback;
    }

    mkdir(MOUNT_BASE, 0755);
    if (mkdir(mountpoint, 0755) != 0 && errno != EEXIST) {
        fail_reason = "could not create mount directory";
        goto rollback;
    }

    if (mount(devpath, mountpoint, "ext4", 0, NULL) != 0) {
        fail_reason = "mount failed";
        goto rollback;
    }

    printf("[vdisk] Created RAM disk '%s' -> %s (mounted at %s)\n", name, devpath, mountpoint);
    return 0;

rollback:
    fprintf(stderr, "vdisk: %s; removing '%s'\n", fail_reason, name);
    {
        char rmcmd[64];
        snprintf(rmcmd, sizeof(rmcmd), "remove %s\n", name);
        proc_write(rmcmd);
    }
    return 1;
}

static int do_create(const char *name, const char *size_str, int raw) {
    unsigned long long size_bytes;
    if (parse_size(size_str, &size_bytes) != 0 || size_bytes == 0) {
        fprintf(stderr, "vdisk: invalid size '%s'\n", size_str);
        return 1;
    }
    return create_disk_bytes(name, size_bytes, raw);
}

static int do_remove(const char *name) {
    if (!valid_name(name)) {
        fprintf(stderr, "vdisk: invalid name '%s'\n", name);
        return 1;
    }

    char mountpoint[256];
    if (device_mountpoint(name, mountpoint, sizeof(mountpoint))) {
        if (umount(mountpoint) != 0) {
            fprintf(stderr, "vdisk: cannot unmount %s: %s (files still open?)\n",
                    mountpoint, strerror(errno));
            return 1;
        }
        /* Only clean up the directory if it's one we would have made
         * ourselves; leave anything else (a user-chosen mountpoint) alone. */
        char ours[128];
        snprintf(ours, sizeof(ours), MOUNT_BASE "/%s", name);
        if (!strcmp(mountpoint, ours))
            rmdir(ours);
    }

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "remove %s\n", name);
    if (proc_write(cmd) != 0) {
        fprintf(stderr, "vdisk: failed to remove '%s': %s\n", name, strerror(errno));
        return 1;
    }
    printf("[vdisk] Removed '%s'\n", name);
    return 0;
}

/* ---- listing helpers shared by list/clear/save/status ---- */

typedef struct { char name[64]; unsigned long long size_bytes, ram_used; } disk_row_t;

/* Returns the number of disks parsed, filling rows (capacity max_rows). */
static int read_disks(disk_row_t *rows, int max_rows) {
    FILE *f = fopen(PROC_PATH, "r");
    if (!f) return -1;
    char line[256];
    int header = 1, n = 0;
    while (fgets(line, sizeof(line), f) && n < max_rows) {
        if (header) { header = 0; continue; }
        if (!strncmp(line, "commands:", 9)) break;
        if (sscanf(line, "%63s %llu %llu", rows[n].name, &rows[n].size_bytes, &rows[n].ram_used) == 3)
            n++;
    }
    fclose(f);
    return n;
}

static int do_list(void) {
    disk_row_t rows[256];
    int n = read_disks(rows, 256);
    if (n < 0) {
        fprintf(stderr, "vdisk: cannot open %s: %s (is the module loaded?)\n",
                PROC_PATH, strerror(errno));
        return 1;
    }
    if (n == 0) {
        printf("(no active vdisk RAM disks)\n");
        return 0;
    }
    printf("%-24s %10s %10s  %s\n", "NAME", "SIZE", "RAM USED", "MOUNTED AT");
    for (int i = 0; i < n; i++) {
        char sz[32], ru[32], mnt[256] = "(not mounted)";
        human_size(rows[i].size_bytes, sz, sizeof(sz));
        human_size(rows[i].ram_used, ru, sizeof(ru));
        device_mountpoint(rows[i].name, mnt, sizeof(mnt));
        printf("%-24s %10s %10s  %s\n", rows[i].name, sz, ru, mnt);
    }
    return 0;
}

static int do_clear(void) {
    disk_row_t rows[256];
    int n = read_disks(rows, 256);
    if (n < 0) {
        fprintf(stderr, "vdisk: cannot open %s: %s\n", PROC_PATH, strerror(errno));
        return 1;
    }
    if (n == 0) {
        printf("(nothing to remove)\n");
        return 0;
    }
    int failures = 0;
    for (int i = 0; i < n; i++)
        if (do_remove(rows[i].name) != 0)
            failures++;
    if (failures)
        fprintf(stderr, "vdisk: %d disk(s) could not be removed\n", failures);
    return failures ? 1 : 0;
}

static int do_status(void) {
    disk_row_t rows[256];
    int n = read_disks(rows, 256);

    unsigned long long mem_total_kb = 0, mem_avail_kb = 0;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof(line), f)) {
            sscanf(line, "MemTotal: %llu kB", &mem_total_kb);
            sscanf(line, "MemAvailable: %llu kB", &mem_avail_kb);
        }
        fclose(f);
    }

    char mt[32], ma[32];
    human_size(mem_total_kb * 1024, mt, sizeof(mt));
    human_size(mem_avail_kb * 1024, ma, sizeof(ma));
    printf("System RAM:  %s total, %s available\n", mt, ma);

    if (n < 0) {
        printf("vdisk:       module not loaded\n");
        return 0;
    }

    unsigned long long total_used = 0, total_capacity = 0;
    for (int i = 0; i < n; i++) {
        total_used += rows[i].ram_used;
        total_capacity += rows[i].size_bytes;
    }
    char tu[32], tc[32];
    human_size(total_used, tu, sizeof(tu));
    human_size(total_capacity, tc, sizeof(tc));
    printf("vdisk disks: %d active, %s RAM used (%s capacity provisioned)\n", n, tu, tc);
    return 0;
}

/* ---- save: remember config, or copy data off a disk ---- */

static int save_remember_config(void) {
    disk_row_t rows[256];
    int n = read_disks(rows, 256);
    if (n < 0) {
        fprintf(stderr, "vdisk: cannot open %s: %s\n", PROC_PATH, strerror(errno));
        return 1;
    }
    mkdir(CONFIG_DIR, 0755);
    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        fprintf(stderr, "vdisk: cannot write %s: %s\n", CONFIG_PATH, strerror(errno));
        return 1;
    }
    for (int i = 0; i < n; i++)
        fprintf(f, "%s %llu\n", rows[i].name, rows[i].size_bytes);
    int close_err = fclose(f);
    if (close_err) {
        fprintf(stderr, "vdisk: failed writing %s: %s\n", CONFIG_PATH, strerror(errno));
        return 1;
    }
    printf("[vdisk] Remembered %d disk(s) in %s\n", n, CONFIG_PATH);
    return 0;
}

static int save_copy_off(const char *name, const char *dest, char **items, int nitems) {
    char mountpoint[256];
    if (!device_mountpoint(name, mountpoint, sizeof(mountpoint))) {
        fprintf(stderr, "vdisk: '%s' is not mounted, nothing to copy from "
                "(raw disks aren't supported by save)\n", name);
        return 1;
    }

    /* mkdir -p dest (dest is a real, user-supplied path -- one level of
     * mkdir is enough for the common case; deeper paths need their parents
     * to already exist, same as plain mkdir). */
    if (mkdir(dest, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "vdisk: cannot create '%s': %s\n", dest, strerror(errno));
        return 1;
    }

    char *argv[64];
    int ac = 0;
    argv[ac++] = "cp";
    argv[ac++] = "-a";
    argv[ac++] = "--";

    char srcbuf[64][300];
    if (nitems == 0) {
        snprintf(srcbuf[0], sizeof(srcbuf[0]), "%s/.", mountpoint);
        argv[ac++] = srcbuf[0];
    } else {
        for (int i = 0; i < nitems && i < 60; i++) {
            snprintf(srcbuf[i], sizeof(srcbuf[i]), "%s/%s", mountpoint, items[i]);
            argv[ac++] = srcbuf[i];
        }
    }
    argv[ac++] = (char *)dest;
    argv[ac] = NULL;

    int rc = run_argv(argv);
    if (rc != 0) {
        fprintf(stderr, "vdisk: copy from '%s' to '%s' failed (cp exit %d)\n", name, dest, rc);
        return 1;
    }
    printf("[vdisk] Copied '%s' -> %s\n", name, dest);
    return 0;
}

/* ---- mount: recreate disks from the saved config ---- */

static int do_mount(void) {
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        fprintf(stderr, "vdisk: no saved config at %s (run 'vdisk save' first)\n", CONFIG_PATH);
        return 1;
    }
    char line[128];
    int n = 0, failures = 0;
    while (fgets(line, sizeof(line), f)) {
        char name[64];
        unsigned long long size_bytes;
        if (sscanf(line, "%63s %llu", name, &size_bytes) != 2)
            continue;
        n++;
        int rc = create_disk_bytes(name, size_bytes, 0);
        /* Already existing (e.g. re-running 'vdisk mount') isn't a failure. */
        if (rc != 0 && errno != EEXIST)
            failures++;
    }
    fclose(f);
    printf("[vdisk] Processed %d disk(s) from %s\n", n, CONFIG_PATH);
    return failures ? 1 : 0;
}

/* ---- autostart: a systemd unit that runs 'vdisk mount' at boot ---- */

static int do_autostart(const char *action) {
    if (!strcmp(action, "status")) {
        char *argv[] = { "systemctl", "is-enabled", "vdisk.service", NULL };
        run_argv(argv);
        return 0;
    }
    if (!strcmp(action, "off")) {
        char *dis[] = { "systemctl", "disable", "--now", "vdisk.service", NULL };
        run_argv(dis);
        remove(SYSTEMD_UNIT);
        char *reload[] = { "systemctl", "daemon-reload", NULL };
        run_argv(reload);
        printf("[vdisk] Autostart disabled\n");
        return 0;
    }
    if (!strcmp(action, "on")) {
        char exe[256];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n < 0) { fprintf(stderr, "vdisk: cannot resolve own path: %s\n", strerror(errno)); return 1; }
        exe[n] = '\0';

        FILE *f = fopen(SYSTEMD_UNIT, "w");
        if (!f) {
            fprintf(stderr, "vdisk: cannot write %s: %s (are you root?)\n", SYSTEMD_UNIT, strerror(errno));
            return 1;
        }
        fprintf(f,
            "[Unit]\n"
            "Description=vdisk - recreate saved RAM disks\n"
            "After=local-fs.target\n"
            "\n"
            "[Service]\n"
            "Type=oneshot\n"
            "ExecStartPre=-/sbin/modprobe vdisk_drv\n"
            "ExecStart=%s mount\n"
            "RemainAfterExit=yes\n"
            "\n"
            "[Install]\n"
            "WantedBy=multi-user.target\n",
            exe);
        int close_err = fclose(f);
        if (close_err) {
            fprintf(stderr, "vdisk: failed writing %s: %s\n", SYSTEMD_UNIT, strerror(errno));
            return 1;
        }

        char *reload[] = { "systemctl", "daemon-reload", NULL };
        run_argv(reload);
        char *en[] = { "systemctl", "enable", "vdisk.service", NULL };
        if (run_argv(en) != 0) {
            fprintf(stderr, "vdisk: 'systemctl enable' failed\n");
            return 1;
        }
        printf("[vdisk] Autostart enabled (%s)\n", SYSTEMD_UNIT);
        return 0;
    }
    fprintf(stderr, "vdisk: autostart expects on|off|status\n");
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    if (!strcmp(argv[1], "create")) {
        if (argc < 4) { usage(argv[0]); return 1; }
        int raw = (argc >= 5 && !strcmp(argv[4], "--raw"));
        return do_create(argv[2], argv[3], raw);
    } else if (!strcmp(argv[1], "remove")) {
        if (argc != 3) { usage(argv[0]); return 1; }
        return do_remove(argv[2]);
    } else if (!strcmp(argv[1], "list")) {
        return do_list();
    } else if (!strcmp(argv[1], "clear")) {
        return do_clear();
    } else if (!strcmp(argv[1], "status")) {
        return do_status();
    } else if (!strcmp(argv[1], "save")) {
        if (argc == 2) return save_remember_config();
        if (argc >= 4) return save_copy_off(argv[2], argv[3], argv + 4, argc - 4);
        usage(argv[0]);
        return 1;
    } else if (!strcmp(argv[1], "mount")) {
        return do_mount();
    } else if (!strcmp(argv[1], "autostart")) {
        if (argc != 3) { usage(argv[0]); return 1; }
        return do_autostart(argv[2]);
    } else {
        usage(argv[0]);
        return 1;
    }
}
