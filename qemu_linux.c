#define _CRT_SECURE_NO_WARNINGS
#include "qemu_linux.h"
#include "vdisk_util.h"
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

int qemu_run_linux(char L, const char *tool_name, int tools_net) {
    L = (char)toupper((unsigned char)L);
    if (L < 'A' || L > 'Z') {
        printf("[vdisk] Specify a drive: 'vdisk linux -s <DRIVE>' (mount it first with 'vdisk create ram 2G <DRIVE>').\n");
        return 0;
    }
    if (!(GetLogicalDrives() & (1u << (L - 'A')))) {
        printf("[vdisk] No vdisk mounted at %c:. Mount one first: 'vdisk create ram 2G %c:'.\n", L, L);
        return 0;
    }

    // A curated recipe (extra setup beyond `apk add`) if we have one for this
    // name; otherwise 'tool_name' is used as-is as a raw apk package name --
    // --tools works for any package, not just the ones with a recipe.
    const tool_recipe_t *recipe = tool_name ? find_tool_recipe(tool_name) : NULL;

    const char *q = qemu_path();
    if (!q[0]) {
        printf("[vdisk] QEMU not found. Install it: 'winget install SoftwareFreedomConservancy.QEMU'.\n");
        return 0;
    }

    char dir[MAX_PATH];
    if (!get_data_dir(dir, sizeof(dir))) { printf("[vdisk] Data dir error.\n"); return 0; }

    char iso[MAX_PATH], bootdir[MAX_PATH], kernel[MAX_PATH], initrd[MAX_PATH];
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

    // A RAM/VRAM-backed data disk (on the vdisk) exposed to Linux as /dev/vda.
    char img[MAX_PATH];
    snprintf(img, sizeof(img), "%c:\\linux.img", L);
    int have_img = file_exists(img);
    if (!have_img) {
        printf("[vdisk] Creating a %d MB data disk %c:\\linux.img ...\n", DATA_IMG_MB, L);
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

    // Give a tool install some headroom over the bare-shell default even
    // without a recipe (we don't know an arbitrary package's footprint).
    unsigned long long vm_ram_mb = recipe ? recipe->vm_ram_mb
                                  : tool_name ? 1280 : DEFAULT_VM_RAM_MB;

    char cmd[2048];
    if (have_img) {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -accel tcg -M pc -m %llu -smp 2 -nographic "
                 "-kernel \"%s\" -initrd \"%s\" "
                 "-append \"console=ttyS0 modloop=/boot/modloop-virt quiet\" "
                 "-cdrom \"%s\" -drive file=\"%s\",format=raw,if=virtio",
                 q, vm_ram_mb, kernel, initrd, iso, img);
    } else {
        snprintf(cmd, sizeof(cmd),
                 "\"%s\" -accel tcg -M pc -m %llu -smp 2 -nographic "
                 "-kernel \"%s\" -initrd \"%s\" "
                 "-append \"console=ttyS0 modloop=/boot/modloop-virt quiet\" "
                 "-cdrom \"%s\"",
                 q, vm_ram_mb, kernel, initrd, iso);
    }

    printf("\n");
    printf("=====================================================================\n");
    printf(" vdisk linux -s %c  --  REAL Alpine Linux in QEMU (headless console)\n", L);
    if (tool_name) {
        printf(" Auto-installing: %s%s (this can take a few minutes under software emulation)\n",
               tool_name, tools_net ? " (from the network)" : " (from the local boot image only)");
    } else {
        printf(" Login: root  (no password).%s\n", have_img ? "  Disk is /dev/vda inside Linux." : "");
    }
    printf(" To leave: type 'poweroff'  (or press Ctrl-A then X to kill QEMU).\n");
    printf(" NOTE: software emulation (TCG) -- boot takes ~1-2 min and runs slowly.\n");
    printf("=====================================================================\n\n");
    fflush(stdout);

    if (!tool_name) {
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

        // A curated recipe supplies its own packages + extra setup; otherwise
        // 'tool_name' IS the apk package, and we make a best-effort attempt to
        // start it as an OpenRC service afterwards ("|| true": harmless no-op
        // for plain CLI tools that have no service to start).
        char generic_script[192];
        const char *apk_packages = recipe ? recipe->apk_packages : tool_name;
        const char *provision_script;
        if (recipe) {
            provision_script = recipe->provision_script;
        } else {
            snprintf(generic_script, sizeof(generic_script),
                     "rc-service %s start >/tmp/vdisk-service-start.log 2>&1 || true; ", tool_name);
            provision_script = generic_script;
        }

        char provcmd[4096];
        if (tools_net) {
            // Full network install: bring up networking and add the CDN repos
            // so the whole apk index (tens of thousands of packages) is reachable.
            snprintf(provcmd, sizeof(provcmd),
                     "( ip link set eth0 up; udhcpc -i eth0 -q -n; "
                     "echo %s >> /etc/apk/repositories; "
                     "echo %s >> /etc/apk/repositories; "
                     "apk update >/tmp/vdisk-apk-update.log 2>&1; "
                     "apk add --no-cache %s >/tmp/vdisk-apk-install.log 2>&1; echo $? >%s; "
                     "%s"
                     "touch %s ) >/tmp/vdisk-provision.log 2>&1 &\n",
                     ALPINE_REPO_MAIN, ALPINE_REPO_COMMUNITY,
                     apk_packages, rcfile, provision_script, marker);
        } else {
            // Local only: whatever's already on the boot ISO (~100 packages),
            // no network and no --tools-net needed -- and near-instant.
            snprintf(provcmd, sizeof(provcmd),
                     "( apk add --no-cache %s >/tmp/vdisk-apk-install.log 2>&1; echo $? >%s; "
                     "%s"
                     "touch %s ) >/tmp/vdisk-provision.log 2>&1 &\n",
                     apk_packages, rcfile, provision_script, marker);
        }
        pipe_send(hChildStdinW, provcmd);
        Sleep(1000); // let the backgrounding itself settle before we probe

        // Network installs over TCG can be slow (observed variance on a loaded
        // host); local-only installs need no such allowance.
        int prov_ok = 0;
        DWORD budget_ms = tools_net ? 600000 : 60000;
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
            Sleep(tools_net ? 7000 : 2000); // don't send the next probe until well after this one settled
        }

        if (!prov_ok) {
            printf("\n[vdisk] Warning: provisioning '%s' did not confirm completion in time.\n", tool_name);
            printf("[vdisk] Dropping into the shell anyway -- check /tmp/vdisk-apk-*.log inside the VM.\n\n");
        } else {
            // One more short probe: did `apk add` actually succeed?
            char rcprobe[96], rctag[48];
            snprintf(rctag, sizeof(rctag), "VDISK_RC_%lu", GetTickCount());
            snprintf(rcprobe, sizeof(rcprobe), "echo %s:$(cat %s)\n", rctag, rcfile);
            pipe_send(hChildStdinW, rcprobe);
            wait_for_nth(&buf, rctag, 2, 6000, pi.hProcess);
            int rc = outbuf_last_int_after(&buf, rctag);

            if (rc == 0) {
                printf("\n[vdisk] '%s' installed. You now have an interactive shell.\n", tool_name);
            } else {
                printf("\n[vdisk] Warning: 'apk add %s' failed (exit code %d) -- misspelled, or not in the%s repos?\n",
                       apk_packages, rc, tools_net ? "" : " local boot image (try --tools-net)");
                printf("[vdisk] Dropping into the shell anyway -- check /tmp/vdisk-apk-install.log inside the VM.\n");
            }
            printf("[vdisk] Type 'poweroff' (or Ctrl-A then X) to exit.\n\n");
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
