#define _CRT_SECURE_NO_WARNINGS
#include "whpx_vmm.h"
#include <WinHvPlatform.h>
#include <WinHvEmulation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "WinHvPlatform.lib")
#pragma comment(lib, "WinHvEmulation.lib")

// ============================================================================
// Guest physical memory map (all addresses fixed/low, well under any real
// guest RAM size we'd configure). See the big comment block above
// vmm_boot_guest() for why each region lives where it does.
// ============================================================================
#define GPA_PML4        0x00001000ull
#define GPA_PDPT        0x00002000ull
#define GPA_PD_BASE     0x00003000ull   // 4 tables x 4KB = identity-maps 0..4GB
#define GPA_GDT         0x00007000ull
#define GPA_STACK_TOP   0x00009000ull   // RSP; stack page is 0x8000-0x8FFF
#define GPA_BOOT_PARAMS 0x00009000ull   // "zero page", exactly one 4K page
#define GPA_CMDLINE     0x0000A000ull
#define GPA_KERNEL_LOAD 0x00100000ull   // 1MB -- standard bzImage load addr

#define VIRTIO_MMIO_BASE 0xD0000000ull
#define VIRTIO_MMIO_SIZE 0x200ull
#define VIRTIO_MMIO_IRQ  5              // legacy-PIC-mode vector 0x20+5

#define COM1_IOPORT_BASE 0x3F8
#define COM1_IRQ         4              // legacy-PIC-mode vector 0x20+4

// ============================================================================
// setup_header field offsets within a bzImage file (Documentation/x86/boot.rst).
// We read what we need, patch a handful of fields, and copy the whole thing
// into boot_params.hdr for the kernel to consume.
// ============================================================================
#define HDR_FILE_OFFSET     0x1F1
#define HDR_SIZE_MAX         0x290 - 0x1F1   // generous upper bound; real header is shorter on old kernels

#pragma pack(push, 1)
typedef struct {
    UINT8  setup_sects;
    UINT16 root_flags;
    UINT32 syssize;
    UINT16 ram_size;
    UINT16 vid_mode;
    UINT16 root_dev;
    UINT16 boot_flag;
    UINT16 jump;
    UINT32 header;              // "HdrS"
    UINT16 version;
    UINT32 realmode_swtch;
    UINT16 start_sys_seg;
    UINT16 kernel_version;
    UINT8  type_of_loader;
    UINT8  loadflags;
    UINT16 setup_move_size;
    UINT32 code32_start;
    UINT32 ramdisk_image;
    UINT32 ramdisk_size;
    UINT32 bootsect_kludge;
    UINT16 heap_end_ptr;
    UINT8  ext_loader_ver;
    UINT8  ext_loader_type;
    UINT32 cmd_line_ptr;
    UINT32 initrd_addr_max;
    UINT32 kernel_alignment;
    UINT8  relocatable_kernel;
    UINT8  min_alignment;
    UINT16 xloadflags;
    UINT32 cmdline_size;
    UINT32 hardware_subarch;
    UINT64 hardware_subarch_data;
    UINT32 payload_offset;
    UINT32 payload_length;
    UINT64 setup_data;
    UINT64 pref_address;
    UINT32 init_size;
    UINT32 handover_offset;
} setup_header_t;
#pragma pack(pop)

#define LOADFLAGS_LOADED_HIGH  0x01
#define LOADFLAGS_CAN_USE_HEAP 0x80
#define XLF_KERNEL_64          0x0001

#pragma pack(push, 1)
typedef struct { UINT64 addr; UINT64 size; UINT32 type; } e820_entry_t;
#pragma pack(pop)
#define E820_TYPE_RAM 1

// ============================================================================
// 16550 UART (COM1) -- minimal state, wired to the automation pipes instead
// of a real port.
// ============================================================================
typedef struct {
    HANDLE hIn;   // read host->guest bytes from here (RX)
    HANDLE hOut;  // write guest->host bytes here (TX)
    UINT8  ier;
    UINT8  lcr;
    UINT8  scr;
    UINT8  rx_byte;
    int    rx_has_byte;
} uart_t;

static void uart_poll_rx(uart_t *u) {
    if (u->rx_has_byte || !u->hIn) return;
    DWORD avail = 0;
    if (PeekNamedPipe(u->hIn, NULL, 0, NULL, &avail, NULL) && avail > 0) {
        DWORD got = 0;
        if (ReadFile(u->hIn, &u->rx_byte, 1, &got, NULL) && got == 1) {
            u->rx_has_byte = 1;
        }
    }
}

