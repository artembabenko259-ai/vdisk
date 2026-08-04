#define _CRT_SECURE_NO_WARNINGS
#include "qemu_linux.h"
#include "vdisk_util.h"
#include "bridge.h"
#include <windows.h>
#include <urlmon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Alpine "virt" ISO -- small, VM-tuned. Its kernel+initramfs (extracted from the
// ISO) boot headless to a serial login when given console=ttyS0, and the ISO
// itself (as a CD-ROM) provides the modloop + full userland.
#define ALPINE_ISO_URL \
    "https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-virt-3.20.3-x86_64.iso"
#define ALPINE_REPO_MAIN      "https://dl-cdn.alpinelinux.org/alpine/v3.20/main"
#define ALPINE_REPO_COMMUNITY "https://dl-cdn.alpinelinux.org/alpine/v3.20/community"

#define DATA_IMG_MB 512
#define DEFAULT_VM_RAM_MB 1024

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

// ---------------------------------------------------------------------------
// --tools <name> --tools-net installs ANY apk package by that name -- "name"
// is passed straight to `apk add` for anything not listed below. Data lives
// on tmpfs (the VM's own RAM), not the vdisk-backed /dev/vda, so it runs at
// RAM speed with no virtio-blk/WinFsp overhead -- and vanishes with the VM,
// like everything else in this project.
//
// Plain CLI packages (git, htop, python3, ffmpeg, ...) need nothing beyond
// `apk add`; we also make a best-effort `rc-service <name> start` afterwards
// in case the package ships an OpenRC service, silently ignored if it
// doesn't. A handful of packages need real first-run bootstrap before they
// can start at all (initdb for Postgres, etc.) -- those get a curated
// "recipe" here instead of the generic path. Add more as needed.
// ---------------------------------------------------------------------------

typedef struct {
    const char *name;               // matched against --tools, case-insensitive
    unsigned long long vm_ram_mb;   // total VM RAM (must cover OS + packages + tmpfs data)
    const char *apk_packages;       // space-separated apk package list
    const char *provision_script;   // shell commands run after the packages install
} tool_recipe_t;

static const tool_recipe_t g_tool_recipes[] = {
    {
        "postgresql",
        1536,
        "postgresql16 postgresql16-contrib",
        // tmpfs for PGDATA: storage is already RAM-speed, so a large
        // shared_buffers would just double-cache the same bytes -- keep it lean.
        "mkdir -p /run/postgresql && chown postgres:postgres /run/postgresql; "
        "mount -t tmpfs -o size=512m tmpfs /var/lib/postgresql; "
        "mkdir -p /var/lib/postgresql/16/data && chown -R postgres:postgres /var/lib/postgresql; "
        "su postgres -c 'initdb -D /var/lib/postgresql/16/data -E UTF8' >/tmp/vdisk-initdb.log 2>&1; "
        // listen_addresses' value must be quoted in postgresql.conf syntax --
        // a bare '*' is a syntax error there (confirmed: this was the actual
        // reason pg_ctl couldn't start the server).
        "printf \"shared_buffers = 64MB\\nlisten_addresses = '*'\\n\" >> /var/lib/postgresql/16/data/postgresql.conf; "
        // Log inside the data dir (already owned by postgres) -- /var/log
        // belongs to root, so pg_ctl can't create its log file there. -t 120:
        // pg_ctl's default ~60s wait can be too short for postgres's own
        // startup under TCG's CPU emulation.
        "su postgres -c 'pg_ctl -D /var/lib/postgresql/16/data -l /var/lib/postgresql/16/data/postgresql.log -t 120 start' >/tmp/vdisk-pgctl.log 2>&1; "
    },
};
#define TOOL_RECIPE_COUNT (sizeof(g_tool_recipes) / sizeof(g_tool_recipes[0]))

static const tool_recipe_t *find_tool_recipe(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < TOOL_RECIPE_COUNT; i++)
        if (_stricmp(g_tool_recipes[i].name, name) == 0) return &g_tool_recipes[i];
    return NULL;
}