// Returns 1 if handled.
static int uart_io(uart_t *u, UINT16 port, int is_write, UINT8 *val) {
    int reg = port - COM1_IOPORT_BASE;
    if (reg < 0 || reg > 7) return 0;

    int dlab = (u->lcr & 0x80) != 0;

    if (is_write) {
        switch (reg) {
        case 0: // THR (or DLL if DLAB) -- transmit
            if (!dlab) {
                DWORD written = 0;
                if (u->hOut) WriteFile(u->hOut, val, 1, &written, NULL);
            }
            break;
        case 1: if (!dlab) u->ier = *val; break; // IER (ignore DLM)
        case 2: break;                            // FCR, write-only, ignore
        case 3: u->lcr = *val; break;              // LCR
        case 4: break;                             // MCR, ignore
        case 7: u->scr = *val; break;               // scratch
        default: break;
        }
        return 1;
    }

    switch (reg) {
    case 0: // RBR (or DLL)
        if (!dlab) {
            uart_poll_rx(u);
            *val = u->rx_has_byte ? u->rx_byte : 0;
            u->rx_has_byte = 0;
        } else {
            *val = 0;
        }
        break;
    case 1: *val = dlab ? 0 : u->ier; break;
    case 2: *val = 0xC1; break; // IIR: FIFO enabled, no pending interrupt (poll mode, see note below)
    case 3: *val = u->lcr; break;
    case 4: *val = 0; break;
    case 5: // LSR
        uart_poll_rx(u);
        *val = 0x60 /* THRE | TEMT: we can always accept a byte to transmit */
             | (u->rx_has_byte ? 0x01 : 0x00);
        break;
    case 6: *val = 0; break; // MSR
    case 7: *val = u->scr; break;
    default: *val = 0; break;
    }
    return 1;
}

// ============================================================================
// virtio-mmio (legacy, version 1) + virtio-blk, backed by a plain file.
// ============================================================================
#define VIRTQ_DESC_F_NEXT  1
#define VIRTQ_DESC_F_WRITE 2

#pragma pack(push, 1)
typedef struct { UINT64 addr; UINT32 len; UINT16 flags; UINT16 next; } vring_desc_t;
#pragma pack(pop)

typedef struct {
    HANDLE   hImg;
    UINT64   capacity_sectors;

    UINT32   host_features_sel;
    UINT32   guest_features;
    UINT32   guest_page_size;
    UINT32   queue_sel;      // always 0, we have one queue
    UINT32   queue_num;
    UINT32   queue_align;
    UINT32   queue_pfn;
    UINT32   status;
    UINT32   isr;

    // resolved queue memory (guest physical addresses), recomputed whenever
    // queue_pfn/queue_num/queue_align/guest_page_size change.
    UINT64   desc_gpa, avail_gpa, used_gpa;
    UINT16   used_idx_seen; // last used.idx we wrote, to detect if a kick actually produced work
} virtio_blk_t;

// Forward-declared; defined once guest RAM + partition handle are known
// (device callbacks need to translate GPA -> host pointer).
static UINT8 *g_ram = NULL;
static UINT64 g_ram_size = 0;

// Validates that [gpa, gpa+len) lies entirely within guest RAM before
// handing back a host pointer -- callers previously only checked the start
// address, letting a guest-controlled len (virtio descriptor length, ring
// size, ...) read/write past the end of the host allocation.
static UINT8 *gpa_ptr(UINT64 gpa, UINT64 len) {
    if (len > g_ram_size) return NULL;
    if (gpa > g_ram_size - len) return NULL; // overflow-safe gpa+len<=g_ram_size
    return g_ram + gpa;
}

static void virtio_recalc_queue(virtio_blk_t *v) {
    if (!v->queue_num || !v->guest_page_size) return;
    UINT64 base = (UINT64)v->queue_pfn * v->guest_page_size;
    UINT64 desc_len = 16ull * v->queue_num;
    UINT64 avail_len = 4 + 2ull * v->queue_num + 2;
    UINT64 used_off = (desc_len + avail_len + v->queue_align - 1) & ~((UINT64)v->queue_align - 1);
    v->desc_gpa = base;
    v->avail_gpa = base + desc_len;
    v->used_gpa = base + used_off;
}

// Processes every available descriptor chain currently posted by the driver.
static void virtio_blk_process(virtio_blk_t *v) {
    if (!v->queue_num) return;
    UINT8 *avail = gpa_ptr(v->avail_gpa, 4 + 2ull * v->queue_num + 2);
    UINT8 *used = gpa_ptr(v->used_gpa, 4 + 8ull * v->queue_num);
    UINT8 *desc_base = gpa_ptr(v->desc_gpa, 16ull * v->queue_num);
    if (!avail || !used || !desc_base) return;

    UINT16 avail_flags = *(UINT16 *)(avail + 0);
    (void)avail_flags;
    UINT16 avail_idx = *(UINT16 *)(avail + 2);
    UINT16 *avail_ring = (UINT16 *)(avail + 4);

    UINT16 used_idx = *(UINT16 *)(used + 2);

    while (used_idx != avail_idx) {
        UINT16 head = avail_ring[used_idx % v->queue_num];
        UINT16 idx = head;

        UINT8 *hdr_ptr = NULL;
        UINT64 sector = 0;
        UINT32 op_type = 0;
        UINT8 *status_ptr = NULL;
        UINT32 total_written = 0;
        int first = 1;
        int chain_ok = 1;
        UINT32 chain_len = 0;

        for (;;) {
            // idx comes from the guest (avail ring head, then each
            // descriptor's own `next`); bound it against the actual
            // descriptor table size and cap total hops at queue_num so a
            // corrupted/malicious ring can't walk desc_base out of bounds
            // or spin this loop forever on a cycle.
            if (idx >= v->queue_num || chain_len++ >= v->queue_num) {
                chain_ok = 0;
                break;
            }
            vring_desc_t d;
            memcpy(&d, desc_base + (UINT64)idx * 16, sizeof(d));
            UINT8 *buf = gpa_ptr(d.addr, d.len);

            if (first) {
                // Descriptor 0: struct virtio_blk_outhdr { u32 type; u32 ioprio; u64 sector; }
                if (buf && d.len >= 16) {
                    op_type = *(UINT32 *)(buf + 0);
                    sector  = *(UINT64 *)(buf + 8);
                } else {
                    chain_ok = 0;
                }
                first = 0;
            } else if ((d.flags & VIRTQ_DESC_F_WRITE) && d.len == 1 && !(d.flags & VIRTQ_DESC_F_NEXT)) {
                // Final status byte (device-writable, no NEXT flag).
                status_ptr = buf;
            } else if (buf) {
                // Data buffer.
                if (op_type == 1 /* VIRTIO_BLK_T_OUT */ && v->hImg) {
                    LARGE_INTEGER off; off.QuadPart = (LONGLONG)(sector * 512);
                    SetFilePointerEx(v->hImg, off, NULL, FILE_BEGIN);
                    DWORD wrote = 0;
                    WriteFile(v->hImg, buf, d.len, &wrote, NULL);
                    sector += wrote / 512;
                } else if (op_type == 0 /* VIRTIO_BLK_T_IN */ && v->hImg) {
                    LARGE_INTEGER off; off.QuadPart = (LONGLONG)(sector * 512);
                    SetFilePointerEx(v->hImg, off, NULL, FILE_BEGIN);
                    DWORD read = 0;
                    ReadFile(v->hImg, buf, d.len, &read, NULL);
                    sector += read / 512;
                }
                total_written += d.len;
            }

            if (!(d.flags & VIRTQ_DESC_F_NEXT)) break;
            idx = d.next;
        }
        (void)hdr_ptr;

        if (status_ptr) *status_ptr = chain_ok ? 0 /*VIRTIO_BLK_S_OK*/ : 1 /*IOERR*/;

        // used ring entry
        UINT8 *used_ring = used + 4;
        UINT32 *elem = (UINT32 *)(used_ring + (UINT64)(used_idx % v->queue_num) * 8);
        elem[0] = head;
        elem[1] = total_written + 1;
        used_idx++;
        *(UINT16 *)(used + 2) = used_idx;
    }

    v->isr |= 1;
}