// ---------------------------------------------------------------------------
// A growable, thread-safe buffer that accumulates the VM's serial output, so
// the automation loop can search it for prompts/sentinels while a background
// thread keeps appending (and mirroring to the real console).
// ---------------------------------------------------------------------------

typedef struct {
    char *data;
    size_t len, cap;
    CRITICAL_SECTION lock;
} outbuf_t;

static void outbuf_init(outbuf_t *b) {
    b->cap = 64 * 1024;
    b->len = 0;
    b->data = (char *)malloc(b->cap);
    b->data[0] = '\0';
    InitializeCriticalSection(&b->lock);
}

static void outbuf_free(outbuf_t *b) {
    free(b->data);
    DeleteCriticalSection(&b->lock);
}

static void outbuf_append(outbuf_t *b, const char *p, size_t n) {
    EnterCriticalSection(&b->lock);
    if (b->len + n + 1 > b->cap) {
        size_t newcap = b->cap * 2;
        while (newcap < b->len + n + 1) newcap *= 2;
        b->data = (char *)realloc(b->data, newcap);
        b->cap = newcap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
    LeaveCriticalSection(&b->lock);
}

// Counts (non-overlapping) occurrences of 'needle'. Used to tell an echoed
// command (1st occurrence) apart from its real completion (2nd occurrence),
// since a sentinel we type ourselves gets echoed back before it ever runs.
static int outbuf_count(outbuf_t *b, const char *needle) {
    EnterCriticalSection(&b->lock);
    int count = 0;
    size_t nl = strlen(needle);
    const char *p = b->data;
    if (nl) {
        const char *hit;
        while ((hit = strstr(p, needle)) != NULL) { count++; p = hit + nl; }
    }
    LeaveCriticalSection(&b->lock);
    return count;
}

// Reads from the child's stdout pipe forever, both buffering (for prompt/
// sentinel search) and mirroring live to the real console, so the user watches
// the automated boot+provisioning happen in real time.
typedef struct { HANDLE hReadFromChild, hRealStdout; outbuf_t *buf; } out_relay_ctx_t;
static DWORD WINAPI out_relay_thread(LPVOID param) {
    out_relay_ctx_t *ctx = (out_relay_ctx_t *)param;
    char tmp[4096];
    DWORD n;
    while (ReadFile(ctx->hReadFromChild, tmp, sizeof(tmp), &n, NULL) && n > 0) {
        outbuf_append(ctx->buf, tmp, n);
        DWORD written;
        WriteFile(ctx->hRealStdout, tmp, n, &written, NULL);
    }
    return 0;
}

// Once provisioning is done, relays real keystrokes straight into the VM.
typedef struct { HANDLE hRealStdin, hWriteToChild; } in_relay_ctx_t;
static DWORD WINAPI in_relay_thread(LPVOID param) {
    in_relay_ctx_t *ctx = (in_relay_ctx_t *)param;
    char tmp[256];
    DWORD n;
    while (ReadFile(ctx->hRealStdin, tmp, sizeof(tmp), &n, NULL) && n > 0) {
        DWORD written;
        if (!WriteFile(ctx->hWriteToChild, tmp, n, &written, NULL)) break;
    }
    return 0;
}

// Sends 'chunk' bytes at a time with a short pause in between (and loops each
// chunk until it's fully written -- WriteFile on a pipe can write fewer bytes
// than requested). A long line sent as one fast burst can overrun the guest's
// emulated UART FIFO under TCG (the guest's interrupt handler can't drain it
// fast enough), silently dropping characters mid-command -- we saw exactly
// that corruption. Pacing the writes gives the guest time to keep up.
static void pipe_send(HANDLE hWrite, const char *s) {
    size_t total = strlen(s);
    size_t off = 0;
    const size_t chunk = 16;
    while (off < total) {
        size_t n = (total - off) < chunk ? (total - off) : chunk;
        size_t sent = 0;
        while (sent < n) {
            DWORD written = 0;
            if (!WriteFile(hWrite, s + off + sent, (DWORD)(n - sent), &written, NULL) || written == 0) return;
            sent += written;
        }
        off += n;
        Sleep(15);
    }
}

// Waits until 'needle' has appeared at least 'n' times, or until timeout/the
// qemu process exits. TCG boot timing on a loaded host varies a lot (observed
// 90s-150s+ just to reach a login prompt), so callers use generous budgets.
static int wait_for_nth(outbuf_t *buf, const char *needle, int n, DWORD timeout_ms, HANDLE hProcess) {
    DWORD start = GetTickCount();
    while (GetTickCount() - start < timeout_ms) {
        if (outbuf_count(buf, needle) >= n) return 1;
        if (WaitForSingleObject(hProcess, 0) == WAIT_OBJECT_0) return 0; // process died
        Sleep(300);
    }
    return 0;
}

static size_t outbuf_length(outbuf_t *b) {
    EnterCriticalSection(&b->lock);
    size_t l = b->len;
    LeaveCriticalSection(&b->lock);
    return l;
}

// Finds the LAST occurrence of "needle:" in the buffer and parses the integer
// right after the colon. Used to read back a probe's "TOKEN:<value>" reply
// (the last occurrence is always the real one -- any earlier one is just the
// probe's own echoed input, which has no colon/value after it yet).
static int outbuf_last_int_after(outbuf_t *buf, const char *needle) {
    int rc = -1;
    EnterCriticalSection(&buf->lock);
    const char *hit = NULL, *p = buf->data, *next;
    while ((next = strstr(p, needle)) != NULL) { hit = next; p = next + 1; }
    if (hit) {
        const char *after = hit + strlen(needle);
        if (*after == ':') rc = atoi(after + 1);
    }
    LeaveCriticalSection(&buf->lock);
    return rc;
}

// ---------------------------------------------------------------------------

int qemu_run_linux(char L, const char *tool_name, int tools_net, int want_share,
                    const char *image_path) {
    L = (char)toupper((unsigned char)L);
    if (L < 'A' || L > 'Z') {
        printf("[vdisk] Specify a drive: 'vdisk linux -s <DRIVE>' (mount it first with 'vdisk create ram 2G <DRIVE>').\n");
        return 0;
    }
    if (!(GetLogicalDrives() & (1u << (L - 'A')))) {
        printf("[vdisk] No vdisk mounted at %c:. Mount one first: 'vdisk create ram 2G %c:'.\n", L, L);
        return 0;
    }
    // A custom image is an unknown OS/boot flow -- we can't assume a login
    // prompt, shell, or apk exist to automate against.
    if (image_path && (tool_name || want_share)) {
        printf("[vdisk] -image can't be combined with --tools/--tools-net/--share "
               "(those assume the built-in Alpine image).\n");
        return 0;
    }
    if (image_path && !file_exists(image_path)) {
        printf("[vdisk] Image not found: %s\n", image_path);
        return 0;
    }

    // A curated recipe (extra setup beyond `apk add`) if we have one for this
    // name; otherwise 'tool_name' is used as-is as a raw apk package name --
    // --tools works for any package, not just the ones with a recipe.
    const tool_recipe_t *recipe = tool_name ? find_tool_recipe(tool_name) : NULL;

    // Bridge setup happens up front, before spending time booting the VM:
    // create/confirm the SMB share (caller must already be elevated for
    // this) and get the password we'll hand the guest for its one mount.
    char share_name[32] = {0}, share_user[256] = {0}, share_pass[256] = {0};
    if (want_share) {
        if (!bridge_ensure_share(share_name, sizeof(share_name))) return 0;
        DWORD ulen = sizeof(share_user);
        GetUserNameA(share_user, &ulen);
        if (!bridge_prompt_password(share_pass, sizeof(share_pass))) {
            printf("[vdisk] No password entered; aborting.\n");
            return 0;
        }
    }

    const char *q = qemu_path();
    if (!q[0]) {
        printf("[vdisk] QEMU not found. Install it: 'winget install SoftwareFreedomConservancy.QEMU'.\n");
        return 0;
    }

    char dir[MAX_PATH];
    if (!get_data_dir(dir, sizeof(dir))) { printf("[vdisk] Data dir error.\n"); return 0; }

    int custom_image = (image_path != NULL);
    char iso[MAX_PATH], bootdir[MAX_PATH], kernel[MAX_PATH], initrd[MAX_PATH];

    if (custom_image) {
        // An arbitrary ISO boots via its own bootloader (-cdrom), not our
        // direct-kernel-boot trick, which only knows how to extract and drive
        // Alpine's specific kernel/initramfs.
        strcpy_s(iso, sizeof(iso), image_path);
    } else {
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
    }

    // A RAM/VRAM-backed data disk (on the vdisk) exposed to the VM as a second
    // drive. Lives in VM\ (created by disk_mgr_create; CreateDirectory here
    // too in case this disk predates that, or was made by something else).
    // IDE for a custom image (unknown OS, may have no virtio drivers);
    // virtio for our own Alpine, which we know supports it.
    const char *disk_if = custom_image ? "ide" : "virtio";
    char vmdir[MAX_PATH], img[MAX_PATH];
    snprintf(vmdir, sizeof(vmdir), "%c:\\VM", L);
    CreateDirectoryA(vmdir, NULL);
    snprintf(img, sizeof(img), "%c:\\VM\\linux.img", L);
    int have_img = file_exists(img);
    if (!have_img) {
        printf("[vdisk] Creating a %d MB data disk %c:\\VM\\linux.img ...\n", DATA_IMG_MB, L);
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

    // Give a tool install (or the bridge's cifs-utils) some headroom over the
    // bare-shell default even without a recipe (we don't know an arbitrary
    // package's footprint); a custom image's own requirements are unknown too.
    unsigned long long vm_ram_mb = recipe ? recipe->vm_ram_mb
                                  : (tool_name || want_share || custom_image) ? 1280 : DEFAULT_VM_RAM_MB;

    // Repeated -accel flags are QEMU's documented fallback chain: it tries
    // whpx first and silently falls back to tcg if Windows Hypervisor
    // Platform isn't enabled (confirmed: prints a couple of warning lines and
    // proceeds normally either way) -- so this is always safe to pass, no
    // pre-check needed. 'vdisk accel enable' is what actually turns on whpx.
    char cmd[2048];
    if (custom_image) {
        // No -kernel/-initrd/-append here: an arbitrary ISO brings its own
        // bootloader (SeaBIOS -> ISOLINUX/GRUB/whatever), which -nographic
        // relays to this console just like it does Alpine's.
        if (have_img) {
            snprintf(cmd, sizeof(cmd),
                     "\"%s\" -accel whpx -accel tcg -M pc -m %llu -smp 2 -nographic "
                     "-cdrom \"%s\" -drive file=\"%s\",format=raw,if=%s",
                     q, vm_ram_mb, iso, img, disk_if);
        } else {
            snprintf(cmd, sizeof(cmd),
                     "\"%s\" -accel whpx -accel tcg -M pc -m %llu -smp 2 -nographic -cdrom \"%s\"",
                     q, vm_ram_mb, iso);
        }
    } else if (have_img) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -accel whpx -accel tcg -M pc -m %llu -smp 2 -nographic "
                 "-kernel \"%s\" -initrd \"%s\" "
                 "-append \"console=ttyS0 modloop=/boot/modloop-virt quiet\" "
                 "-cdrom \"%s\" -drive file=\"%s\",format=raw,if=%s",
                 q, vm_ram_mb, kernel, initrd, iso, img, disk_if);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -accel whpx -accel tcg -M pc -m %llu -smp 2 -nographic "
                 "-kernel \"%s\" -initrd \"%s\" "
                 "-append \"console=ttyS0 modloop=/boot/modloop-virt quiet\" "
                 "-cdrom \"%s\"",
                 q, vm_ram_mb, kernel, initrd, iso);
    }

    printf("\n");
    printf("=====================================================================\n");
    if (custom_image) {
        printf(" vdisk linux -s %c -image  --  %s (headless console)\n", L, iso);
    } else {
        printf(" vdisk linux -s %c  --  REAL Alpine Linux in QEMU (headless console)\n", L);
    }
    if (tool_name) {
        printf(" Auto-installing: %s%s (this can take a few minutes under software emulation)\n",
               tool_name, tools_net ? " (from the network)" : " (from the local boot image only)");
    }
    if (want_share) {
        char bridgepath[MAX_PATH];
        bridge_folder_path(bridgepath, sizeof(bridgepath));
        printf(" Bridging %s into the VM at /mnt/win (share '%s').\n", bridgepath, share_name);
    }
    if (custom_image) {
        printf(" Unknown OS: follow its own boot menu/login on screen.%s\n",
               have_img ? "  Extra data disk attached." : "");
    } else if (!tool_name && !want_share) {
        printf(" Login: root  (no password).%s\n", have_img ? "  Disk is /dev/vda inside Linux." : "");
    }
    printf(" To leave: type 'poweroff'  (or press Ctrl-A then X to kill QEMU).\n");
    printf(" NOTE: software emulation (TCG) -- boot takes ~1-2 min and runs slowly.\n");
    printf("=====================================================================\n\n");
    fflush(stdout);

    if (!tool_name && !want_share) {
        // Plain shell: inherit the console directly for a fully native terminal.
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

    // --tools-net: we drive the serial console ourselves (via pipes) to log in
    // and provision automatically, then hand real keystrokes to the VM.
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hChildStdinR, hChildStdinW, hChildStdoutR, hChildStdoutW;
    if (!CreatePipe(&hChildStdinR, &hChildStdinW, &sa, 0) ||
        !CreatePipe(&hChildStdoutR, &hChildStdoutW, &sa, 0)) {
        printf("[vdisk] Failed to create pipes (error %lu).\n", GetLastError());
        return 0;
    }
    SetHandleInformation(hChildStdinW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hChildStdoutR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hChildStdinR;
    si.hStdOutput = hChildStdoutW;
    si.hStdError = hChildStdoutW;
    PROCESS_INFORMATION pi = {0};
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("[vdisk] Failed to launch QEMU (error %lu).\n", GetLastError());
        CloseHandle(hChildStdinR); CloseHandle(hChildStdinW);
        CloseHandle(hChildStdoutR); CloseHandle(hChildStdoutW);
        return 0;
    }
    CloseHandle(hChildStdinR);
    CloseHandle(hChildStdoutW);

    outbuf_t buf;
    outbuf_init(&buf);
    out_relay_ctx_t octx = { hChildStdoutR, GetStdHandle(STD_OUTPUT_HANDLE), &buf };
    HANDLE hOutThread = CreateThread(NULL, 0, out_relay_thread, &octx, 0, NULL);

    int ok = 0;
    // TCG boot time to a login prompt varies a lot under load (90s-240s+
    // observed); budget generously rather than aborting a boot that's still
    // progressing.
    if (!wait_for_nth(&buf, "login:", 1, 360000, pi.hProcess)) {
        printf("\n[vdisk] Did not reach the login prompt in time; aborting.\n");
        TerminateProcess(pi.hProcess, 1);
        goto cleanup;
    }
    pipe_send(hChildStdinW, "root\n");
    // Wait for the post-login MOTD to finish printing before sending more
    // input: characters written while the login/MOTD sequence is still in
    // flight can be swallowed or interleaved, garbling the command.
    wait_for_nth(&buf, "You may change this message by editing /etc/motd.", 1, 20000, pi.hProcess);
    Sleep(500); // let the fresh shell prompt actually render

    {
        // The whole provisioning chain runs in a backgrounded subshell
        // ("( ... ) &"): the shell prompt returns to us IMMEDIATELY (nothing
        // stays running in the foreground), so once the marker file is
        // written we can safely send short, isolated probe commands one at a
        // time -- each one's own reply is unambiguous, waited for before the
        // next is sent. (Two earlier approaches broke: probes sent while a
        // foreground job was still running just queued up and their echoed
        // input got misread as real output; and waiting for the shell prompt
        // to "reappear" after the long command was unreliable -- the guest's
        // line editor seems to redraw the prompt on its own, likely because
        // we never answer its cursor-position query, well before the chain
        // actually finishes.)
        const char *marker = "/tmp/vdisk_provision_ok";
        const char *rcfile = "/tmp/vdisk-apk-install.rc";
        const char *mountrc = "/tmp/vdisk-mount.rc";
        // Networking is needed for a full apk-repo install OR the SMB bridge
        // (both go out through the same SLIRP gateway); a local-only tool
        // install needs neither.
        int need_network = tools_net || want_share;

        // A curated recipe supplies its own packages + extra setup; otherwise
        // 'tool_name' IS the apk package, and we make a best-effort attempt to
        // start it as an OpenRC service afterwards ("|| true": harmless no-op
        // for plain CLI tools that have no service to start).
        char generic_script[192] = "";
        const char *apk_packages = recipe ? recipe->apk_packages : tool_name;
        const char *provision_script = "";
        if (tool_name) {
            if (recipe) {
                provision_script = recipe->provision_script;
            } else {
                snprintf(generic_script, sizeof(generic_script),
                         "rc-service %s start >/tmp/vdisk-service-start.log 2>&1 || true; ", tool_name);
                provision_script = generic_script;
            }
        }

        // Built up piece by piece so each of --tools / --tools-net / --share
        // can be present independently of the others.
        char provcmd[4096];
        strcpy_s(provcmd, sizeof(provcmd), "( ");
        if (need_network) {
            strcat_s(provcmd, sizeof(provcmd), "ip link set eth0 up; udhcpc -i eth0 -q -n; ");
        }
        if (tools_net) {
            char repopart[400];
            snprintf(repopart, sizeof(repopart),
                     "echo %s >> /etc/apk/repositories; echo %s >> /etc/apk/repositories; "
                     "apk update >/tmp/vdisk-apk-update.log 2>&1; ",
                     ALPINE_REPO_MAIN, ALPINE_REPO_COMMUNITY);
            strcat_s(provcmd, sizeof(provcmd), repopart);
        }
        if (tool_name) {
            char installpart[700];
            snprintf(installpart, sizeof(installpart),
                     "apk add --no-cache %s >/tmp/vdisk-apk-install.log 2>&1; echo $? >%s; %s",
                     apk_packages, rcfile, provision_script);
            strcat_s(provcmd, sizeof(provcmd), installpart);
        }
        if (want_share) {
            bridge_append_mount_cmd(provcmd, sizeof(provcmd), share_name, share_user, share_pass);
        }
        {
            char touchpart[64];
            snprintf(touchpart, sizeof(touchpart), "touch %s ) >/tmp/vdisk-provision.log 2>&1 &\n", marker);
            strcat_s(provcmd, sizeof(provcmd), touchpart);
        }
        pipe_send(hChildStdinW, provcmd);
        Sleep(1000); // let the backgrounding itself settle before we probe

        // Network work over TCG can be slow (observed variance on a loaded
        // host); a purely local-only tool install needs no such allowance.
        int prov_ok = 0;
        DWORD budget_ms = need_network ? 600000 : 60000;
        DWORD deadline = GetTickCount() + budget_ms;
        while (GetTickCount() < deadline) {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
            char probe[128], hit[48];
            snprintf(hit, sizeof(hit), "VDISK_READY_%lu", GetTickCount());
            snprintf(probe, sizeof(probe), "test -f %s && echo %s\n", marker, hit);
            // Each probe's target text is unique, so counting within THIS
            // probe's attempt is safe: 1st occurrence is always our own
            // echoed input (short, single line, no wrap-echo risk); a 2nd
            // occurrence can only be the real "echo" firing, i.e. the file
            // existed and && actually ran it.
            pipe_send(hChildStdinW, probe);
            if (wait_for_nth(&buf, hit, 2, 8000, pi.hProcess)) { prov_ok = 1; break; }
            Sleep(need_network ? 7000 : 2000); // don't send the next probe until well after this one settled
        }

        if (!prov_ok) {
            printf("\n[vdisk] Warning: provisioning did not confirm completion in time.\n");
            printf("[vdisk] Dropping into the shell anyway -- check /tmp/vdisk-*.log inside the VM.\n\n");
        } else {
            if (tool_name) {
                // Short probe: did `apk add` actually succeed?
                char rcprobe[96], rctag[48];
                snprintf(rctag, sizeof(rctag), "VDISK_RC_%lu", GetTickCount());
                snprintf(rcprobe, sizeof(rcprobe), "echo %s:$(cat %s)\n", rctag, rcfile);
                pipe_send(hChildStdinW, rcprobe);
                wait_for_nth(&buf, rctag, 2, 6000, pi.hProcess);
                int rc = outbuf_last_int_after(&buf, rctag);

                if (rc == 0) {
                    printf("\n[vdisk] '%s' installed.\n", tool_name);
                } else {
                    printf("\n[vdisk] Warning: 'apk add %s' failed (exit code %d) -- misspelled, or not in the%s repos?\n",
                           apk_packages, rc, tools_net ? "" : " local boot image (try --tools-net)");
                    printf("[vdisk] Check /tmp/vdisk-apk-install.log inside the VM.\n");
                }
            }
            if (want_share) {
                // Short probe: did the cifs mount actually succeed?
                char rcprobe[96], rctag[48];
                snprintf(rctag, sizeof(rctag), "VDISK_MNT_%lu", GetTickCount());
                snprintf(rcprobe, sizeof(rcprobe), "echo %s:$(cat %s 2>/dev/null)\n", rctag, mountrc);
                pipe_send(hChildStdinW, rcprobe);
                wait_for_nth(&buf, rctag, 2, 6000, pi.hProcess);
                int rc = outbuf_last_int_after(&buf, rctag);

                if (rc == 0) {
                    char bridgepath[MAX_PATH];
                    bridge_folder_path(bridgepath, sizeof(bridgepath));
                    printf("[vdisk] Bridge mounted: /mnt/win inside the VM = %s on Windows.\n", bridgepath);
                } else {
                    printf("[vdisk] Warning: the SMB mount failed (exit code %d) -- wrong password?\n", rc);
                    printf("[vdisk] Check /tmp/vdisk-mount.log inside the VM.\n");
                }
            }
            printf("\n[vdisk] You now have an interactive shell. Type 'poweroff' (or Ctrl-A then X) to exit.\n\n");
        }
        fflush(stdout);
    }

    // Hand real keystrokes to the VM: raw input mode, live relay thread.
    {
        HANDLE hRealStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD origMode = 0;
        GetConsoleMode(hRealStdin, &origMode);
        SetConsoleMode(hRealStdin, origMode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));

        in_relay_ctx_t ictx = { hRealStdin, hChildStdinW };
        HANDLE hInThread = CreateThread(NULL, 0, in_relay_thread, &ictx, 0, NULL);

        WaitForSingleObject(pi.hProcess, INFINITE);

        SetConsoleMode(hRealStdin, origMode);
        // The input thread is blocked in a console ReadFile with no clean way to
        // cancel it; the process is about to exit anyway, so terminate it rather
        // than leak it indefinitely.
        TerminateThread(hInThread, 0);
    }
    ok = 1;

cleanup:
    WaitForSingleObject(hOutThread, 2000);
    CloseHandle(hChildStdoutR);
    CloseHandle(hChildStdinW);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    outbuf_free(&buf);
    printf("\n[vdisk] Linux VM exited.\n");
    return ok;
}