static int virtio_mmio_io(virtio_blk_t *v, UINT64 gpa, int is_write, UINT32 *val, UINT8 size, int *kick) {
    UINT64 off = gpa - VIRTIO_MMIO_BASE;
    *kick = 0;

    if (is_write) {
        switch (off) {
        case 0x014: v->host_features_sel = *val; break;
        case 0x020: v->guest_features = *val; break;
        case 0x024: break; // guest features sel, we only report 32 bits
        case 0x028: v->guest_page_size = *val; virtio_recalc_queue(v); break;
        case 0x030: v->queue_sel = *val; break;
        case 0x038: v->queue_num = *val; virtio_recalc_queue(v); break;
        case 0x03C: v->queue_align = *val ? *val : 4096; virtio_recalc_queue(v); break;
        case 0x040: v->queue_pfn = *val; virtio_recalc_queue(v); break;
        case 0x050: // QueueNotify -- a kick
            *kick = 1;
            break;
        case 0x064: v->isr &= ~(*val); break; // InterruptACK
        case 0x070: v->status = *val; break;
        default: break;
        }
        return 1;
    }

    switch (off) {
    case 0x000: *val = 0x74726976; break;         // "virt"
    case 0x004: *val = 1; break;                  // legacy version
    case 0x008: *val = 2; break;                  // DeviceID: block
    case 0x00C: *val = 0x554D4551; break;          // VendorID (arbitrary)
    case 0x010: *val = v->host_features_sel == 0 ? 0 : 0; break; // no extra features offered
    case 0x034: *val = 256; break;                 // QueueNumMax
    case 0x060: *val = v->isr; break;
    case 0x070: *val = v->status; break;
    default:
        if (off >= 0x100 && off + size <= 0x108) {
            // config space: __le64 capacity (in 512-byte sectors)
            UINT8 tmp[8];
            memcpy(tmp, &v->capacity_sectors, 8);
            memcpy(val, tmp + (off - 0x100), size);
        } else {
            *val = 0;
        }
        break;
    }
    return 1;
}

// ============================================================================
// bzImage loading
// ============================================================================

static int load_file(const char *path, UINT8 **out_buf, UINT64 *out_size) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return 0; }
    UINT8 *buf = (UINT8 *)malloc((size_t)sz.QuadPart);
    if (!buf) { CloseHandle(h); return 0; }
    DWORD totalRead = 0;
    while (totalRead < sz.QuadPart) {
        DWORD n = 0;
        if (!ReadFile(h, buf + totalRead, (DWORD)(sz.QuadPart - totalRead), &n, NULL) || n == 0) {
            free(buf); CloseHandle(h); return 0;
        }
        totalRead += n;
    }
    CloseHandle(h);
    *out_buf = buf;
    *out_size = (UINT64)sz.QuadPart;
    return 1;
}

// ============================================================================
// Page tables + GDT
// ============================================================================

static void build_identity_map(UINT8 *ram) {
    UINT64 *pml4 = (UINT64 *)(ram + GPA_PML4);
    UINT64 *pdpt = (UINT64 *)(ram + GPA_PDPT);
    memset(pml4, 0, 4096);
    memset(pdpt, 0, 4096);

    pml4[0] = GPA_PDPT | 0x3; // present, writable

    for (int g = 0; g < 4; g++) {
        UINT64 pd_gpa = GPA_PD_BASE + (UINT64)g * 4096;
        pdpt[g] = pd_gpa | 0x3;
        UINT64 *pd = (UINT64 *)(ram + pd_gpa);
        for (int i = 0; i < 512; i++) {
            UINT64 phys = ((UINT64)g << 30) | ((UINT64)i << 21);
            pd[i] = phys | 0x83; // present, writable, PS (2MB page)
        }
    }
}

static void build_gdt(UINT8 *ram) {
    UINT64 *gdt = (UINT64 *)(ram + GPA_GDT);
    gdt[0] = 0; // null
    gdt[1] = 0x00AF9A000000FFFFull; // 64-bit code: present, DPL0, exec/read, L=1, limit=0xFFFFF
    gdt[2] = 0x00CF92000000FFFFull; // flat data: present, DPL0, read/write
}

// ============================================================================
// vCPU / partition state
// ============================================================================

static int set_reg64(WHV_PARTITION_HANDLE p, WHV_REGISTER_NAME name, UINT64 v) {
    WHV_REGISTER_VALUE val; val.Reg64 = v;
    return SUCCEEDED(WHvSetVirtualProcessorRegisters(p, 0, &name, 1, &val));
}

static int set_seg(WHV_PARTITION_HANDLE p, WHV_REGISTER_NAME name, UINT16 selector, UINT64 base,
                    UINT32 limit, UINT16 type, int isLong, int isDefault) {
    WHV_REGISTER_VALUE val;
    memset(&val, 0, sizeof(val));
    val.Segment.Base = base;
    val.Segment.Limit = limit;
    val.Segment.Selector = selector;
    val.Segment.SegmentType = type & 0xF;
    val.Segment.NonSystemSegment = 1;
    val.Segment.Present = 1;
    val.Segment.Long = isLong ? 1 : 0;
    val.Segment.Default = isDefault ? 1 : 0;
    val.Segment.Granularity = 1;
    return SUCCEEDED(WHvSetVirtualProcessorRegisters(p, 0, &name, 1, &val));
}

static int setup_long_mode_vcpu(WHV_PARTITION_HANDLE p, UINT64 rip, UINT64 rsi) {
    int ok = 1;
    ok &= set_reg64(p, WHvX64RegisterCr3, GPA_PML4);
    ok &= set_reg64(p, WHvX64RegisterCr4, 0x20);        // PAE
    ok &= set_reg64(p, WHvX64RegisterCr0, 0x80000011);  // PE|ET|PG
    ok &= set_reg64(p, WHvX64RegisterEfer, 0x500);      // LME|LMA

    {
        WHV_REGISTER_NAME name = WHvX64RegisterGdtr;
        WHV_REGISTER_VALUE val; memset(&val, 0, sizeof(val));
        val.Table.Base = GPA_GDT;
        val.Table.Limit = 0x17;
        ok &= SUCCEEDED(WHvSetVirtualProcessorRegisters(p, 0, &name, 1, &val));
    }

    ok &= set_seg(p, WHvX64RegisterCs, 0x08, 0, 0xFFFFFFFF, 0xB, 1, 0);
    ok &= set_seg(p, WHvX64RegisterDs, 0x10, 0, 0xFFFFFFFF, 0x3, 0, 1);
    ok &= set_seg(p, WHvX64RegisterEs, 0x10, 0, 0xFFFFFFFF, 0x3, 0, 1);
    ok &= set_seg(p, WHvX64RegisterSs, 0x10, 0, 0xFFFFFFFF, 0x3, 0, 1);
    ok &= set_seg(p, WHvX64RegisterFs, 0x10, 0, 0xFFFFFFFF, 0x3, 0, 1);
    ok &= set_seg(p, WHvX64RegisterGs, 0x10, 0, 0xFFFFFFFF, 0x3, 0, 1);

    ok &= set_reg64(p, WHvX64RegisterRip, rip);
    ok &= set_reg64(p, WHvX64RegisterRsi, rsi);
    ok &= set_reg64(p, WHvX64RegisterRsp, GPA_STACK_TOP);
    ok &= set_reg64(p, WHvX64RegisterRflags, 0x2); // reserved bit 1 only, interrupts off

    return ok;
}

int whpx_vmm_available(void) {
    WHV_CAPABILITY cap;
    UINT32 written = 0;
    if (FAILED(WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &cap, sizeof(cap), &written))) return 0;
    return cap.HypervisorPresent ? 1 : 0;
}

// ============================================================================
// Emulator glue (only used for MMIO; I/O ports are simple enough to decode
// by hand directly from WHV_X64_IO_PORT_ACCESS_CONTEXT).
// ============================================================================

typedef struct {
    WHV_PARTITION_HANDLE partition;
    virtio_blk_t *vblk;
} emu_ctx_t;

static HRESULT CALLBACK emu_get_regs(VOID *ctx, const WHV_REGISTER_NAME *names, UINT32 count, WHV_REGISTER_VALUE *values) {
    emu_ctx_t *e = (emu_ctx_t *)ctx;
    return WHvGetVirtualProcessorRegisters(e->partition, 0, names, count, values);
}
static HRESULT CALLBACK emu_set_regs(VOID *ctx, const WHV_REGISTER_NAME *names, UINT32 count, const WHV_REGISTER_VALUE *values) {
    emu_ctx_t *e = (emu_ctx_t *)ctx;
    return WHvSetVirtualProcessorRegisters(e->partition, 0, names, count, values);
}
static HRESULT CALLBACK emu_translate(VOID *ctx, WHV_GUEST_VIRTUAL_ADDRESS gva, WHV_TRANSLATE_GVA_FLAGS flags,
                                       WHV_TRANSLATE_GVA_RESULT_CODE *result, WHV_GUEST_PHYSICAL_ADDRESS *gpa) {
    emu_ctx_t *e = (emu_ctx_t *)ctx;
    WHV_TRANSLATE_GVA_RESULT full;
    HRESULT hr = WHvTranslateGva(e->partition, 0, gva, flags, &full, gpa);
    if (result) *result = full.ResultCode;
    return hr;
}
static HRESULT CALLBACK emu_io(VOID *ctx, WHV_EMULATOR_IO_ACCESS_INFO *io) {
    // Not used by our MMIO-only emulation path.
    (void)ctx; (void)io;
    return S_OK;
}
static HRESULT CALLBACK emu_mmio(VOID *ctx, WHV_EMULATOR_MEMORY_ACCESS_INFO *mem) {
    emu_ctx_t *e = (emu_ctx_t *)ctx;
    if (mem->GpaAddress >= VIRTIO_MMIO_BASE && mem->GpaAddress < VIRTIO_MMIO_BASE + VIRTIO_MMIO_SIZE) {
        UINT32 val = 0;
        int kick = 0;
        if (mem->Direction) { // write
            memcpy(&val, mem->Data, mem->AccessSize < 4 ? mem->AccessSize : 4);
            virtio_mmio_io(e->vblk, mem->GpaAddress, 1, &val, mem->AccessSize, &kick);
            if (kick) virtio_blk_process(e->vblk);
        } else {
            virtio_mmio_io(e->vblk, mem->GpaAddress, 0, &val, mem->AccessSize, &kick);
            memcpy(mem->Data, &val, mem->AccessSize < 4 ? mem->AccessSize : 4);
        }
    }
    return S_OK;
}

// ============================================================================
// Main entry point
// ============================================================================

int whpx_vmm_run(const char *kernel_path, const char *initrd_path,
                  const char *img_path, const char *cmdline,
                  unsigned long long ram_mb,
                  HANDLE hSerialIn, HANDLE hSerialOut) {
    if (!whpx_vmm_available()) {
        printf("[vdisk] Windows Hypervisor Platform is not enabled. Run 'vdisk accel enable' first.\n");
        return 0;
    }

    UINT8 *kfile = NULL; UINT64 ksize = 0;
    UINT8 *ifile = NULL; UINT64 isize = 0;
    if (!load_file(kernel_path, &kfile, &ksize) || ksize < 0x300) {
        printf("[vdisk] Failed to read kernel image '%s'.\n", kernel_path);
        return 0;
    }
    if (!load_file(initrd_path, &ifile, &isize)) {
        printf("[vdisk] Failed to read initrd '%s'.\n", initrd_path);
        free(kfile);
        return 0;
    }

    setup_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    UINT64 avail_hdr = ksize - HDR_FILE_OFFSET;
    UINT64 copy_hdr = avail_hdr < sizeof(hdr) ? avail_hdr : sizeof(hdr);
    memcpy(&hdr, kfile + HDR_FILE_OFFSET, (size_t)copy_hdr);

    if (hdr.boot_flag != 0xAA55 || hdr.header != 0x53726448 /* "HdrS" */) {
        printf("[vdisk] Kernel doesn't look like a valid bzImage (bad boot signature).\n");
        free(kfile); free(ifile);
        return 0;
    }
    if (hdr.version < 0x020c || !(hdr.xloadflags & XLF_KERNEL_64)) {
        printf("[vdisk] Kernel is too old for direct 64-bit boot (need protocol >= 2.12 with a 64-bit entry point).\n");
        free(kfile); free(ifile);
        return 0;
    }

    UINT32 setup_sects = hdr.setup_sects ? hdr.setup_sects : 4;
    UINT64 kernel_file_off = (UINT64)(setup_sects + 1) * 512;
    if (kernel_file_off >= ksize) {
        printf("[vdisk] Kernel file truncated (bad setup_sects).\n");
        free(kfile); free(ifile);
        return 0;
    }
    UINT64 kernel_img_size = ksize - kernel_file_off;

    UINT64 ram_bytes = ram_mb * 1024ull * 1024ull;
    UINT64 kernel_end = GPA_KERNEL_LOAD + kernel_img_size;
    UINT64 initrd_load = (kernel_end + 0xFFF) & ~0xFFFull;
    UINT32 initrd_addr_max = hdr.initrd_addr_max ? hdr.initrd_addr_max : 0x37FFFFFF;

    if (initrd_load + isize > ram_bytes || initrd_load + isize > initrd_addr_max) {
        printf("[vdisk] Not enough guest RAM for kernel + initrd (have %llu MB).\n", ram_mb);
        free(kfile); free(ifile);
        return 0;
    }
    if (ram_bytes < 64ull * 1024 * 1024 || ram_bytes > 4064ull * 1024 * 1024) {
        printf("[vdisk] whpx_vmm supports 64MB-4064MB of guest RAM (got %llu MB).\n", ram_mb);
        free(kfile); free(ifile);
        return 0;
    }

    // --- Partition setup ---------------------------------------------------
    WHV_PARTITION_HANDLE partition;
    if (FAILED(WHvCreatePartition(&partition))) {
        printf("[vdisk] WHvCreatePartition failed.\n");
        free(kfile); free(ifile);
        return 0;
    }

    {
        WHV_PARTITION_PROPERTY prop;
        memset(&prop, 0, sizeof(prop));
        prop.ProcessorCount = 1;
        WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeProcessorCount, &prop, sizeof(prop.ProcessorCount));

        memset(&prop, 0, sizeof(prop));
        prop.LocalApicEmulationMode = WHvX64LocalApicEmulationModeXApic;
        WHvSetPartitionProperty(partition, WHvPartitionPropertyCodeLocalApicEmulationMode, &prop, sizeof(prop.LocalApicEmulationMode));
    }

    if (FAILED(WHvSetupPartition(partition))) {
        printf("[vdisk] WHvSetupPartition failed (is Windows Hypervisor Platform actually enabled + rebooted?).\n");
        WHvDeletePartition(partition);
        free(kfile); free(ifile);
        return 0;
    }

    UINT8 *ram = (UINT8 *)VirtualAlloc(NULL, (SIZE_T)ram_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!ram) {
        printf("[vdisk] Failed to allocate %llu MB for guest RAM.\n", ram_mb);
        WHvDeletePartition(partition);
        free(kfile); free(ifile);
        return 0;
    }
    memset(ram, 0, (size_t)ram_bytes);

    if (FAILED(WHvMapGpaRange(partition, ram, 0, ram_bytes,
                               WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagWrite | WHvMapGpaRangeFlagExecute))) {
        printf("[vdisk] WHvMapGpaRange failed.\n");
        VirtualFree(ram, 0, MEM_RELEASE);
        WHvDeletePartition(partition);
        free(kfile); free(ifile);
        return 0;
    }

    g_ram = ram;
    g_ram_size = ram_bytes;

    // --- Load kernel + initrd, build tables ---------------------------------
    memcpy(ram + GPA_KERNEL_LOAD, kfile + kernel_file_off, (size_t)kernel_img_size);
    memcpy(ram + initrd_load, ifile, (size_t)isize);
    build_identity_map(ram);
    build_gdt(ram);

    // Patch the header copy that goes into boot_params.
    hdr.type_of_loader = 0xFF;
    hdr.loadflags |= LOADFLAGS_LOADED_HIGH;
    hdr.loadflags &= ~LOADFLAGS_CAN_USE_HEAP; // keep it simple, no heap needed for our fixed cmdline
    hdr.ramdisk_image = (UINT32)initrd_load;
    hdr.ramdisk_size = (UINT32)isize;
    hdr.cmd_line_ptr = (UINT32)GPA_CMDLINE;

    size_t cmdlen = strlen(cmdline);
    if (cmdlen >= 4096) cmdlen = 4095;
    memcpy(ram + GPA_CMDLINE, cmdline, cmdlen);
    ram[GPA_CMDLINE + cmdlen] = 0;

    memset(ram + GPA_BOOT_PARAMS, 0, 4096);
    ram[GPA_BOOT_PARAMS + 0x1E8] = 2; // e820_entries
    memcpy(ram + GPA_BOOT_PARAMS + 0x1F1, &hdr, (size_t)copy_hdr);

    {
        e820_entry_t e[2];
        e[0].addr = 0; e[0].size = 0x9FC00; e[0].type = E820_TYPE_RAM;
        e[1].addr = 0x100000; e[1].size = ram_bytes - 0x100000; e[1].type = E820_TYPE_RAM;
        memcpy(ram + GPA_BOOT_PARAMS + 0x2D0, e, sizeof(e));
    }

    if (FAILED(WHvCreateVirtualProcessor(partition, 0, 0))) {
        printf("[vdisk] WHvCreateVirtualProcessor failed.\n");
        VirtualFree(ram, 0, MEM_RELEASE);
        WHvDeletePartition(partition);
        free(kfile); free(ifile);
        return 0;
    }

    UINT64 entry = GPA_KERNEL_LOAD + 0x200; // 64-bit entry point per XLF_KERNEL_64
    if (!setup_long_mode_vcpu(partition, entry, GPA_BOOT_PARAMS)) {
        printf("[vdisk] Failed to set initial vCPU register state.\n");
        WHvDeleteVirtualProcessor(partition, 0);
        VirtualFree(ram, 0, MEM_RELEASE);
        WHvDeletePartition(partition);
        free(kfile); free(ifile);
        return 0;
    }

    free(kfile);
    free(ifile);

    // --- Devices -------------------------------------------------------------
    uart_t uart; memset(&uart, 0, sizeof(uart));
    uart.hIn = hSerialIn;
    uart.hOut = hSerialOut;

    virtio_blk_t vblk; memset(&vblk, 0, sizeof(vblk));
    vblk.hImg = CreateFileA(img_path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (vblk.hImg != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(vblk.hImg, &sz)) vblk.capacity_sectors = (UINT64)sz.QuadPart / 512;
    } else {
        vblk.hImg = NULL;
    }

    WHV_EMULATOR_CALLBACKS cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.Size = sizeof(cbs);
    cbs.WHvEmulatorIoPortCallback = emu_io;
    cbs.WHvEmulatorMemoryCallback = emu_mmio;
    cbs.WHvEmulatorGetVirtualProcessorRegisters = emu_get_regs;
    cbs.WHvEmulatorSetVirtualProcessorRegisters = emu_set_regs;
    cbs.WHvEmulatorTranslateGvaPage = emu_translate;

    WHV_EMULATOR_HANDLE emulator = NULL;
    WHvEmulatorCreateEmulator(&cbs, &emulator);

    emu_ctx_t ectx; ectx.partition = partition; ectx.vblk = &vblk;

    printf("[vdisk] Booting via built-in VMM (no QEMU) -- kernel=%s\n", kernel_path);

    int running = 1;
    int result = 1;
    while (running) {
        WHV_RUN_VP_EXIT_CONTEXT exitCtx;
        memset(&exitCtx, 0, sizeof(exitCtx));
        HRESULT hr = WHvRunVirtualProcessor(partition, 0, &exitCtx, sizeof(exitCtx));
        if (FAILED(hr)) {
            printf("[vdisk] WHvRunVirtualProcessor failed (hr=0x%08lX).\n", hr);
            result = 0;
            break;
        }

        switch (exitCtx.ExitReason) {
        case WHvRunVpExitReasonX64IoPortAccess: {
            WHV_X64_IO_PORT_ACCESS_CONTEXT *io = &exitCtx.IoPortAccess;
            UINT8 val8 = (UINT8)io->Rax;
            if (uart_io(&uart, io->PortNumber, io->AccessInfo.IsWrite, &val8)) {
                if (!io->AccessInfo.IsWrite) {
                    WHV_REGISTER_NAME rn = WHvX64RegisterRax;
                    WHV_REGISTER_VALUE rv; memset(&rv, 0, sizeof(rv));
                    rv.Reg64 = (io->Rax & ~0xFFull) | val8;
                    WHvSetVirtualProcessorRegisters(partition, 0, &rn, 1, &rv);
                }
            }
            UINT64 newRip = exitCtx.VpContext.Rip + exitCtx.VpContext.InstructionLength;
            set_reg64(partition, WHvX64RegisterRip, newRip);
            break;
        }
        case WHvRunVpExitReasonMemoryAccess: {
            if (emulator) {
                WHV_EMULATOR_STATUS status;
                WHvEmulatorTryMmioEmulation(emulator, &ectx, &exitCtx.VpContext, &exitCtx.MemoryAccess, &status);
            }
            break;
        }
        case WHvRunVpExitReasonX64Halt:
            // A clean 'poweroff' inside Alpine ends in a CPU halt loop with
            // interrupts disabled -- that's our signal the session is over.
            running = 0;
            break;
        case WHvRunVpExitReasonCanceled:
            running = 0;
            break;
        case WHvRunVpExitReasonUnrecoverableException:
        case WHvRunVpExitReasonInvalidVpRegisterValue:
        case WHvRunVpExitReasonUnsupportedFeature:
            printf("[vdisk] Guest crashed during boot (exit reason 0x%X, rip=0x%llX).\n",
                   exitCtx.ExitReason, (unsigned long long)exitCtx.VpContext.Rip);
            running = 0;
            result = 0;
            break;
        default:
            // Unhandled but non-fatal exit reasons (e.g. interrupt window) --
            // just resume.
            break;
        }
    }

    if (emulator) WHvEmulatorDestroyEmulator(emulator);
    if (vblk.hImg) CloseHandle(vblk.hImg);
    WHvDeleteVirtualProcessor(partition, 0);
    VirtualFree(ram, 0, MEM_RELEASE);
    WHvDeletePartition(partition);
    g_ram = NULL;
    g_ram_size = 0;

    printf("\n[vdisk] Linux VM exited.\n");
    return result;
}
