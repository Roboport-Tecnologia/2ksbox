/*
 * d3dptkmd.c: the WDDM kernel-mode driver for the d3dpt-vga adapter on
 * Windows 7 (M18 step 2, ADR-022). Loaded by dxgkrnl.sys through
 * DxgkInitialize; the XP-model miniport (../../nt/d3dptvid.c) stays the
 * driver for XP and the fallback on Windows 7.
 *
 * Enough of WDDM 1.1 for Windows 7's GDI desktop: the child, the VidPN,
 * allocations, paging and cdd.dll's presents, executed by the CPU at
 * submit time (the command stream, below). What is still a stub names
 * itself in the QEMU log and fails. The log is the device's DEBUG
 * register, one character per write (doc 15's rule: the QEMU log, never
 * a debugger), so a boot shows what dxgkrnl asks for next.
 *
 * Build: guest-tools/build-wddm.cmd (the EWDK's MSBuild, kernel-mode
 * toolset, TargetVersion Windows7, Win32; docs/build-windows.md "The
 * WDDM driver"). The WDK's headers are read from the kit, never copied.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/* The DDI's structures at Windows 7's sizes. The kit's headers default to
 * WDDM 2.7 whatever TargetVersion says, and dxgkrnl on Windows 7 passes
 * its own, smaller ones (DXGK_DRIVERCAPS: 0x208 bytes), which a driver
 * checking OutputDataSize against the larger sizeof then refuses. */
#define DXGKDDI_INTERFACE_VERSION DXGKDDI_INTERFACE_VERSION_WIN7
#include <ntddk.h>
#include <dispmprt.h>
#include "../../../../../d3dpt/d3dpt_fb.h"
#include "../../../../../d3dpt/d3dpt_enc.h"
#include "../d3dpt_wddm.h"

#define D3DPT_TAG 'kd3d'

/* The DMA buffers, in system memory (segment set 0), each a run of this
 * driver's packets (the command stream, below). The user-mode driver's
 * command buffer is as large, and it leaves 4 KiB of it unused for the
 * registration packet Render puts in front of its records. */
#define D3DPT_DMA_SIZE (256 * 1024)
#define D3DPT_ALLOC_LIST D3DPT_WDDM_MAX_LIST
#define D3DPT_PATCH_LIST 4096

/* Segment 2, the aperture: system memory the "GPU" reaches through a page
 * table dxgkrnl fills (MAP_APERTURE_SEGMENT), here kernel mappings of the
 * pages. GDI's shadow surface lives there, locked for the CPU all along,
 * so it can never move to VRAM. Its GPU addresses start at a base no VRAM
 * address reaches, which keeps the two apart in a trace. */
#define D3DPT_AP_BASE 0x80000000u
#define D3DPT_AP_PAGES ((64u * 1024 * 1024) / PAGE_SIZE)

/* One adapter: the device has no multi-head and the INF installs one. */
typedef struct D3DPT_ADAPTER {
    PDEVICE_OBJECT pdo;
    DXGKRNL_INTERFACE dxgk;           /* dxgkrnl's callbacks, from StartDevice */
    DXGK_DEVICE_INFO info;            /* the PCI resources */
    PHYSICAL_ADDRESS vram_phys;       /* BAR 0 */
    ULONG vram_len;
    PHYSICAL_ADDRESS regs_phys;       /* BAR 1 */
    ULONG regs_len;
    volatile ULONG *regs;             /* BAR 1 mapped (kernel VA) */
    ULONG seg_size;                   /* VRAM below the Direct3D command window */
    struct { ULONG w, h, hz; } modes[64];   /* the host's 32-bpp modes (MODE_*) */
    ULONG nmodes;
    ULONG cur_w, cur_h, cur_pitch, cur_hz;  /* what CommitVidPn programmed */
    PUCHAR vram;                      /* the segment mapped (kernel VA): the "GPU" is the CPU */
    ULONG vram_map;                   /* bytes mapped: the segment, then the cursor image */
    ULONG cursor_off;                 /* VRAM offset of the cursor image (above the segment), 0 = none */
    ULONG cursor_hot_x, cursor_hot_y;
    PUCHAR *ap_va;                    /* segment 2, the aperture: each page's kernel VA, or NULL */
    struct D3DPT_AP_MAP { PVOID base; PMDL mdl; BOOLEAN mine; } *ap_map; /* at a mapping's first page */
    volatile LONG fence_done;         /* the last submission fence executed */
    volatile LONG fence_notify;       /* the fence the next DPC reports */
    volatile LONG preempt_fence;      /* a preemption request to answer, 0 for none */
    /* the vertical blank while dxgkrnl has it enabled (ControlInterrupt):
     * the device's interrupt (register set v6, CAP_IRQ), else a periodic
     * timer at the mode's refresh */
    BOOLEAN vsync_irq;                /* the device raises it (IRQ_VBLANK); CAP_IRQ */
    ULONG vsync_isr;                  /* interrupts taken, the first few logged */
    volatile ULONG irq_mask;          /* what IRQ_ENABLE holds: IRQ_DMA always (fence_irq), IRQ_VBLANK while dxgkrnl wants it */
    /* register set v7 (CAP_DMA): the device appends the user-mode driver's
     * records to the window from the DMA buffer (dma), and a submission's
     * fence is the device's DMA interrupt (fence_irq, with CAP_IRQ) */
    BOOLEAN dma, fence_irq;
    PUCHAR sub_va;                    /* the DMA buffer of the submission being run: system VA */
    PHYSICAL_ADDRESS sub_pa;          /* and its physical address (contiguous) */
    ULONG dma_errors;                 /* appends the device refused, the first few logged */
    KTIMER vsync_timer;
    KDPC vsync_dpc;
    volatile LONG vsync_on;
    BOOLEAN vsync_ready;              /* the timer and its DPC initialized (StartDevice) */
    volatile ULONG scan_addr;         /* what the scanout shows (OFFSET), as the vsync reports it */
    /* Direct3D: the command window (d3dpt_proto.h) the user-mode driver's
     * records are copied into at submit time, the host's surface handles
     * this driver hands out, and the handles of D3D allocations destroyed
     * since the last submission, released on the host by the next one */
    ULONG cmd_offset;                 /* the window's VRAM offset (CMD_OFFSET), 0: none */
    PUCHAR win;                       /* the window mapped */
    BOOLEAN d3d;                      /* a window and an executor on the host (D3D_STATUS) */
    d3dpt_enc enc;                    /* the window's writer (submissions are serial) */
    ULONG d3d_errors;                 /* batches the host refused, the first few logged */
    volatile LONG next_handle;
    KSPIN_LOCK rel_lock;
    ULONG rel_n;
    ULONG rel[256];
    ULONG ctx_rel_n;                  /* host contexts to destroy at the next submission (rel_lock) */
    ULONG ctx_rel[256];
} D3DPT_ADAPTER;

/* A device and a context are only names here: everything they would hold
 * lives in the adapter (one engine, executed on the CPU at submit time). */
typedef struct D3DPT_DEVICE {
    D3DPT_ADAPTER *a;
    HANDLE dxgk_device;
} D3DPT_DEVICE;

typedef struct D3DPT_CONTEXT {
    D3DPT_DEVICE *dev;
    ULONG host_ctx;                   /* the user-mode driver's host context (D3DPT_CTX_PRIV), 0: none */
} D3DPT_CONTEXT;

/* An allocation (hAllocation): what its private driver data said
 * (D3DPT_ALLOC_DESC, ../d3dpt_wddm.h) and, for a D3D one, what the host
 * was last told of it. */
typedef struct D3DPT_ALLOC {
    D3DPT_ALLOC_DESC d;
    SIZE_T size;
    ULONG handle;                     /* the host's handle (D3D allocations; 0: none) */
    ULONG host_addr;                  /* the VRAM offset the host has it registered at; ~0u: not registered */
    ULONG cur_seg, cur_addr;          /* where the last Patch saw it (dxgkrnl skips a Patch where nothing moved) */
} D3DPT_ALLOC;

/* The DEBUG register of the adapter that started last: stubs called with
 * no adapter handle (Unload, ControlEtwLogging) log through it too. */
static volatile ULONG *g_regs;

/* -------------------------------------------------------------- debug */

/* Before StartDevice has mapped the register BAR (DriverEntry, AddDevice,
 * a start that fails finding it) lines go to QEMU's debug console port,
 * 0xE9, which `-debugcon file:<log>` captures (its default port); with no
 * debugcon on the board the writes go nowhere. */
#define DEBUGCON_PORT ((PUCHAR)0xe9)

static void dbg_puts(const char *s)
{
    volatile ULONG *r = g_regs;

    while (*s) {
        if (r) {
            r[D3DPT_FB_REG_DEBUG / 4] = (ULONG)(unsigned char)*s++;
        } else {
            WRITE_PORT_UCHAR(DEBUGCON_PORT, (UCHAR)*s++);
        }
    }
}

static void dbg_hex(const char *tag, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    int i;

    dbg_puts(tag);
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - 4 * i)) & 0xf];
    }
    buf[10] = 0;
    dbg_puts(buf);
}

static void dbg_line(const char *s)
{
    dbg_puts("d3dptkmd: ");
    dbg_puts(s);
    dbg_puts("\n");
}

/* The command window's doorbell: the host runs the batch inside this
 * write. The window is write-combined, so the records are pushed out
 * first. */
static void d3d_doorbell(d3dpt_enc *e)
{
    D3DPT_ADAPTER *a = CONTAINING_RECORD(e, D3DPT_ADAPTER, enc);
    const d3dpt_shm_hdr *h = (const d3dpt_shm_hdr *)e->shm;

    KeMemoryBarrier();
    a->regs[D3DPT_FB_REG_DOORBELL / 4] = 1;
    if (h->ret_status != D3DPT_ERR_OK && a->d3d_errors++ < 16) {
        dbg_hex("d3dptkmd: the host refused a batch: error ", h->ret_status);
        dbg_hex(" at record ", h->ret_index);
        dbg_puts("\n");
    }
}

/* ------------------------------------------------- the device's life */

static DXGKDDI_ADD_DEVICE d3dpt_add_device;
static NTSTATUS d3dpt_add_device(IN_CONST_PDEVICE_OBJECT pdo, OUT_PPVOID ctx)
{
    D3DPT_ADAPTER *a;

    if (!pdo || !ctx) {
        return STATUS_INVALID_PARAMETER;
    }
    a = ExAllocatePoolWithTag(NonPagedPool, sizeof(*a), D3DPT_TAG);
    if (!a) {
        return STATUS_NO_MEMORY;
    }
    RtlZeroMemory(a, sizeof(*a));
    a->pdo = pdo;
    *ctx = a;
    dbg_line("AddDevice");
    return STATUS_SUCCESS;
}

/* BAR 0 (VRAM) and BAR 1 (the registers), found by address. A VGA-class
 * device's resource list also carries the legacy window at 0xA0000 and
 * the VGA ports, ahead of the BARs, so counting memory resources (what
 * the XP miniport's VideoPortGetAccessRanges, BARs only, allowed) maps
 * the wrong range: the BARs' addresses come from PCI config space and
 * the translated resources are matched against them. */
static NTSTATUS find_bars(D3DPT_ADAPTER *a)
{
    PCM_RESOURCE_LIST list = a->info.TranslatedResourceList;
    ULONG bar[2], got = 0, f, i, found = 0;
    NTSTATUS st;

    if (!list) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    st = a->dxgk.DxgkCbReadDeviceSpace(a->dxgk.DeviceHandle, DXGK_WHICHSPACE_CONFIG,
                                       bar, 0x10, sizeof(bar), &got);
    if (!NT_SUCCESS(st) || got != sizeof(bar)) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    bar[0] &= ~0xfu;
    bar[1] &= ~0xfu;
    for (f = 0; f < list->Count; f++) {
        PCM_PARTIAL_RESOURCE_LIST pl = &list->List[f].PartialResourceList;

        for (i = 0; i < pl->Count; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &pl->PartialDescriptors[i];

            if (d->Type != CmResourceTypeMemory || d->u.Memory.Start.HighPart) {
                continue;
            }
            if (d->u.Memory.Start.LowPart == bar[0]) {
                a->vram_phys = d->u.Memory.Start;
                a->vram_len = d->u.Memory.Length;
                found |= 1;
            } else if (d->u.Memory.Start.LowPart == bar[1]) {
                a->regs_phys = d->u.Memory.Start;
                a->regs_len = d->u.Memory.Length;
                found |= 2;
            }
        }
    }
    return found == 3 ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
}

/* The host's mode table (the player decides what the guest can pick, as
 * for the XP driver), 32 bpp only: the desktop's primary is X8R8G8B8. A
 * mode whose frame does not fit the segment is left out. */
static void read_modes(D3DPT_ADAPTER *a)
{
    volatile ULONG *r = a->regs;
    ULONG n = r[D3DPT_FB_REG_MODE_COUNT / 4], i;

    a->nmodes = 0;
    for (i = 0; i < n && a->nmodes < RTL_NUMBER_OF(a->modes); i++) {
        ULONG w, h, bpp, hz;

        r[D3DPT_FB_REG_MODE_SEL / 4] = i;
        w = r[D3DPT_FB_REG_MODE_W / 4];
        h = r[D3DPT_FB_REG_MODE_H / 4];
        bpp = r[D3DPT_FB_REG_MODE_BPP / 4];
        hz = r[D3DPT_FB_REG_MODE_HZ / 4];
        if (bpp != 32 || !w || !h || w * h * 4 > a->seg_size) {
            continue;
        }
        a->modes[a->nmodes].w = w;
        a->modes[a->nmodes].h = h;
        a->modes[a->nmodes].hz = hz ? hz : 60;
        a->nmodes++;
    }
    dbg_hex("d3dptkmd: modes ", a->nmodes);
    dbg_hex(" of ", n);
    dbg_puts("\n");
}

static void unmap(D3DPT_ADAPTER *a);
static KDEFERRED_ROUTINE vsync_tick;
static void vsync_stop(D3DPT_ADAPTER *a);

static DXGKDDI_START_DEVICE d3dpt_start_device;
static NTSTATUS d3dpt_start_device(IN_CONST_PVOID ctx, IN_PDXGK_START_INFO start,
                                   IN_PDXGKRNL_INTERFACE dxgk, OUT_PULONG sources,
                                   OUT_PULONG children)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;
    NTSTATUS st;
    ULONG magic, version;

    UNREFERENCED_PARAMETER(start);
    if (!a || !dxgk || !sources || !children) {
        return STATUS_INVALID_PARAMETER;
    }
    a->dxgk = *dxgk;
    st = a->dxgk.DxgkCbGetDeviceInformation(a->dxgk.DeviceHandle, &a->info);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = find_bars(a);
    if (!NT_SUCCESS(st)) {
        dbg_hex("d3dptkmd: StartDevice: no BAR 0 / BAR 1 in the resources ", st);
        dbg_puts("\n");
        return st;
    }
    if (a->regs_len < D3DPT_FB_REGS_SIZE) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    a->regs = MmMapIoSpace(a->regs_phys, D3DPT_FB_REGS_SIZE, MmNonCached);
    if (!a->regs) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    g_regs = a->regs;

    magic = a->regs[D3DPT_FB_REG_MAGIC / 4];
    version = a->regs[D3DPT_FB_REG_VERSION / 4];
    dbg_puts("d3dptkmd: StartDevice");
    dbg_hex(" magic=", magic);
    dbg_hex(" version=", version);
    dbg_hex(" vram=", a->vram_len);
    dbg_puts("\n");
    /* register set v5 is what this driver needs; v6's interrupt is used
     * when CAP_IRQ says it is there, a timer stands in otherwise */
    if (magic != D3DPT_FB_MAGIC || version < 5u) {
        dbg_line("not a d3dpt-vga register set this driver knows");
        g_regs = NULL;
        MmUnmapIoSpace((PVOID)a->regs, D3DPT_FB_REGS_SIZE);
        a->regs = NULL;
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    /* The memory segment is VRAM up to the command window the executor
     * reads (its top 64 MiB, d3dpt_fb.h); a device with no window gives
     * all of it. */
    a->seg_size = a->regs[D3DPT_FB_REG_CMD_OFFSET / 4];
    if (a->seg_size == 0 || a->seg_size > a->vram_len) {
        a->seg_size = a->vram_len;
    }

    /* The hardware cursor's image (register set v4) takes the top of what
     * is left, outside the segment dxgkrnl manages. */
    a->vram_map = a->seg_size;
    a->cursor_off = 0;
    if ((a->regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_CURSOR) && a->seg_size > 2 * D3DPT_FB_CURSOR_BYTES) {
        a->cursor_off = (a->seg_size - D3DPT_FB_CURSOR_BYTES) & ~(PAGE_SIZE - 1);
        a->seg_size = a->cursor_off;
        /* SetPointerPosition says when the pointer hides; without this
         * the device hides it from every flip (its 9x / XP rule), and DWM
         * flips at every composed frame */
        if (version >= 8u) {
            a->regs[D3DPT_FB_REG_CURSOR_FLAGS / 4] = D3DPT_FB_CURSOR_OWNED;
        }
    }

    read_modes(a);

    /* The segment as the "GPU" sees it: paging transfers, fills and the
     * presents' blits run on the CPU at submit time, through this. */
    a->vram = MmMapIoSpace(a->vram_phys, a->vram_map, MmWriteCombined);
    if (!a->vram) {
        dbg_line("StartDevice: cannot map the segment");
        unmap(a);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    /* The Direct3D command window, when the device has one and the host an
     * executor to run it (reading D3D_STATUS loads the library). */
    a->cmd_offset = a->regs[D3DPT_FB_REG_CMD_OFFSET / 4];
    a->d3d = FALSE;
    if (a->cmd_offset && a->cmd_offset + D3DPT_SHM_SIZE <= a->vram_len &&
        a->regs[D3DPT_FB_REG_D3D_STATUS / 4] == D3DPT_STATUS_READY) {
        PHYSICAL_ADDRESS wp = a->vram_phys;

        wp.QuadPart += a->cmd_offset;
        a->win = MmMapIoSpace(wp, D3DPT_SHM_SIZE, MmWriteCombined);
        a->d3d = a->win != NULL;
        if (a->d3d) {
            d3dpt_enc_init(&a->enc, a->win, d3d_doorbell);
        }
    }
    a->d3d_errors = 0;
    a->next_handle = 0;
    a->rel_n = 0;
    a->ctx_rel_n = 0;
    KeInitializeSpinLock(&a->rel_lock);
    dbg_hex("d3dptkmd: Direct3D window at ", a->d3d ? a->cmd_offset : 0);
    dbg_puts("\n");
    a->ap_va = ExAllocatePoolWithTag(NonPagedPool, D3DPT_AP_PAGES * sizeof(*a->ap_va), D3DPT_TAG);
    a->ap_map = ExAllocatePoolWithTag(NonPagedPool, D3DPT_AP_PAGES * sizeof(*a->ap_map), D3DPT_TAG);
    if (!a->ap_va || !a->ap_map) {
        unmap(a);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(a->ap_va, D3DPT_AP_PAGES * sizeof(*a->ap_va));
    RtlZeroMemory(a->ap_map, D3DPT_AP_PAGES * sizeof(*a->ap_map));
    a->fence_done = a->fence_notify = a->preempt_fence = 0;
    a->vsync_on = 0;
    a->scan_addr = 0;
    a->vsync_irq = version >= 6u && (a->regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_IRQ);
    a->vsync_isr = 0;
    a->dma = a->d3d && version >= 7u && (a->regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_DMA);
    a->fence_irq = a->vsync_irq && version >= 7u && (a->regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_DMA);
    a->dma_errors = 0;
    a->irq_mask = a->fence_irq ? D3DPT_FB_IRQ_DMA : 0;
    if (a->vsync_irq) {
        a->regs[D3DPT_FB_REG_IRQ_STATUS / 4] = ~0u;
        a->regs[D3DPT_FB_REG_IRQ_ENABLE / 4] = a->irq_mask;   /* the vertical blank when dxgkrnl enables it */
    }
    dbg_line(a->vsync_irq ? "vertical blank: the device's interrupt" : "vertical blank: a timer (no CAP_IRQ)");
    dbg_line(a->fence_irq ? "fences: the device's interrupt" : "fences: reported at submit (no CAP_DMA / CAP_IRQ)");
    dbg_line(a->dma ? "records: appended by the device from the DMA buffer" : "records: copied into the window by the CPU");
    KeInitializeTimer(&a->vsync_timer);
    KeInitializeDpc(&a->vsync_dpc, vsync_tick, a);
    a->vsync_ready = TRUE;

    /* one scanout, one monitor */
    *sources = 1;
    *children = 1;
    return STATUS_SUCCESS;
}

static void unmap(D3DPT_ADAPTER *a)
{
    ULONG i;

    if (a->vsync_ready) {           /* no tick may run past this */
        vsync_stop(a);
        if (a->vsync_irq && a->regs) {      /* and no interrupt of the device's */
            a->irq_mask = 0;
            a->regs[D3DPT_FB_REG_IRQ_ENABLE / 4] = 0;
        }
        KeFlushQueuedDpcs();
        a->vsync_ready = FALSE;
    }
    if (a->ap_map) {
        for (i = 0; i < D3DPT_AP_PAGES; i++) {
            if (a->ap_map[i].base && a->ap_map[i].mine) {
                MmUnmapLockedPages(a->ap_map[i].base, a->ap_map[i].mdl);
            }
        }
        ExFreePoolWithTag(a->ap_map, D3DPT_TAG);
        a->ap_map = NULL;
    }
    if (a->ap_va) {
        ExFreePoolWithTag(a->ap_va, D3DPT_TAG);
        a->ap_va = NULL;
    }
    if (a->vram) {
        MmUnmapIoSpace(a->vram, a->vram_map);
        a->vram = NULL;
    }
    if (a->win) {
        MmUnmapIoSpace(a->win, D3DPT_SHM_SIZE);
        a->win = NULL;
        a->d3d = FALSE;
    }
    if (a->regs) {
        if (g_regs == a->regs) {
            g_regs = NULL;
        }
        MmUnmapIoSpace((PVOID)a->regs, D3DPT_FB_REGS_SIZE);
        a->regs = NULL;
    }
}

static DXGKDDI_STOP_DEVICE d3dpt_stop_device;
static NTSTATUS d3dpt_stop_device(IN_CONST_PVOID ctx)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;

    dbg_line("StopDevice");
    unmap(a);
    return STATUS_SUCCESS;
}

static DXGKDDI_REMOVE_DEVICE d3dpt_remove_device;
static NTSTATUS d3dpt_remove_device(IN_CONST_PVOID ctx)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;

    dbg_line("RemoveDevice");
    if (a) {
        unmap(a);
        ExFreePoolWithTag(a, D3DPT_TAG);
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_UNLOAD d3dpt_unload;
static VOID d3dpt_unload(VOID)
{
    dbg_line("Unload");
}

/* ---------------------------------------------- what dxgkrnl asks next */

/* What the adapter can do, as dxgkrnl asks right after StartDevice: a
 * WDDM 1.0 GPU with one engine, 32-bit addresses, no overlays, no swizzling,
 * and the device's hardware cursor when it has one. */
static NTSTATUS driver_caps(const D3DPT_ADAPTER *a, DXGK_DRIVERCAPS *c)
{
    RtlZeroMemory(c, sizeof(*c));
    if (a->cursor_off) {            /* the device's cursor sprite, any of the three kinds */
        c->MaxPointerWidth = D3DPT_FB_CURSOR_MAX;
        c->MaxPointerHeight = D3DPT_FB_CURSOR_MAX;
        c->PointerCaps.Monochrome = 1;
        c->PointerCaps.Color = 1;
        c->PointerCaps.MaskedColor = 1;
    }
#ifdef _WIN64
    /* The device reads any guest-physical address (DMA_ADDR_HI) and the
     * aperture is the CPU's own mappings. Below 4 GB only, Windows 11 on
     * a guest with memory above it fails the adapter's start ("Not Enough
     * Quota", StartAdapter_AddAdapterFailed; track M20 step 5). */
    c->HighestAcceptableAddress.QuadPart = ~0ull;
#else
    c->HighestAcceptableAddress.QuadPart = 0xffffffffull;
#endif
    c->MaxAllocationListSlotId = 16;
    c->MaxQueuedFlipOnVSync = 1;
    c->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
    /* the blits are memcpy row by row: a screen-to-screen blit whose
     * rectangles overlap comes through a staging copy instead */
    c->PresentationCaps.NoOverlapScreenBlt = 1;
    c->WDDMVersion = DXGKDDI_WDDMv1;
    return STATUS_SUCCESS;
}

/* Segment 1: VRAM below the command window, linear and CPU visible
 * (BAR 0). Its GPU address is the VRAM offset itself, so an allocation's
 * segment address is what the OFFSET register takes. Segment 2: the
 * aperture (D3DPT_AP_BASE). The paging buffer is in system memory
 * (segment 0). */
static NTSTATUS query_segment(const D3DPT_ADAPTER *a, const DXGKARG_QUERYADAPTERINFO *q)
{
    DXGK_QUERYSEGMENTOUT *o = (DXGK_QUERYSEGMENTOUT *)q->pOutputData;
    DXGK_SEGMENTDESCRIPTOR *d;

    if (q->OutputDataSize < sizeof(*o)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!o->pSegmentDescriptor) {               /* the first call asks how many */
        o->NbSegment = 2;
        return STATUS_SUCCESS;
    }
    d = o->pSegmentDescriptor;
    RtlZeroMemory(d, 2 * sizeof(*d));
    d[0].BaseAddress.QuadPart = 0;
    d[0].CpuTranslatedAddress = a->vram_phys;
    d[0].Size = a->seg_size;
    d[0].CommitLimit = a->seg_size;
    d[0].Flags.CpuVisible = 1;
    d[1].BaseAddress.QuadPart = D3DPT_AP_BASE;
    d[1].Size = D3DPT_AP_PAGES * PAGE_SIZE;
    d[1].CommitLimit = D3DPT_AP_PAGES * PAGE_SIZE;
    d[1].Flags.Aperture = 1;
    o->NbSegment = 2;
    o->PagingBufferSegmentId = 0;
    o->PagingBufferSize = 64 * 1024;
    o->PagingBufferPrivateDataSize = 0;
    return STATUS_SUCCESS;
}

/* DXGKQAITYPE_64BITONLYCAPS: Windows 11 24H2's, newer than the EWDK
 * 10.0.19041 headers; undocumented past its name, a 4-byte output. */
#define D3DPT_QAITYPE_64BITONLYCAPS 47

static DXGKDDI_QUERYADAPTERINFO d3dpt_query_adapter_info;
static NTSTATUS APIENTRY d3dpt_query_adapter_info(IN_CONST_HANDLE h,
                                                  IN_CONST_PDXGKARG_QUERYADAPTERINFO q)
{
    const D3DPT_ADAPTER *a = (const D3DPT_ADAPTER *)h;
    NTSTATUS st;

    switch ((ULONG)q->Type) {
    case DXGKQAITYPE_DRIVERCAPS:
        st = q->OutputDataSize < sizeof(DXGK_DRIVERCAPS)
             ? STATUS_INVALID_PARAMETER : driver_caps(a, (DXGK_DRIVERCAPS *)q->pOutputData);
        break;
    case DXGKQAITYPE_QUERYSEGMENT:
        st = query_segment(a, q);
        break;
    case DXGKQAITYPE_UMDRIVERPRIVATE: {         /* the user-mode driver's OpenAdapter */
        D3DPT_UMD_INFO *u = (D3DPT_UMD_INFO *)q->pOutputData;

        if (q->OutputDataSize < sizeof(*u)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        RtlZeroMemory(u, sizeof(*u));
        u->magic = D3DPT_UMD_MAGIC;
        u->version = D3DPT_UMD_VERSION;
        u->fb_version = a->regs[D3DPT_FB_REG_VERSION / 4];
        u->ddflags = a->regs[D3DPT_FB_REG_DDFLAGS / 4];
        u->d3d = a->d3d;
        u->vram = a->vram_len;
        u->seg_size = a->seg_size;
        u->fb_caps = a->regs[D3DPT_FB_REG_CAPS / 4];
        st = STATUS_SUCCESS;
        break;
    }
    case D3DPT_QAITYPE_64BITONLYCAPS:
        /* Windows 11 asks it of every driver, and stops a driver that
         * fails it (track M20 step 5). Zero: no 64-bit-only claim. */
        RtlZeroMemory(q->pOutputData, q->OutputDataSize);
        st = STATUS_SUCCESS;
        break;
    default:
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    dbg_hex("d3dptkmd: QueryAdapterInfo type=", (ULONG)q->Type);
    dbg_hex(" out=", q->OutputDataSize);
    dbg_hex(" -> ", (ULONG)st);
    dbg_puts("\n");
    return st;
}

static BOOLEAN notify_vsync(PVOID ctx);

/* The device's interrupt (register set v6): a level-triggered INTx line,
 * possibly shared, so an interrupt with nothing in IRQ_STATUS is another
 * device's. The vertical blank is acknowledged here and reported as an
 * ISR reports it, with a DPC queued for dxgkrnl's half. */
static DXGKDDI_INTERRUPT_ROUTINE d3dpt_interrupt;
static BOOLEAN d3dpt_interrupt(IN_CONST_PVOID ctx, IN_ULONG msg)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;
    ULONG st;

    UNREFERENCED_PARAMETER(msg);
    if (!a->vsync_irq || !a->regs) {
        return FALSE;
    }
    st = a->regs[D3DPT_FB_REG_IRQ_STATUS / 4];
    if (!st) {
        return FALSE;
    }
    a->regs[D3DPT_FB_REG_IRQ_STATUS / 4] = st;
    if (st & D3DPT_FB_IRQ_DMA) {
        DXGKARGCB_NOTIFY_INTERRUPT_DATA n;

        /* the last fence written is done, and with it every earlier one */
        a->fence_notify = (LONG)a->regs[D3DPT_FB_REG_FENCE_DONE / 4];
        RtlZeroMemory(&n, sizeof(n));
        n.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
        n.DmaCompleted.SubmissionFenceId = (UINT)a->fence_notify;
        a->dxgk.DxgkCbNotifyInterrupt(a->dxgk.DeviceHandle, &n);
        if (!(st & D3DPT_FB_IRQ_VBLANK) || !a->vsync_on) {
            a->dxgk.DxgkCbQueueDpc(a->dxgk.DeviceHandle);
        }
    }
    if ((st & D3DPT_FB_IRQ_VBLANK) && a->vsync_on) {
        a->vsync_isr++;
        notify_vsync(a);            /* queues the DPC */
    }
    return TRUE;
}

/* Queued after every completion reported from a submit (complete_fence):
 * dxgkrnl's own DPC work, the scheduler's half of DMA_COMPLETED. */
static DXGKDDI_DPC_ROUTINE d3dpt_dpc;
static VOID d3dpt_dpc(IN_CONST_PVOID ctx)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;
    static ULONG logged;

    if (logged < 4) {
        logged++;
        dbg_hex("d3dptkmd: DPC, fence ", (ULONG)a->fence_notify);
        dbg_puts("\n");
    }
    a->dxgk.DxgkCbNotifyDpc(a->dxgk.DeviceHandle);
}

static DXGKDDI_CONTROL_ETW_LOGGING d3dpt_control_etw_logging;
static VOID d3dpt_control_etw_logging(IN_BOOLEAN enable, IN_ULONG flags, IN_UCHAR level)
{
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(level);
    dbg_hex("d3dptkmd: ControlEtwLogging enable=", (ULONG)enable);
    dbg_puts("\n");
}

/*
 * The stubs. Each is declared with the WDK's own function type first, so
 * the compiler checks it against the header, then logs its name and
 * fails. Most DDIs take a handle and one argument structure.
 */
#define STUB1(type, fn, argt)                                   \
    static type fn;                                             \
    static NTSTATUS APIENTRY fn(argt a)                         \
    {                                                           \
        UNREFERENCED_PARAMETER(a);                              \
        dbg_line(#fn);                                          \
        return STATUS_NOT_SUPPORTED;                            \
    }
#define STUB2(type, fn, ht, argt)                               \
    static type fn;                                             \
    static NTSTATUS APIENTRY fn(ht h, argt a)                   \
    {                                                           \
        UNREFERENCED_PARAMETER(h);                              \
        UNREFERENCED_PARAMETER(a);                              \
        dbg_line(#fn);                                          \
        return STATUS_NOT_SUPPORTED;                            \
    }

/* the miniport half (dispmprt.h); no APIENTRY there, so not STUB2 */
static DXGKDDI_DISPATCH_IO_REQUEST d3dpt_dispatch_io_request;
static NTSTATUS d3dpt_dispatch_io_request(IN_CONST_PVOID ctx, IN_ULONG source,
                                          IN_PVIDEO_REQUEST_PACKET vrp)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(source);
    UNREFERENCED_PARAMETER(vrp);
    dbg_line("DispatchIoRequest");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_QUERY_CHILD_RELATIONS d3dpt_query_child_relations;
/* One child: the video output the player's window is, always connected.
 * Its UID is the video present target's ID in the VidPN. */
static NTSTATUS d3dpt_query_child_relations(IN_CONST_PVOID ctx,
                                            PDXGK_CHILD_DESCRIPTOR rel, ULONG size)
{
    UNREFERENCED_PARAMETER(ctx);
    dbg_hex("d3dptkmd: QueryChildRelations size=", size);
    dbg_hex(" each=", (ULONG)sizeof(*rel));
    dbg_puts("\n");
    if (!rel || size < sizeof(*rel)) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    RtlZeroMemory(rel, size);
    rel[0].ChildDeviceType = TypeVideoOutput;
    /* a VGA connector, as VirtualBox's WDDM driver reports its outputs on
     * Windows 7 (D3DKMDT_VOT_OTHER: StopDevice right after this) */
    rel[0].ChildCapabilities.Type.VideoOutput.InterfaceTechnology = D3DKMDT_VOT_HD15;
    rel[0].ChildCapabilities.Type.VideoOutput.MonitorOrientationAwareness = D3DKMDT_MOA_NONE;
    rel[0].ChildCapabilities.Type.VideoOutput.SupportsSdtvModes = FALSE;
    /* not AlwaysConnected, which dxgkrnl keeps for integrated panels: the
     * Basic Display Driver's choice, connected through QueryChildStatus */
    rel[0].ChildCapabilities.HpdAwareness = HpdAwarenessInterruptible;
    rel[0].AcpiUid = 0;
    rel[0].ChildUid = 0;
    return STATUS_SUCCESS;
}

static DXGKDDI_QUERY_CHILD_STATUS d3dpt_query_child_status;
static NTSTATUS d3dpt_query_child_status(IN_CONST_PVOID ctx, INOUT_PDXGK_CHILD_STATUS st,
                                         IN_BOOLEAN non_destructive)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(non_destructive);
    dbg_hex("d3dptkmd: QueryChildStatus type=", (ULONG)st->Type);
    dbg_puts("\n");
    switch (st->Type) {
    case StatusConnection:
        st->HotPlug.Connected = TRUE;
        return STATUS_SUCCESS;
    case StatusRotation:
        st->Rotation.Angle = 0;
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/*
 * The monitor's EDID, made up: with none Windows starts the desktop at
 * 640x480. One block, EDID 1.3, manufacturer "TKS", its preferred timing
 * 1024x768 at 60 Hz (VESA DMT: 65 MHz, 1344x806 total), the established
 * 640x480 / 800x600 / 1024x768, the name "2ksbox". The rest of the host's
 * mode table still comes from RecommendMonitorModes.
 */
static const UCHAR edid_block[128] = {
    0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00,     /* header */
    0x51, 0x73, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,     /* "TKS", product 1, no serial */
    0x01, 0x24, 0x01, 0x03,                             /* week 1 of 2026, EDID 1.3 */
    0x80, 0x22, 0x1b, 0x78, 0x06,                       /* digital, 34x27 cm, gamma 2.2, sRGB + preferred */
    0xee, 0x91, 0xa3, 0x54, 0x4c, 0x99, 0x26, 0x0f, 0x50, 0x54, /* sRGB chromaticity */
    0x21, 0x08, 0x00,                                   /* 640x480, 800x600, 1024x768 at 60 */
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,     /* no standard timings */
    0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    /* detailed timing 1: 1024x768 at 60 */
    0x64, 0x19, 0x00, 0x40, 0x41, 0x00, 0x26, 0x30,
    0x18, 0x88, 0x36, 0x00, 0x54, 0x0e, 0x11, 0x00, 0x00, 0x18,
    /* monitor name */
    0x00, 0x00, 0x00, 0xfc, 0x00, '2', 'k', 's', 'b', 'o', 'x', 0x0a,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    /* range limits: 50-75 Hz, 30-80 kHz, 170 MHz */
    0x00, 0x00, 0x00, 0xfd, 0x00, 0x32, 0x4b, 0x1e, 0x50, 0x11, 0x00, 0x0a,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    /* dummy */
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00                                          /* no extensions, checksum (set below) */
};

static DXGKDDI_QUERY_DEVICE_DESCRIPTOR d3dpt_query_device_descriptor;
static NTSTATUS d3dpt_query_device_descriptor(IN_CONST_PVOID ctx, IN_ULONG uid,
                                              INOUT_PDXGK_DEVICE_DESCRIPTOR desc)
{
    UCHAR sum = 0;
    ULONG i, n;

    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(uid);
    dbg_hex("d3dptkmd: QueryDeviceDescriptor offset ", desc->DescriptorOffset);
    dbg_hex(" length ", desc->DescriptorLength);
    dbg_puts("\n");
    if (desc->DescriptorOffset >= sizeof(edid_block)) {
        return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
    }
    n = sizeof(edid_block) - desc->DescriptorOffset;
    if (n > desc->DescriptorLength) {
        n = desc->DescriptorLength;
    }
    for (i = 0; i < sizeof(edid_block) - 1; i++) {
        sum = (UCHAR)(sum + edid_block[i]);
    }
    for (i = 0; i < n; i++) {
        ULONG at = desc->DescriptorOffset + i;

        ((PUCHAR)desc->DescriptorBuffer)[i] =
            at == sizeof(edid_block) - 1 ? (UCHAR)(0x100 - sum) : edid_block[at];
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_SET_POWER_STATE d3dpt_set_power_state;
static NTSTATUS d3dpt_set_power_state(IN_CONST_PVOID ctx, IN_ULONG uid,
                                      IN_DEVICE_POWER_STATE state, IN_POWER_ACTION action)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(uid);
    UNREFERENCED_PARAMETER(action);
    dbg_hex("d3dptkmd: SetPowerState D", (ULONG)state - 1);
    dbg_puts("\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_NOTIFY_ACPI_EVENT d3dpt_notify_acpi_event;
static NTSTATUS d3dpt_notify_acpi_event(IN_CONST_PVOID ctx, IN_DXGK_EVENT_TYPE type,
                                        IN_ULONG ev, IN_PVOID arg, OUT_PULONG flags)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(type);
    UNREFERENCED_PARAMETER(ev);
    UNREFERENCED_PARAMETER(arg);
    UNREFERENCED_PARAMETER(flags);
    dbg_line("NotifyAcpiEvent");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_RESET_DEVICE d3dpt_reset_device;
static VOID d3dpt_reset_device(IN_CONST_PVOID ctx)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;

    dbg_line("ResetDevice");
    /* bugcheck or reboot: hand the screen back to the VGA core, as the
     * XP miniport's reset does */
    if (a && a->regs) {
        a->regs[D3DPT_FB_REG_ENABLE / 4] = 0;
    }
}

static DXGKDDI_QUERY_INTERFACE d3dpt_query_interface;
static NTSTATUS d3dpt_query_interface(IN_CONST_PVOID ctx, IN_PQUERY_INTERFACE qi)
{
    UNREFERENCED_PARAMETER(ctx);
    dbg_hex("d3dptkmd: QueryInterface guid=", qi->InterfaceType ? qi->InterfaceType->Data1 : 0);
    dbg_hex(" version=", (ULONG)qi->Version);
    dbg_puts("\n");
    return STATUS_NOT_SUPPORTED;
}

/* the adapter, device, context and overlay halves (d3dkmddi.h) still to do */
STUB2(DXGKDDI_ACQUIRESWIZZLINGRANGE, d3dpt_acquire_swizzling_range, IN_CONST_HANDLE, INOUT_PDXGKARG_ACQUIRESWIZZLINGRANGE)
STUB2(DXGKDDI_RELEASESWIZZLINGRANGE, d3dpt_release_swizzling_range, IN_CONST_HANDLE, IN_CONST_PDXGKARG_RELEASESWIZZLINGRANGE)
STUB2(DXGKDDI_SETPALETTE, d3dpt_set_palette, IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETPALETTE)

/* The user-mode driver's escapes (D3DPT_ESC): its log lines, into the
 * DEBUG register beside this driver's. dxgkrnl hands a kernel copy of the
 * data. */
static DXGKDDI_ESCAPE d3dpt_escape;
static NTSTATUS APIENTRY d3dpt_escape(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_ESCAPE e)
{
    D3DPT_ESC *m = (D3DPT_ESC *)e->pPrivateDriverData;

    UNREFERENCED_PARAMETER(h);
    if (!m || e->PrivateDriverDataSize < sizeof(*m) || m->magic != D3DPT_ESC_MAGIC) {
        dbg_hex("d3dptkmd: Escape not ours, size ", e->PrivateDriverDataSize);
        dbg_puts("\n");
        return STATUS_INVALID_PARAMETER;
    }
    switch (m->op) {
    case D3DPT_ESC_LOG:
        m->text[D3DPT_ESC_TEXT - 1] = 0;
        dbg_puts("d3dptumd: ");
        dbg_puts(m->text);
        dbg_puts("\n");
        return STATUS_SUCCESS;
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

/* ------------------------------------------------------ hardware cursor
 * (register set v4, doc 15 "The hardware cursor"), as the XP display
 * driver does it: the pointer converted to a8r8g8b8 into the VRAM above
 * the segment, the device told, the host showing it as its own cursor.
 * Larger pointers than D3DPT_FB_CURSOR_MAX are refused, and dxgkrnl then
 * has GDI draw them. */
static ULONG ptr_bit(const UCHAR *rows, ULONG pitch, ULONG row, ULONG x)
{
    return (rows[row * pitch + (x >> 3)] >> (7 - (x & 7))) & 1;
}

static DXGKDDI_SETPOINTERSHAPE d3dpt_set_pointer_shape;
static NTSTATUS APIENTRY d3dpt_set_pointer_shape(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_SETPOINTERSHAPE s)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;
    const UCHAR *px = (const UCHAR *)s->pPixels;
    ULONG w = s->Width, ht = s->Height, i, j;
    PULONG img;
    static ULONG logged;

    if (logged < 8) {
        logged++;
        dbg_hex("d3dptkmd: SetPointerShape flags=", s->Flags.Value);
        dbg_hex(" ", w);
        dbg_hex(" x ", ht);
        dbg_hex(" pitch ", s->Pitch);
        dbg_puts("\n");
    }
    if (!a->cursor_off || !w || !ht || w > D3DPT_FB_CURSOR_MAX || ht > D3DPT_FB_CURSOR_MAX ||
        s->XHot >= w || s->YHot >= ht || !px) {
        return STATUS_NOT_SUPPORTED;
    }
    img = (PULONG)(a->vram + a->cursor_off);
    if (s->Flags.Monochrome) {
        /* AND rows then XOR rows: AND 1 / XOR 0 transparent, AND 0 black
         * or white by XOR, AND 1 / XOR 1 (invert) black, as a sprite
         * cannot invert */
        for (j = 0; j < ht; j++) {
            for (i = 0; i < w; i++) {
                ULONG and_ = ptr_bit(px, s->Pitch, j, i), xr = ptr_bit(px, s->Pitch, ht + j, i);

                img[j * w + i] = (and_ && !xr) ? 0 : (xr && !and_) ? 0xffffffffu : 0xff000000u;
            }
        }
    } else {
        for (j = 0; j < ht; j++) {
            const ULONG *row = (const ULONG *)(px + j * s->Pitch);

            for (i = 0; i < w; i++) {
                ULONG c = row[i];

                if (s->Flags.MaskedColor) {
                    /* the top byte is a mask: 0 draws the colour, 0xff XORs
                     * it, where black is no change (transparent) and any
                     * other colour shows as itself */
                    c = (c >> 24) && !(c & 0x00ffffffu) ? 0 : (c | 0xff000000u);
                }
                img[j * w + i] = c;
            }
        }
    }
    a->cursor_hot_x = s->XHot;
    a->cursor_hot_y = s->YHot;
    a->regs[D3DPT_FB_REG_CURSOR_ADDR / 4] = a->cursor_off;
    a->regs[D3DPT_FB_REG_CURSOR_W / 4] = w;
    a->regs[D3DPT_FB_REG_CURSOR_H / 4] = ht;
    a->regs[D3DPT_FB_REG_CURSOR_HOT_X / 4] = s->XHot;
    a->regs[D3DPT_FB_REG_CURSOR_HOT_Y / 4] = s->YHot;
    a->regs[D3DPT_FB_REG_CURSOR_DEFINE / 4] = 1;
    return STATUS_SUCCESS;
}

/* X and Y are the pointer image's corner; the device takes the hot spot. */
static DXGKDDI_SETPOINTERPOSITION d3dpt_set_pointer_position;
static NTSTATUS APIENTRY d3dpt_set_pointer_position(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_SETPOINTERPOSITION p)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;

    if (!a->cursor_off) {
        return STATUS_SUCCESS;
    }
    if (!p->Flags.Visible) {
        a->regs[D3DPT_FB_REG_CURSOR_ENABLE / 4] = 0;
        return STATUS_SUCCESS;
    }
    a->regs[D3DPT_FB_REG_CURSOR_X / 4] = (ULONG)(p->X + (INT)a->cursor_hot_x);
    a->regs[D3DPT_FB_REG_CURSOR_Y / 4] = (ULONG)(p->Y + (INT)a->cursor_hot_y);
    a->regs[D3DPT_FB_REG_CURSOR_ENABLE / 4] = 1;
    return STATUS_SUCCESS;
}

/* ------------------------------------------------- devices and contexts */

static DXGKDDI_CREATEDEVICE d3dpt_create_device;
static NTSTATUS APIENTRY d3dpt_create_device(IN_CONST_HANDLE h, INOUT_PDXGKARG_CREATEDEVICE c)
{
    D3DPT_DEVICE *d = ExAllocatePoolWithTag(NonPagedPool, sizeof(*d), D3DPT_TAG);

    if (!d) {
        return STATUS_NO_MEMORY;
    }
    d->a = (D3DPT_ADAPTER *)h;
    d->dxgk_device = c->hDevice;
    c->hDevice = d;
    /* Flags and pInfo share their storage, and Windows 7 hands a pointer
     * there (0x81e6a050 and the like, a kernel address): the device's DMA
     * buffer and list sizes, as the Vista-era interface reads them. */
    if ((ULONG_PTR)c->pInfo >= (ULONG_PTR)MM_SYSTEM_RANGE_START && MmIsAddressValid(c->pInfo)) {
        c->pInfo->DmaBufferSize = D3DPT_DMA_SIZE;
        c->pInfo->DmaBufferSegmentSet = 0;
        c->pInfo->DmaBufferPrivateDataSize = 0;
        c->pInfo->AllocationListSize = D3DPT_ALLOC_LIST;
        c->pInfo->PatchLocationListSize = D3DPT_PATCH_LIST;
        c->pInfo->Flags.Value = 0;
    }
    dbg_line("CreateDevice");
    return STATUS_SUCCESS;
}

static DXGKDDI_DESTROYDEVICE d3dpt_destroy_device;
static NTSTATUS APIENTRY d3dpt_destroy_device(IN_CONST_HANDLE h)
{
    dbg_line("DestroyDevice");
    ExFreePoolWithTag((PVOID)h, D3DPT_TAG);
    return STATUS_SUCCESS;
}

static DXGKDDI_CREATECONTEXT d3dpt_create_context;
static NTSTATUS APIENTRY d3dpt_create_context(IN_CONST_HANDLE h, INOUT_PDXGKARG_CREATECONTEXT c)
{
    D3DPT_CONTEXT *x = ExAllocatePoolWithTag(NonPagedPool, sizeof(*x), D3DPT_TAG);

    if (!x) {
        return STATUS_NO_MEMORY;
    }
    x->dev = (D3DPT_DEVICE *)h;
    x->host_ctx = 0;
    if (c->pPrivateDriverData && c->PrivateDriverDataSize >= sizeof(D3DPT_CTX_PRIV) &&
        ((const D3DPT_CTX_PRIV *)c->pPrivateDriverData)->magic == D3DPT_CTX_MAGIC) {
        x->host_ctx = ((const D3DPT_CTX_PRIV *)c->pPrivateDriverData)->host_ctx;
    }
    c->hContext = x;
    c->ContextInfo.DmaBufferSize = D3DPT_DMA_SIZE;
    c->ContextInfo.DmaBufferSegmentSet = 0;
    c->ContextInfo.DmaBufferPrivateDataSize = 0;
    c->ContextInfo.AllocationListSize = D3DPT_ALLOC_LIST;
    c->ContextInfo.PatchLocationListSize = D3DPT_PATCH_LIST;
    dbg_hex("d3dptkmd: CreateContext node=", c->NodeOrdinal);
    dbg_hex(" flags=", c->Flags.Value);
    dbg_puts("\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_DESTROYCONTEXT d3dpt_destroy_context;
static NTSTATUS APIENTRY d3dpt_destroy_context(IN_CONST_HANDLE h)
{
    D3DPT_CONTEXT *x = (D3DPT_CONTEXT *)h;

    /* the host's context goes at the next submission, as a release does:
     * only a submission writes the window. A second destroy after the
     * user-mode driver's own is a no-op there. */
    if (x->host_ctx) {
        D3DPT_ADAPTER *a = x->dev->a;
        KIRQL irql;

        KeAcquireSpinLock(&a->rel_lock, &irql);
        if (a->ctx_rel_n < RTL_NUMBER_OF(a->ctx_rel)) {
            a->ctx_rel[a->ctx_rel_n++] = x->host_ctx;
        }
        KeReleaseSpinLock(&a->rel_lock, irql);
    }
    ExFreePoolWithTag(x, D3DPT_TAG);
    return STATUS_SUCCESS;
}

/* --------------------------------------------------------- allocations */

static ULONG format_bpp(D3DDDIFORMAT f)
{
    switch (f) {
    case D3DDDIFMT_R5G6B5:
    case D3DDDIFMT_X1R5G5B5:
    case D3DDDIFMT_A1R5G5B5:
        return 2;
    case D3DDDIFMT_P8:
        return 1;
    default:
        return 4;
    }
}

/* What dxgkrnl's own surfaces are (the desktop's primary, the shadow GDI
 * draws into, a staging surface for readbacks), as the private data its
 * CreateAllocation then hands back. GDI surfaces need GDI acceleration,
 * which the caps do not claim. */
static DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA d3dpt_get_standard_allocation_driver_data;
static NTSTATUS APIENTRY d3dpt_get_standard_allocation_driver_data(IN_CONST_HANDLE h,
    INOUT_PDXGKARG_GETSTANDARDALLOCATIONDRIVERDATA g)
{
    D3DPT_ALLOC_DESC d;

    UNREFERENCED_PARAMETER(h);
    RtlZeroMemory(&d, sizeof(d));
    d.magic = D3DPT_ALLOC_MAGIC;
    switch (g->StandardAllocationType) {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
        d.kind = D3DPT_ALLOC_PRIMARY;
        d.w = g->pCreateSharedPrimarySurfaceData->Width;
        d.h = g->pCreateSharedPrimarySurfaceData->Height;
        d.format = g->pCreateSharedPrimarySurfaceData->Format;
        d.refresh_num = g->pCreateSharedPrimarySurfaceData->RefreshRate.Numerator;
        d.refresh_den = g->pCreateSharedPrimarySurfaceData->RefreshRate.Denominator;
        d.source = g->pCreateSharedPrimarySurfaceData->VidPnSourceId;
        break;
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
        d.kind = D3DPT_ALLOC_SHADOW;
        d.w = g->pCreateShadowSurfaceData->Width;
        d.h = g->pCreateShadowSurfaceData->Height;
        d.format = g->pCreateShadowSurfaceData->Format;
        break;
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
        d.kind = D3DPT_ALLOC_STAGING;
        d.w = g->pCreateStagingSurfaceData->Width;
        d.h = g->pCreateStagingSurfaceData->Height;
        d.format = D3DDDIFMT_X8R8G8B8;
        break;
    default:
        dbg_hex("d3dptkmd: GetStandardAllocationDriverData type ", (ULONG)g->StandardAllocationType);
        dbg_puts(" refused\n");
        return STATUS_NOT_SUPPORTED;
    }
    d.bpp = format_bpp((D3DDDIFORMAT)d.format);
    d.pitch = (d.w * d.bpp + 3) & ~3u;
    if (g->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE) {
        g->pCreateShadowSurfaceData->Pitch = d.pitch;
    } else if (g->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE) {
        g->pCreateStagingSurfaceData->Pitch = d.pitch;
    }
    g->AllocationPrivateDriverDataSize = sizeof(d);
    g->ResourcePrivateDriverDataSize = 0;
    if (g->pAllocationPrivateDriverData) {
        RtlCopyMemory(g->pAllocationPrivateDriverData, &d, sizeof(d));
    }
    return STATUS_SUCCESS;
}

/* Every allocation but a shared one is CPU visible (GDI locks the shadow
 * and staging surfaces, the primary is scanned out from VRAM, Direct3D
 * locks its resources) and is evicted to system memory. Windows 7's video
 * memory manager takes a shared CPU-visible allocation only in aperture
 * segments (dxgmms1's CreateOneAllocation: STATUS_INVALID_PARAMETER,
 * which DWM's redirection surface for a Direct3D window met), and the
 * host reads these from VRAM. A D3D allocation gets the host handle it
 * will be registered under. */
static DXGKDDI_CREATEALLOCATION d3dpt_create_allocation;
static NTSTATUS APIENTRY d3dpt_create_allocation(IN_CONST_HANDLE h, INOUT_PDXGKARG_CREATEALLOCATION c)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;
    ULONG i;

    for (i = 0; i < c->NumAllocations; i++) {
        DXGK_ALLOCATIONINFO *info = &c->pAllocationInfo[i];
        const D3DPT_ALLOC_DESC *d = (const D3DPT_ALLOC_DESC *)info->pPrivateDriverData;
        D3DPT_ALLOC *al;

        if (!d || info->PrivateDriverDataSize < sizeof(*d) || d->magic != D3DPT_ALLOC_MAGIC ||
            (d->kind == D3DPT_ALLOC_D3D && (!d->size || d->nlv > D3DPT_ALLOC_MAX_LV))) {
            dbg_hex("d3dptkmd: CreateAllocation: not ours, private data size ", info->PrivateDriverDataSize);
            dbg_puts("\n");
            goto fail;
        }
        al = ExAllocatePoolWithTag(NonPagedPool, sizeof(*al), D3DPT_TAG);
        if (!al) {
            goto fail;
        }
        al->d = *d;
        al->handle = 0;
        al->host_addr = ~0u;
        al->cur_seg = 0;
        al->cur_addr = 0;
        if (d->kind == D3DPT_ALLOC_D3D) {
            al->size = ROUND_TO_PAGES((SIZE_T)d->size);
            if (d->caps) {
                al->handle = (ULONG)InterlockedIncrement(&a->next_handle);
            }
        } else {
            al->size = ROUND_TO_PAGES((SIZE_T)d->pitch * d->h);
            /* the desktop's primary is a render target too: ddraw.dll's
             * full-screen flip chain opens it as one of its buffers */
            if (d->kind == D3DPT_ALLOC_PRIMARY) {
                al->d.caps = D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY;
                al->d.levels = 1;
                al->d.nlv = 0;
                al->handle = (ULONG)InterlockedIncrement(&a->next_handle);
            }
        }
        info->Alignment = 0;
        info->Size = al->size;
        info->PitchAlignedSize = 0;
        RtlZeroMemory(&info->HintedBank, sizeof(info->HintedBank));
        RtlZeroMemory(&info->PreferredSegment, sizeof(info->PreferredSegment));
        /* the primary is scanned out and the host reads Direct3D's
         * resources from VRAM, so those are VRAM only; the rest may stay in
         * system memory behind the aperture (bit 1: segment 2) */
        info->SupportedReadSegmentSet = d->kind == D3DPT_ALLOC_PRIMARY || d->kind == D3DPT_ALLOC_D3D ? 1 : 3;
        info->SupportedWriteSegmentSet = info->SupportedReadSegmentSet;
        info->EvictionSegmentSet = 0;
        info->MaximumRenamingListLength = 0;
        info->hAllocation = al;
        info->Flags.Value = 0;
        info->Flags.CpuVisible = d->kind != D3DPT_ALLOC_D3D || !d->shared;
        info->pAllocationUsageHint = NULL;
        info->AllocationPriority = d->kind == D3DPT_ALLOC_PRIMARY ? D3DDDI_ALLOCATIONPRIORITY_HIGH
                                                                  : D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        dbg_hex("d3dptkmd: CreateAllocation kind=", d->kind);
        dbg_hex(" ", d->w);
        dbg_hex(" x ", d->h);
        dbg_hex(" fmt ", (ULONG)d->format);
        if (d->kind == D3DPT_ALLOC_D3D) {
            dbg_hex(" caps ", d->caps);
            dbg_hex(" bytes ", d->size);
            dbg_hex(" handle ", al->handle);
        }
        dbg_puts("\n");
    }
    return STATUS_SUCCESS;

fail:
    while (i--) {
        ExFreePoolWithTag(c->pAllocationInfo[i].hAllocation, D3DPT_TAG);
        c->pAllocationInfo[i].hAllocation = NULL;
    }
    return STATUS_INVALID_PARAMETER;
}

static DXGKDDI_DESTROYALLOCATION d3dpt_destroy_allocation;
static NTSTATUS APIENTRY d3dpt_destroy_allocation(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_DESTROYALLOCATION d)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;
    ULONG i;

    dbg_hex("d3dptkmd: DestroyAllocation ", d->NumAllocations);
    dbg_puts("\n");
    for (i = 0; i < d->NumAllocations; i++) {
        D3DPT_ALLOC *al = (D3DPT_ALLOC *)d->pAllocationList[i];

        if (!al) {
            continue;
        }
        /* the host lets go of its copy at the next submission */
        if (al->handle && al->host_addr != ~0u) {
            KIRQL irql;

            KeAcquireSpinLock(&a->rel_lock, &irql);
            if (a->rel_n < RTL_NUMBER_OF(a->rel)) {
                a->rel[a->rel_n++] = al->handle;
            }
            KeReleaseSpinLock(&a->rel_lock, irql);
        }
        ExFreePoolWithTag(al, D3DPT_TAG);
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_DESCRIBEALLOCATION d3dpt_describe_allocation;
static NTSTATUS APIENTRY d3dpt_describe_allocation(IN_CONST_HANDLE h, INOUT_PDXGKARG_DESCRIBEALLOCATION d)
{
    const D3DPT_ALLOC *al = (const D3DPT_ALLOC *)d->hAllocation;

    UNREFERENCED_PARAMETER(h);
    d->Width = al->d.w;
    d->Height = al->d.h;
    d->Format = (D3DDDIFORMAT)al->d.format;
    d->MultisampleMethod.NumSamples = 0;
    d->MultisampleMethod.NumQualityLevels = 0;
    d->RefreshRate.Numerator = al->d.refresh_num;
    d->RefreshRate.Denominator = al->d.refresh_den;
    d->PrivateDriverFormatAttribute = 0;
    return STATUS_SUCCESS;
}

/* A device's handle on an allocation is the allocation itself. */
static DXGKDDI_OPENALLOCATIONINFO d3dpt_open_allocation;
static NTSTATUS APIENTRY d3dpt_open_allocation(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_OPENALLOCATION o)
{
    const D3DPT_DEVICE *dev = (const D3DPT_DEVICE *)h;
    ULONG i;

    for (i = 0; i < o->NumAllocations; i++) {
        DXGKARGCB_GETHANDLEDATA gd;
        D3DPT_ALLOC *al;

        RtlZeroMemory(&gd, sizeof(gd));
        gd.hObject = o->pOpenAllocation[i].hAllocation;
        gd.Type = DXGK_HANDLE_ALLOCATION;
        al = (D3DPT_ALLOC *)dev->a->dxgk.DxgkCbGetHandleData(&gd);
        if (!al || al->d.magic != D3DPT_ALLOC_MAGIC) {
            dbg_line("OpenAllocation: GetHandleData gave no allocation of ours");
            return STATUS_INVALID_PARAMETER;
        }
        o->pOpenAllocation[i].hDeviceSpecificAllocation = al;
    }
    dbg_hex("d3dptkmd: OpenAllocation ", o->NumAllocations);
    dbg_puts("\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_CLOSEALLOCATION d3dpt_close_allocation;
static NTSTATUS APIENTRY d3dpt_close_allocation(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_CLOSEALLOCATION c)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(c);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------ the command stream */

/*
 * The DMA buffers hold this driver's own packets, executed on the CPU when
 * dxgkrnl submits them (SubmitCommand): there is no GPU behind the
 * segment, only memory both sides map. Every packet starts with a header
 * whose size takes the reader to the next one.
 */
#define D3DPT_PKT_MAGIC 0x3d504b54u             /* "TKP=" */
enum { PKT_TRANSFER = 1, PKT_FILL, PKT_BLT, PKT_COLORFILL, PKT_FLIP, PKT_MAP, PKT_UNMAP, PKT_REG, PKT_D3D, PKT_FIX };

typedef struct PKT_HDR { ULONG magic, op, size; } PKT_HDR;

/* one side of a paging transfer: a segment address, or system memory */
typedef struct PKT_MEM { ULONG seg, addr; PMDL mdl; ULONG mdl_off; } PKT_MEM;

typedef struct PKT_TRANSFER_T { PKT_HDR h; ULONG bytes; PKT_MEM src, dst; } PKT_TRANSFER_T;
typedef struct PKT_FILL_T { PKT_HDR h; ULONG bytes, pattern, seg, addr; } PKT_FILL_T;

/* A surface of a present: its slot in the allocation list, where it is
 * (from that list at Present, again at Patch), its pitch and pixel size. */
typedef struct PKT_SURF { ULONG index, seg, addr, pitch, bpp; } PKT_SURF;
/* a destination rectangle and the source corner it copies from */
typedef struct PKT_RECT { LONG sx, sy, l, t, r, b; } PKT_RECT;
typedef struct PKT_BLT_T { PKT_HDR h; PKT_SURF src, dst; ULONG color, n; PKT_RECT r[1]; } PKT_BLT_T;
typedef struct PKT_FLIP_T { PKT_HDR h; PKT_SURF src; } PKT_FLIP_T;
/* aperture pages first..first+n-1 onto an MDL's pages from mdl_off on */
typedef struct PKT_MAP_T { PKT_HDR h; ULONG first, n; PMDL mdl; ULONG mdl_off, cached; } PKT_MAP_T;
/* A user-mode driver's submission (Render): every allocation of its list
 * (seg / addr / al filled at Patch), registered with the host where it is
 * new there, then the records, which go into the command window as they
 * are. */
typedef struct PKT_REG_E { ULONG index, seg, addr; struct D3DPT_ALLOC *al; } PKT_REG_E;
typedef struct PKT_REG_T { PKT_HDR h; ULONG n; PKT_REG_E e[1]; } PKT_REG_T;
typedef struct PKT_D3D_T { PKT_HDR h; ULONG count; } PKT_D3D_T;
/* The records' own patches (D3DPT_PATCH_*). A host handle never changes,
 * so Render writes it; a VRAM offset is written at Patch. Neither relies
 * on dxgkrnl's patch location list: dxgkrnl leaves out a location whose
 * allocation it already patched in that list slot and has not moved since
 * (the slot-id optimisation, made for a GPU whose state keeps the
 * address), and calls no Patch at all when every one is left out, while
 * each DMA buffer here is a fresh copy of the user-mode driver's records.
 * So an allocation keeps where the last Patch saw it (cur_seg, cur_addr),
 * and the registration and the offsets read that at submit time; the
 * fix-up packet comes before the records it patches. */
typedef struct PKT_FIX_E { ULONG kind, add, off; struct D3DPT_ALLOC *al; } PKT_FIX_E;
typedef struct PKT_FIX_T { PKT_HDR h; ULONG n; PKT_FIX_E e[1]; } PKT_FIX_T;

static void pkt_hdr(PKT_HDR *p, ULONG op, ULONG size)
{
    p->magic = D3DPT_PKT_MAGIC;
    p->op = op;
    p->size = size;
}

/* System memory the "GPU" touches: an MDL already in system space as it
 * is, any other mapped here (and unmapped by the caller when *mine). */
static PUCHAR mdl_map(PMDL m, MEMORY_CACHING_TYPE cache, BOOLEAN *mine)
{
    *mine = FALSE;
    if (m->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL)) {
        return (PUCHAR)m->MappedSystemVa;
    }
    *mine = TRUE;
    return (PUCHAR)MmMapLockedPagesSpecifyCache(m, KernelMode, cache, NULL, FALSE, NormalPagePriority);
}

/* A segment address range as the CPU reaches it, or NULL: VRAM through the
 * BAR mapping, the aperture through the pages' mappings when the range's
 * pages are contiguous there (callers ask a row at a time). */
static PUCHAR seg_va(D3DPT_ADAPTER *a, ULONG seg, ULONG addr, ULONG bytes)
{
    if (seg == 1) {
        if (addr > a->seg_size || bytes > a->seg_size - addr) {
            return NULL;
        }
        return a->vram + addr;
    }
    if (seg == 2 && addr >= D3DPT_AP_BASE && bytes) {
        ULONG off = addr - D3DPT_AP_BASE, first, last, i;
        PUCHAR base;

        if (off >= D3DPT_AP_PAGES * PAGE_SIZE || bytes > D3DPT_AP_PAGES * PAGE_SIZE - off) {
            return NULL;
        }
        first = off >> PAGE_SHIFT;
        last = (off + bytes - 1) >> PAGE_SHIFT;
        base = a->ap_va[first];
        if (!base) {
            return NULL;
        }
        for (i = first + 1; i <= last; i++) {
            if (a->ap_va[i] != base + (i - first) * PAGE_SIZE) {
                return NULL;
            }
        }
        return base + (off & (PAGE_SIZE - 1));
    }
    return NULL;
}

/* MAP_APERTURE_SEGMENT: the MDL mapped once, each page's address noted;
 * UNMAP: the mapping undone at the page it started from. */
static void run_map(D3DPT_ADAPTER *a, const PKT_MAP_T *p)
{
    BOOLEAN mine;
    PUCHAR base;
    ULONG i;

    if (p->first >= D3DPT_AP_PAGES || p->n > D3DPT_AP_PAGES - p->first) {
        return;
    }
    base = mdl_map(p->mdl, p->cached ? MmCached : MmWriteCombined, &mine);
    if (!base) {
        dbg_hex("d3dptkmd: aperture map failed, pages ", p->n);
        dbg_puts("\n");
        return;
    }
    if (a->ap_map[p->first].base && a->ap_map[p->first].mine) {
        MmUnmapLockedPages(a->ap_map[p->first].base, a->ap_map[p->first].mdl);
    }
    a->ap_map[p->first].base = base;
    a->ap_map[p->first].mdl = p->mdl;
    a->ap_map[p->first].mine = mine;
    for (i = 0; i < p->n; i++) {
        a->ap_va[p->first + i] = base + (p->mdl_off + i) * PAGE_SIZE;
    }
}

static void run_unmap(D3DPT_ADAPTER *a, const PKT_MAP_T *p)
{
    ULONG i;

    if (p->first >= D3DPT_AP_PAGES || p->n > D3DPT_AP_PAGES - p->first) {
        return;
    }
    for (i = p->first; i < p->first + p->n; i++) {
        if (a->ap_map[i].base && a->ap_map[i].mine) {
            MmUnmapLockedPages(a->ap_map[i].base, a->ap_map[i].mdl);
        }
        a->ap_map[i].base = NULL;
        a->ap_va[i] = NULL;
    }
}

static void run_transfer(D3DPT_ADAPTER *a, const PKT_TRANSFER_T *p)
{
    PUCHAR src, dst, sm = NULL, dm = NULL;
    BOOLEAN smine = FALSE, dmine = FALSE;

    if (p->src.seg == 0) {
        sm = mdl_map(p->src.mdl, MmWriteCombined, &smine);
        src = sm ? sm + p->src.mdl_off : NULL;
    } else {
        src = seg_va(a, p->src.seg, p->src.addr, p->bytes);
    }
    if (p->dst.seg == 0) {
        dm = mdl_map(p->dst.mdl, MmWriteCombined, &dmine);
        dst = dm ? dm + p->dst.mdl_off : NULL;
    } else {
        dst = seg_va(a, p->dst.seg, p->dst.addr, p->bytes);
    }
    if (src && dst) {
        RtlCopyMemory(dst, src, p->bytes);
    } else {
        dbg_hex("d3dptkmd: transfer skipped, segments ", p->src.seg);
        dbg_hex(" -> ", p->dst.seg);
        dbg_puts("\n");
    }
    if (smine && sm) {
        MmUnmapLockedPages(sm, p->src.mdl);
    }
    if (dmine && dm) {
        MmUnmapLockedPages(dm, p->dst.mdl);
    }
}

static void run_fill(D3DPT_ADAPTER *a, const PKT_FILL_T *p)
{
    PULONG d = (PULONG)seg_va(a, p->seg, p->addr, p->bytes);
    ULONG i;

    if (!d) {
        return;
    }
    for (i = 0; i < p->bytes / 4; i++) {
        d[i] = p->pattern;
    }
}

/* Row by row: an aperture surface's rows need not be contiguous in system
 * space (an allocation can be mapped in pieces). */
static void run_blt(D3DPT_ADAPTER *a, const PKT_BLT_T *p)
{
    static ULONG skipped;
    ULONG k;

    for (k = 0; k < p->n; k++) {
        const PKT_RECT *r = &p->r[k];
        ULONG w = (ULONG)(r->r - r->l), hgt = (ULONG)(r->b - r->t), y, x;

        if (r->r <= r->l || r->b <= r->t || r->l < 0 || r->t < 0 ||
            (p->h.op == PKT_BLT && (r->sx < 0 || r->sy < 0))) {
            continue;
        }
        for (y = 0; y < hgt; y++) {
            PUCHAR drow = seg_va(a, p->dst.seg,
                                 p->dst.addr + ((ULONG)r->t + y) * p->dst.pitch + (ULONG)r->l * p->dst.bpp,
                                 w * p->dst.bpp);
            PUCHAR srow = NULL;

            if (p->h.op == PKT_BLT) {
                srow = seg_va(a, p->src.seg,
                              p->src.addr + ((ULONG)r->sy + y) * p->src.pitch + (ULONG)r->sx * p->src.bpp,
                              w * p->src.bpp);
            }
            if (!drow || (p->h.op == PKT_BLT && !srow)) {
                if (skipped < 16) {
                    skipped++;
                    dbg_hex("d3dptkmd: blit row skipped, segments ", p->src.seg);
                    dbg_hex(" -> ", p->dst.seg);
                    dbg_hex(" at ", p->h.op == PKT_BLT && !srow ? p->src.addr : p->dst.addr);
                    dbg_puts("\n");
                }
                break;
            }
            if (p->h.op == PKT_BLT) {
                RtlCopyMemory(drow, srow, w * p->dst.bpp);
            } else if (p->dst.bpp == 4) {
                for (x = 0; x < w; x++) {
                    ((PULONG)drow)[x] = p->color;
                }
            } else if (p->dst.bpp == 2) {
                for (x = 0; x < w; x++) {
                    ((PUSHORT)drow)[x] = (USHORT)p->color;
                }
            } else {
                RtlFillMemory(drow, w, (UCHAR)p->color);
            }
        }
    }
}

/* Tell the host of a D3D allocation at its place in VRAM (the record the
 * XP driver sends for a DirectDraw surface): the levels' offsets made
 * absolute, a volume's {depth, slice pitch} pair left as it is. */
static void d3d_register(D3DPT_ADAPTER *a, D3DPT_ALLOC *al, ULONG addr)
{
    const D3DPT_ALLOC_DESC *d = &al->d;
    d3dpt_vram_surface *s;
    d3dpt_u32x2 *lv;
    ULONG i, nofs = d->nlv;

    if (d->caps & D3DPT_VS_VOLUME) {
        nofs = d->nlv ? d->nlv - 1 : 0;
    }
    s = d3dpt_enc_cmd(&a->enc, D3DPT_OP_VRAM_SURFACE, sizeof(*s), d->nlv * sizeof(*lv));
    if (!s) {
        return;
    }
    s->handle = al->handle;
    s->offset = addr;
    s->width = d->w;
    s->height = d->h;
    s->pitch = d->pitch;
    s->format = d->format;
    s->caps = d->caps;
    s->levels = d->levels;
    lv = (d3dpt_u32x2 *)(s + 1);
    for (i = 0; i < d->nlv; i++) {
        lv[i].a = d->lv[i].a + (i < nofs ? addr : 0);
        lv[i].b = d->lv[i].b;
    }
    al->host_addr = addr;
}

/* The allocations of a user-mode driver's submission, each registered
 * where the host does not know it there yet; the releases queued since
 * the last submission go first. */
static void run_reg(D3DPT_ADAPTER *a, const PKT_REG_T *p)
{
    static ULONG logged;
    KIRQL irql;
    ULONG i;

    KeAcquireSpinLock(&a->rel_lock, &irql);
    for (i = 0; i < a->ctx_rel_n; i++) {
        d3dpt_handle *r = d3dpt_enc_cmd(&a->enc, D3DPT_OP_CTX_DESTROY, sizeof(*r), 0);

        if (r) {
            r->handle = a->ctx_rel[i];
            r->pad = 0;
        }
    }
    a->ctx_rel_n = 0;
    for (i = 0; i < a->rel_n; i++) {
        d3dpt_handle *r = d3dpt_enc_cmd(&a->enc, D3DPT_OP_VRAM_RELEASE, sizeof(*r), 0);

        if (r) {
            r->handle = a->rel[i];
            r->pad = 0;
        }
    }
    a->rel_n = 0;
    KeReleaseSpinLock(&a->rel_lock, irql);
    for (i = 0; i < p->n; i++) {
        const PKT_REG_E *e = &p->e[i];

        if (!e->al || !e->al->handle) {
            continue;
        }
        if (e->al->cur_seg != 1) {
            if (logged++ < 16) {
                dbg_hex("d3dptkmd: a D3D allocation outside VRAM at submit, segment ", e->al->cur_seg);
                dbg_hex(" handle ", e->al->handle);
                dbg_puts("\n");
            }
            continue;
        }
        if (e->al->host_addr != e->al->cur_addr) {
            d3d_register(a, e->al, e->al->cur_addr);
        }
    }
}

/* The user-mode driver's records, as they are: each one's size was checked
 * at Render. A D3DPT_UMD_OP_RETURN before one (../d3dpt_wddm.h) gives it a
 * return slot here (every result record has its ret_off second in its
 * body), runs the batch so far, and copies the result into the user-mode
 * driver's result allocation in VRAM. */
/* Records of the DMA buffer onto the window's batch, as they are (each
 * whole and a multiple of 8 bytes, checked at Render): appended by the
 * device from the buffer's physical address (register set v7), which is
 * the host's copy instead of the vCPU's, else copied here. */
static void d3d_put(D3DPT_ADAPTER *a, const UCHAR *from, ULONG bytes, ULONG count)
{
    ULONG i;

    if (!count) {
        return;
    }
    if (a->dma && a->sub_va && bytes <= D3DPT_CMD_SIZE) {
        PHYSICAL_ADDRESS pa = a->sub_pa;
        ULONG st;

        if (d3dpt_enc_hdr(&a->enc)->cmd_bytes + bytes > D3DPT_CMD_SIZE) {
            d3dpt_enc_flush(&a->enc);
        }
        pa.QuadPart += from - a->sub_va;
        a->regs[D3DPT_FB_REG_DMA_ADDR_LO / 4] = pa.LowPart;
        a->regs[D3DPT_FB_REG_DMA_ADDR_HI / 4] = (ULONG)pa.HighPart;
        a->regs[D3DPT_FB_REG_DMA_BYTES / 4] = bytes;
        a->regs[D3DPT_FB_REG_DMA_APPEND / 4] = count;
        st = a->regs[D3DPT_FB_REG_DMA_APPEND / 4];
        if (st == D3DPT_FB_DMA_OK) {
            return;
        }
        if (a->dma_errors++ < 8) {
            dbg_hex("d3dptkmd: DMA append refused: ", st);
            dbg_hex(" bytes ", bytes);
            dbg_hex(" records ", count);
            dbg_puts("; copied instead\n");
        }
    }
    for (i = 0; i < count; i++) {
        const d3dpt_cmd *c = (const d3dpt_cmd *)from;
        UCHAR *body = (UCHAR *)d3dpt_enc_cmd(&a->enc, c->op, c->size - sizeof(*c), 0);

        if (body) {
            RtlCopyMemory(body, c + 1, c->size - sizeof(*c));
        }
        from += c->size;
    }
}

static void run_d3d(D3DPT_ADAPTER *a, const PKT_D3D_T *p)
{
    const UCHAR *r = (const UCHAR *)(p + 1), *end = (const UCHAR *)p + p->h.size, *run_at = r;
    ULONG i, ret_dst = 0, ret_bytes = 0, run_n = 0;
    BOOLEAN ret = FALSE;

    for (i = 0; i < p->count && r + sizeof(d3dpt_cmd) <= end; i++) {
        d3dpt_cmd *c = (d3dpt_cmd *)r;
        ULONG body_size = c->size - sizeof(*c), slot;

        if (c->op == D3DPT_UMD_OP_RETURN) {
            const d3dpt_u32x2 *u = (const d3dpt_u32x2 *)(c + 1);

            d3d_put(a, run_at, (ULONG)(r - run_at), run_n);
            r += c->size;
            run_at = r;
            run_n = 0;
            ret = body_size >= sizeof(*u) && u->b <= 4096 && u->a + sizeof(d3dpt_ret) + u->b <= a->seg_size;
            ret_dst = ret ? u->a : 0;
            ret_bytes = ret ? u->b : 0;
            continue;
        }
        r += c->size;
        run_n++;
        if (!ret || body_size < 8) {
            ret = FALSE;
            continue;
        }
        /* the record whose result is wanted: its return slot written in
         * the buffer itself, then it alone, then the batch run */
        slot = d3dpt_enc_ret(&a->enc, ret_bytes);
        ((ULONG *)(c + 1))[1] = slot;
        d3d_put(a, run_at, (ULONG)(r - run_at), run_n);
        run_at = r;
        run_n = 0;
        d3dpt_enc_flush(&a->enc);
        RtlCopyMemory(a->vram + ret_dst, d3dpt_enc_result(&a->enc, slot), sizeof(d3dpt_ret) + ret_bytes);
        ret = FALSE;
    }
    d3d_put(a, run_at, (ULONG)(r - run_at), run_n);
}

/* Execute one submission: every packet from start to end, in order. */
static void run(D3DPT_ADAPTER *a, PUCHAR p, ULONG len)
{
    ULONG off = 0;

    while (off + sizeof(PKT_HDR) <= len) {
        const PKT_HDR *hd = (const PKT_HDR *)(p + off);

        if (hd->magic != D3DPT_PKT_MAGIC || hd->size < sizeof(*hd) || hd->size > len - off) {
            dbg_hex("d3dptkmd: bad packet at ", off);
            dbg_hex(" magic ", hd->magic);
            dbg_puts("\n");
            return;
        }
        switch (hd->op) {
        case PKT_TRANSFER:
            run_transfer(a, (const PKT_TRANSFER_T *)hd);
            break;
        case PKT_FILL:
            run_fill(a, (const PKT_FILL_T *)hd);
            break;
        case PKT_BLT:
        case PKT_COLORFILL:
            run_blt(a, (const PKT_BLT_T *)hd);
            break;
        case PKT_FLIP:
            if (((const PKT_FLIP_T *)hd)->src.seg == 1) {
                a->regs[D3DPT_FB_REG_OFFSET / 4] = ((const PKT_FLIP_T *)hd)->src.addr;
                a->scan_addr = ((const PKT_FLIP_T *)hd)->src.addr;
            }
            break;
        case PKT_MAP:
            run_map(a, (const PKT_MAP_T *)hd);
            break;
        case PKT_UNMAP:
            run_unmap(a, (const PKT_MAP_T *)hd);
            break;
        case PKT_REG:
            if (a->d3d) {
                run_reg(a, (const PKT_REG_T *)hd);
            }
            break;
        case PKT_FIX: {             /* the records' VRAM offsets, where their allocations are now */
            const PKT_FIX_T *x = (const PKT_FIX_T *)hd;
            ULONG i;

            for (i = 0; i < x->n; i++) {
                const PKT_FIX_E *e = &x->e[i];

                if (e->kind == D3DPT_PATCH_OFFSET && e->al && e->off + 4 <= len) {
                    *(ULONG *)(p + e->off) = e->al->cur_addr + e->add;
                }
            }
            break;
        }
        case PKT_D3D:
            if (a->d3d) {
                run_d3d(a, (const PKT_D3D_T *)hd);
            }
            break;
        }
        off += hd->size;
    }
    if (a->d3d) {                   /* one doorbell per submission */
        d3dpt_enc_flush(&a->enc);
    }
}

/* ---------------------------------------------------------------- fences */

/*
 * The device raises no interrupt for finished work: a submission is done
 * when SubmitCommand returns. The completion is still reported the way an
 * interrupt service routine would, under the interrupt's lock, and the
 * DPC dxgkrnl expects after it is queued from there.
 */
static KSYNCHRONIZE_ROUTINE notify_completed;
static BOOLEAN notify_completed(PVOID ctx)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n;

    RtlZeroMemory(&n, sizeof(n));
    n.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
    n.DmaCompleted.SubmissionFenceId = (UINT)a->fence_notify;
    a->dxgk.DxgkCbNotifyInterrupt(a->dxgk.DeviceHandle, &n);
    a->dxgk.DxgkCbQueueDpc(a->dxgk.DeviceHandle);
    return TRUE;
}

static KSYNCHRONIZE_ROUTINE notify_preempted;
static BOOLEAN notify_preempted(PVOID ctx)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n;

    RtlZeroMemory(&n, sizeof(n));
    n.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
    n.DmaPreempted.PreemptionFenceId = (UINT)a->preempt_fence;
    n.DmaPreempted.LastCompletedFenceId = (UINT)a->fence_done;
    a->dxgk.DxgkCbNotifyInterrupt(a->dxgk.DeviceHandle, &n);
    a->dxgk.DxgkCbQueueDpc(a->dxgk.DeviceHandle);
    return TRUE;
}

static void complete_fence(D3DPT_ADAPTER *a, ULONG fence)
{
    BOOLEAN ret = FALSE;
    NTSTATUS st;

    a->fence_done = (LONG)fence;
    a->fence_notify = (LONG)fence;
    st = a->dxgk.DxgkCbSynchronizeExecution(a->dxgk.DeviceHandle, notify_completed, a, 0, &ret);
    if (!NT_SUCCESS(st) || !ret) {
        dbg_hex("d3dptkmd: SynchronizeExecution ", (ULONG)st);
        dbg_hex(" ran ", (ULONG)ret);
        dbg_puts("\n");
    }
}

/* --------------------------------------------- building and submitting */

/* Where an allocation is, from everything that moves it or names its
 * place: the paging operations (in the order they will run), every
 * Patch's allocation list, the scanout. The registration with the host
 * reads it at submit time (PKT_REG); no one source sees every allocation
 * (dxgkrnl skips the Patch of a submission whose allocations it already
 * patched, cdd.dll's presents patch the desktop's primary). */
static void alloc_at(HANDLE h, UINT seg, ULONG addr)
{
    D3DPT_ALLOC *al = (D3DPT_ALLOC *)h;

    if (al && al->d.magic == D3DPT_ALLOC_MAGIC) {
        al->cur_seg = seg;
        al->cur_addr = seg ? addr : 0;
    }
}

static void transfer_side(PKT_MEM *m, UINT seg, LARGE_INTEGER addr, MDL *mdl, UINT offset, UINT mdl_pages)
{
    m->seg = seg;
    if (seg == 0) {
        m->addr = 0;
        m->mdl = mdl;
        m->mdl_off = mdl_pages * PAGE_SIZE;
    } else {
        m->addr = addr.LowPart + offset;
        m->mdl = NULL;
        m->mdl_off = 0;
    }
}

/* Paging: moving allocations between the segment and system memory, and
 * filling new ones. Each operation is one packet, run at submit time in
 * the order dxgkrnl queued it. */
static DXGKDDI_BUILDPAGINGBUFFER d3dpt_build_paging_buffer;
static NTSTATUS APIENTRY d3dpt_build_paging_buffer(IN_CONST_HANDLE h, IN_PDXGKARG_BUILDPAGINGBUFFER b)
{
    static ULONG logged;

    UNREFERENCED_PARAMETER(h);
    switch (b->Operation) {
    case DXGK_OPERATION_TRANSFER: {
        PKT_TRANSFER_T *p = (PKT_TRANSFER_T *)b->pDmaBuffer;

        if (b->DmaSize < sizeof(*p)) {
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        pkt_hdr(&p->h, PKT_TRANSFER, sizeof(*p));
        p->bytes = (ULONG)b->Transfer.TransferSize;
        alloc_at(b->Transfer.hAllocation, b->Transfer.Destination.SegmentId,
                 b->Transfer.Destination.SegmentAddress.LowPart);
        transfer_side(&p->src, b->Transfer.Source.SegmentId, b->Transfer.Source.SegmentAddress,
                      b->Transfer.Source.pMdl, b->Transfer.TransferOffset, b->Transfer.MdlOffset);
        transfer_side(&p->dst, b->Transfer.Destination.SegmentId, b->Transfer.Destination.SegmentAddress,
                      b->Transfer.Destination.pMdl, b->Transfer.TransferOffset, b->Transfer.MdlOffset);
        if (logged < 16) {          /* what TransferOffset and MdlOffset hold, the first few times */
            logged++;
            dbg_hex("d3dptkmd: transfer ", p->bytes);
            dbg_hex(" seg ", p->src.seg);
            dbg_hex(" -> ", p->dst.seg);
            dbg_hex(" offset ", b->Transfer.TransferOffset);
            dbg_hex(" mdl pages ", b->Transfer.MdlOffset);
            dbg_puts("\n");
        }
        b->pDmaBuffer = (PUCHAR)b->pDmaBuffer + sizeof(*p);
        break;
    }
    case DXGK_OPERATION_FILL: {
        PKT_FILL_T *p = (PKT_FILL_T *)b->pDmaBuffer;

        if (b->DmaSize < sizeof(*p)) {
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        pkt_hdr(&p->h, PKT_FILL, sizeof(*p));
        alloc_at(b->Fill.hAllocation, b->Fill.Destination.SegmentId, b->Fill.Destination.SegmentAddress.LowPart);
        p->bytes = (ULONG)b->Fill.FillSize;
        p->pattern = b->Fill.FillPattern;
        p->seg = b->Fill.Destination.SegmentId;
        p->addr = b->Fill.Destination.SegmentAddress.LowPart;
        b->pDmaBuffer = (PUCHAR)b->pDmaBuffer + sizeof(*p);
        break;
    }
    case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
    case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT: {
        PKT_MAP_T *p = (PKT_MAP_T *)b->pDmaBuffer;
        BOOLEAN map = b->Operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT;

        if (b->DmaSize < sizeof(*p)) {
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        pkt_hdr(&p->h, map ? PKT_MAP : PKT_UNMAP, sizeof(*p));
        if (map) {
            p->first = (ULONG)b->MapApertureSegment.OffsetInPages;
            p->n = (ULONG)b->MapApertureSegment.NumberOfPages;
            p->mdl = b->MapApertureSegment.pMdl;
            p->mdl_off = b->MapApertureSegment.MdlOffset;
            p->cached = b->MapApertureSegment.Flags.CacheCoherent;
        } else {
            p->first = (ULONG)b->UnmapApertureSegment.OffsetInPages;
            p->n = (ULONG)b->UnmapApertureSegment.NumberOfPages;
            p->mdl = NULL;
            p->mdl_off = 0;
            p->cached = 0;
        }
        b->pDmaBuffer = (PUCHAR)b->pDmaBuffer + sizeof(*p);
        break;
    }
    case DXGK_OPERATION_DISCARD_CONTENT:
        break;
    default:
        dbg_hex("d3dptkmd: BuildPagingBuffer operation ", (ULONG)b->Operation);
        dbg_puts(" ignored\n");
        break;
    }
    return STATUS_SUCCESS;
}

/* A patch location for a surface of a present: dxgkrnl then makes the
 * allocation resident before the submission and calls Patch with where it
 * went (with none, the source never left system memory). Patch walks the
 * packets itself; the offset only has to point inside the buffer. */
static BOOLEAN add_patch(INOUT_PDXGKARG_PRESENT p, const PUCHAR start, const PKT_SURF *s)
{
    D3DDDI_PATCHLOCATIONLIST *pl = p->pPatchLocationListOut;

    if (!pl || p->PatchLocationListOutSize == 0) {
        return FALSE;
    }
    RtlZeroMemory(pl, sizeof(*pl));
    pl->AllocationIndex = s->index;
    pl->PatchOffset = (UINT)((const UCHAR *)&s->seg - start);
    p->pPatchLocationListOut = pl + 1;
    p->PatchLocationListOutSize--;
    return TRUE;
}

static void surf_from_list(PKT_SURF *s, const DXGK_ALLOCATIONLIST *list, ULONG index)
{
    const D3DPT_ALLOC *al = (const D3DPT_ALLOC *)list[index].hDeviceSpecificAllocation;

    s->index = index;
    s->seg = list[index].SegmentId;
    s->addr = list[index].PhysicalAddress.LowPart;
    s->pitch = al ? al->d.pitch : 0;
    s->bpp = al ? al->d.bpp : 4;
}

/*
 * cdd.dll's presents (no DWM yet): blits from the shadow surface GDI drew
 * into to the primary, color fills, and a flip. One packet per call; when
 * the sub-rectangles do not all fit, the rest go in the next DMA buffer
 * (MultipassOffset).
 */
static DXGKDDI_PRESENT d3dpt_present;
static NTSTATUS APIENTRY d3dpt_present(IN_CONST_HANDLE h, INOUT_PDXGKARG_PRESENT p)
{
    static ULONG logged;
    PKT_BLT_T *k = (PKT_BLT_T *)p->pDmaBuffer;
    ULONG room, n, total, i;

    UNREFERENCED_PARAMETER(h);
    if (logged < 8) {
        logged++;
        dbg_hex("d3dptkmd: Present flags=", p->Flags.Value);
        dbg_hex(" subrects ", p->SubRectCnt);
        dbg_puts("\n");
    }
    if (p->Flags.Flip) {
        PKT_FLIP_T *f = (PKT_FLIP_T *)p->pDmaBuffer;

        if (p->DmaSize < sizeof(*f)) {
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        pkt_hdr(&f->h, PKT_FLIP, sizeof(*f));
        surf_from_list(&f->src, p->pAllocationList, DXGK_PRESENT_SOURCE_INDEX);
        if (!add_patch(p, (PUCHAR)f, &f->src)) {
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        p->pDmaBuffer = (PUCHAR)p->pDmaBuffer + sizeof(*f);
        return STATUS_SUCCESS;
    }
    if (!p->Flags.Blt && !p->Flags.ColorFill) {
        return STATUS_NOT_SUPPORTED;
    }
    if (p->DmaSize < sizeof(*k)) {
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    total = p->SubRectCnt ? p->SubRectCnt : 1;
    room = (ULONG)((p->DmaSize - FIELD_OFFSET(PKT_BLT_T, r)) / sizeof(PKT_RECT));
    n = total - p->MultipassOffset;
    if (n > room) {
        n = room;
    }
    if (p->Flags.ColorFill) {
        pkt_hdr(&k->h, PKT_COLORFILL, 0);
        RtlZeroMemory(&k->src, sizeof(k->src));
        k->color = p->Color;
    } else {
        pkt_hdr(&k->h, PKT_BLT, 0);
        surf_from_list(&k->src, p->pAllocationList, DXGK_PRESENT_SOURCE_INDEX);
        k->color = 0;
    }
    surf_from_list(&k->dst, p->pAllocationList, DXGK_PRESENT_DESTINATION_INDEX);
    if (p->PatchLocationListOutSize < 2 ||
        (p->Flags.Blt && !add_patch(p, (PUCHAR)k, &k->src)) || !add_patch(p, (PUCHAR)k, &k->dst)) {
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    if (logged < 8) {
        dbg_hex("d3dptkmd: Present source in segment ", k->src.seg);
        dbg_hex(" at ", k->src.addr);
        dbg_hex(", destination in ", k->dst.seg);
        dbg_puts("\n");
    }
    for (i = 0; i < n; i++) {
        const RECT *d = p->SubRectCnt ? &p->pDstSubRects[p->MultipassOffset + i] : &p->DstRect;
        PKT_RECT *r = &k->r[i];

        r->l = d->left;
        r->t = d->top;
        r->r = d->right;
        r->b = d->bottom;
        r->sx = p->SrcRect.left + (d->left - p->DstRect.left);
        r->sy = p->SrcRect.top + (d->top - p->DstRect.top);
    }
    k->n = n;
    k->h.size = FIELD_OFFSET(PKT_BLT_T, r) + n * sizeof(PKT_RECT);
    p->pDmaBuffer = (PUCHAR)p->pDmaBuffer + k->h.size;
    p->MultipassOffset += n;
    return p->MultipassOffset < total ? STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER : STATUS_SUCCESS;
}

/*
 * The user-mode driver's command buffer (../d3dpt_wddm.h): a registration
 * packet naming every allocation of its list, then the records, copied
 * first and checked in the copy (the command buffer is the process's to
 * change under us), and a fix-up packet with its patch locations. One
 * patch location per allocation has dxgkrnl make it resident and say
 * where. The user-mode driver keeps its buffer small enough that one DMA
 * buffer always takes it.
 */
static DXGKDDI_RENDER d3dpt_render;
static NTSTATUS APIENTRY d3dpt_render(IN_CONST_HANDLE h, INOUT_PDXGKARG_RENDER r)
{
    static ULONG logged;
    PKT_REG_T *reg = (PKT_REG_T *)r->pDmaBuffer;
    PKT_D3D_T *d3d;
    PKT_FIX_T *fix;
    D3DDDI_PATCHLOCATIONLIST *out = r->pPatchLocationListOut;
    ULONG nal = r->AllocationListSize, npl = r->PatchLocationListInSize, reg_size, fix_size, rec_bytes, i, off, count;
    D3DPT_UMD_CMD hdr;
    PUCHAR rec;

    UNREFERENCED_PARAMETER(h);
    if (r->CommandLength < sizeof(hdr) || (r->CommandLength & 7)) {
        dbg_hex("d3dptkmd: Render: command length ", r->CommandLength);
        dbg_puts("\n");
        return STATUS_INVALID_PARAMETER;
    }
    rec_bytes = r->CommandLength - sizeof(hdr);
    reg_size = FIELD_OFFSET(PKT_REG_T, e) + (nal ? nal : 1) * sizeof(PKT_REG_E);
    fix_size = FIELD_OFFSET(PKT_FIX_T, e) + (npl ? npl : 1) * sizeof(PKT_FIX_E);
    if (reg_size + sizeof(PKT_D3D_T) + rec_bytes + fix_size > r->DmaSize || nal > r->PatchLocationListOutSize) {
        dbg_hex("d3dptkmd: Render does not fit one DMA buffer: bytes ", r->CommandLength);
        dbg_hex(" allocations ", nal);
        dbg_hex(" patches ", r->PatchLocationListInSize);
        dbg_puts("\n");
        return STATUS_INVALID_PARAMETER;
    }
    fix = (PKT_FIX_T *)((PUCHAR)r->pDmaBuffer + reg_size);
    d3d = (PKT_D3D_T *)((PUCHAR)fix + fix_size);
    rec = (PUCHAR)(d3d + 1);
    __try {
        RtlCopyMemory(&hdr, r->pCommand, sizeof(hdr));
        RtlCopyMemory(rec, (const UCHAR *)r->pCommand + sizeof(hdr), rec_bytes);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return STATUS_INVALID_PARAMETER;
    }
    if (hdr.magic != D3DPT_UMD_CMD_MAGIC) {
        dbg_hex("d3dptkmd: Render: not a command buffer of ours, magic ", hdr.magic);
        dbg_puts("\n");
        return STATUS_INVALID_PARAMETER;
    }
    /* every record whole, the count the header says */
    for (off = 0, count = 0; off < rec_bytes; count++) {
        const d3dpt_cmd *c = (const d3dpt_cmd *)(rec + off);

        if (rec_bytes - off < sizeof(*c) || c->size < sizeof(*c) || (c->size & 7) || c->size > rec_bytes - off) {
            dbg_hex("d3dptkmd: Render: a broken record at ", off);
            dbg_puts("\n");
            return STATUS_INVALID_PARAMETER;
        }
        off += c->size;
    }
    if (count != hdr.count) {
        dbg_hex("d3dptkmd: Render: records ", count);
        dbg_hex(" where the header says ", hdr.count);
        dbg_puts("\n");
        return STATUS_INVALID_PARAMETER;
    }

    pkt_hdr(&reg->h, PKT_REG, reg_size);
    reg->n = nal;
    for (i = 0; i < nal; i++) {
        PKT_REG_E *e = &reg->e[i];

        e->index = i;
        e->seg = 0;
        e->addr = 0;
        e->al = (D3DPT_ALLOC *)r->pAllocationList[i].hDeviceSpecificAllocation;
        RtlZeroMemory(out, sizeof(*out));
        out->AllocationIndex = i;
        out->PatchOffset = (UINT)((PUCHAR)&e->seg - (PUCHAR)r->pDmaBuffer);
        out++;
    }
    pkt_hdr(&d3d->h, PKT_D3D, sizeof(*d3d) + rec_bytes);
    d3d->count = count;
    pkt_hdr(&fix->h, PKT_FIX, fix_size);
    fix->n = npl;
    for (i = 0; i < npl; i++) {
        D3DDDI_PATCHLOCATIONLIST in = r->pPatchLocationListIn[i];

        if (in.AllocationIndex >= nal || in.PatchOffset < sizeof(hdr) || in.PatchOffset + 4 > r->CommandLength ||
            (in.PatchOffset & 3) || (in.DriverId != D3DPT_PATCH_HANDLE && in.DriverId != D3DPT_PATCH_OFFSET)) {
            dbg_hex("d3dptkmd: Render: a bad patch location, allocation ", in.AllocationIndex);
            dbg_hex(" at ", in.PatchOffset);
            dbg_hex(" kind ", in.DriverId);
            dbg_puts("\n");
            return STATUS_INVALID_PARAMETER;
        }
        fix->e[i].al = (D3DPT_ALLOC *)r->pAllocationList[in.AllocationIndex].hDeviceSpecificAllocation;
        fix->e[i].kind = in.DriverId;
        fix->e[i].add = in.AllocationOffset;
        fix->e[i].off = (ULONG)(rec - (PUCHAR)r->pDmaBuffer) + in.PatchOffset - sizeof(hdr);
        if (in.DriverId == D3DPT_PATCH_HANDLE) {
            *(ULONG *)((PUCHAR)r->pDmaBuffer + fix->e[i].off) = fix->e[i].al ? fix->e[i].al->handle : 0;
        }
    }
    if (logged < 8) {
        logged++;
        dbg_hex("d3dptkmd: Render records ", count);
        dbg_hex(" bytes ", rec_bytes);
        dbg_hex(" allocations ", nal);
        dbg_hex(" patches ", r->PatchLocationListInSize);
        dbg_puts("\n");
    }
    r->pDmaBuffer = rec + rec_bytes;
    r->pPatchLocationListOut = out;
    return STATUS_SUCCESS;
}

/* Where the present's surfaces ended up: every packet of the submission
 * takes its surfaces' addresses from the list again. */
static DXGKDDI_PATCH d3dpt_patch;
static NTSTATUS APIENTRY d3dpt_patch(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_PATCH p)
{
    PUCHAR buf = (PUCHAR)p->pDmaBuffer;
    ULONG off = p->DmaBufferSubmissionStartOffset;
    static ULONG logged;

    UNREFERENCED_PARAMETER(h);
    if (logged < 8) {
        logged++;
        dbg_hex("d3dptkmd: Patch fence ", p->SubmissionFenceId);
        dbg_hex(" patches ", p->PatchLocationListSubmissionLength);
        if (p->AllocationListSize > DXGK_PRESENT_SOURCE_INDEX) {
            dbg_hex(" source segment ", p->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].SegmentId);
            dbg_hex(" at ", p->pAllocationList[DXGK_PRESENT_SOURCE_INDEX].PhysicalAddress.LowPart);
        }
        dbg_puts("\n");
    }
    for (off = 0; off < p->AllocationListSize; off++) {
        alloc_at(p->pAllocationList[off].hDeviceSpecificAllocation, p->pAllocationList[off].SegmentId,
                 p->pAllocationList[off].PhysicalAddress.LowPart);
    }
    off = p->DmaBufferSubmissionStartOffset;
    while (off + sizeof(PKT_HDR) <= p->DmaBufferSubmissionEndOffset) {
        PKT_HDR *hd = (PKT_HDR *)(buf + off);

        if (hd->magic != D3DPT_PKT_MAGIC || hd->size < sizeof(*hd)) {
            break;
        }
        if (hd->op == PKT_BLT || hd->op == PKT_COLORFILL) {
            PKT_BLT_T *k = (PKT_BLT_T *)hd;

            if (hd->op == PKT_BLT && k->src.index < p->AllocationListSize) {
                k->src.seg = p->pAllocationList[k->src.index].SegmentId;
                k->src.addr = p->pAllocationList[k->src.index].PhysicalAddress.LowPart;
            }
            if (k->dst.index < p->AllocationListSize) {
                k->dst.seg = p->pAllocationList[k->dst.index].SegmentId;
                k->dst.addr = p->pAllocationList[k->dst.index].PhysicalAddress.LowPart;
            }
        } else if (hd->op == PKT_FLIP) {
            PKT_FLIP_T *f = (PKT_FLIP_T *)hd;

            if (f->src.index < p->AllocationListSize) {
                f->src.seg = p->pAllocationList[f->src.index].SegmentId;
                f->src.addr = p->pAllocationList[f->src.index].PhysicalAddress.LowPart;
            }
        } else if (hd->op == PKT_REG) {
            PKT_REG_T *g = (PKT_REG_T *)hd;
            ULONG i;

            for (i = 0; i < g->n; i++) {
                PKT_REG_E *e = &g->e[i];

                if (e->index < p->AllocationListSize) {
                    const DXGK_ALLOCATIONLIST *l = &p->pAllocationList[e->index];

                    e->seg = l->SegmentId;
                    e->addr = l->PhysicalAddress.LowPart;
                    e->al = (D3DPT_ALLOC *)l->hDeviceSpecificAllocation;
                    if (e->al) {
                        e->al->cur_seg = e->seg;
                        e->al->cur_addr = e->addr;
                    }
                }
            }
        }
        off += hd->size;
    }
    return STATUS_SUCCESS;
}

/* The DMA buffer is in contiguous system memory (segment set 0), so its
 * physical address gives it back in system space. Run it, then report the
 * fence done: through the device's DMA interrupt (register set v7, the
 * FENCE register), else as an interrupt would, from here. */
static DXGKDDI_SUBMITCOMMAND d3dpt_submit_command;
static NTSTATUS APIENTRY d3dpt_submit_command(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_SUBMITCOMMAND s)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;
    static ULONG logged;

    if (logged < 8) {
        logged++;
        dbg_hex("d3dptkmd: SubmitCommand fence ", s->SubmissionFenceId);
        dbg_hex(" flags ", s->Flags.Value);
        dbg_hex(" bytes ", s->DmaBufferSubmissionEndOffset - s->DmaBufferSubmissionStartOffset);
        dbg_puts("\n");
    }
    if (s->DmaBufferSubmissionEndOffset > s->DmaBufferSubmissionStartOffset) {
        PHYSICAL_ADDRESS pa = s->DmaBufferPhysicalAddress;
        PUCHAR va;

        pa.QuadPart += s->DmaBufferSubmissionStartOffset;
        va = s->DmaBufferSegmentId == 0 ? (PUCHAR)MmGetVirtualForPhysical(pa) : NULL;
        if (va) {
            a->sub_va = va;
            a->sub_pa = pa;
            run(a, va, s->DmaBufferSubmissionEndOffset - s->DmaBufferSubmissionStartOffset);
            a->sub_va = NULL;
        } else {
            dbg_hex("d3dptkmd: SubmitCommand: no system address for the DMA buffer, segment ",
                    s->DmaBufferSegmentId);
            dbg_puts("\n");
        }
    }
    if (a->fence_irq) {
        a->fence_done = (LONG)s->SubmissionFenceId;
        a->regs[D3DPT_FB_REG_FENCE / 4] = s->SubmissionFenceId;   /* IRQ_DMA: the ISR reports it */
    } else {
        complete_fence(a, s->SubmissionFenceId);
    }
    return STATUS_SUCCESS;
}

/* Nothing is ever in flight, so a preemption finds the queue empty: it is
 * answered at once with the last fence done. */
static DXGKDDI_PREEMPTCOMMAND d3dpt_preempt_command;
static NTSTATUS APIENTRY d3dpt_preempt_command(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_PREEMPTCOMMAND p)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;
    BOOLEAN ret;

    a->preempt_fence = (LONG)p->PreemptionFenceId;
    a->dxgk.DxgkCbSynchronizeExecution(a->dxgk.DeviceHandle, notify_preempted, a, 0, &ret);
    return STATUS_SUCCESS;
}

static DXGKDDI_QUERYCURRENTFENCE d3dpt_query_current_fence;
static NTSTATUS APIENTRY d3dpt_query_current_fence(IN_CONST_HANDLE h, INOUT_PDXGKARG_QUERYCURRENTFENCE q)
{
    q->CurrentFence = (UINT)((D3DPT_ADAPTER *)h)->fence_done;
    return STATUS_SUCCESS;
}

static DXGKDDI_RESETFROMTIMEOUT d3dpt_reset_from_timeout;
static NTSTATUS APIENTRY d3dpt_reset_from_timeout(IN_CONST_HANDLE h)
{
    UNREFERENCED_PARAMETER(h);
    dbg_line("ResetFromTimeout");
    return STATUS_SUCCESS;
}

static DXGKDDI_RESTARTFROMTIMEOUT d3dpt_restart_from_timeout;
static NTSTATUS APIENTRY d3dpt_restart_from_timeout(IN_CONST_HANDLE h)
{
    UNREFERENCED_PARAMETER(h);
    dbg_line("RestartFromTimeout");
    return STATUS_SUCCESS;
}

static DXGKDDI_COLLECTDBGINFO d3dpt_collect_dbg_info;
static NTSTATUS APIENTRY d3dpt_collect_dbg_info(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_COLLECTDBGINFO c)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(c);
    dbg_line("CollectDbgInfo");
    return STATUS_SUCCESS;
}
/* ------------------------------------------------------------- VidPN */

/* A mode as the video signal a target and the monitor see: progressive,
 * no blanking (the player shows the active area as it is). */
static void signal_info(D3DKMDT_VIDEO_SIGNAL_INFO *s, ULONG w, ULONG h, ULONG hz)
{
    s->VideoStandard = D3DKMDT_VSS_OTHER;
    s->TotalSize.cx = w;
    s->TotalSize.cy = h;
    s->ActiveSize = s->TotalSize;
    s->VSyncFreq.Numerator = hz;
    s->VSyncFreq.Denominator = 1;
    s->HSyncFreq.Numerator = hz * h;
    s->HSyncFreq.Denominator = 1;
    s->PixelRate = (SIZE_T)w * h * hz;
    s->ScanLineOrdering = D3DDDI_VSSLO_PROGRESSIVE;
}

/* the mode the desktop starts in: 1024x768 where the host offers it */
static BOOLEAN preferred(const D3DPT_ADAPTER *a, ULONG i)
{
    ULONG k;

    for (k = 0; k < a->nmodes; k++) {
        if (a->modes[k].w == 1024 && a->modes[k].h == 768) {
            return k == i;
        }
    }
    return i == 0;
}

static DXGKDDI_RECOMMENDMONITORMODES d3dpt_recommend_monitor_modes;
static NTSTATUS APIENTRY d3dpt_recommend_monitor_modes(IN_CONST_HANDLE h,
                                                       IN_CONST_PDXGKARG_RECOMMENDMONITORMODES_CONST r)
{
    const D3DPT_ADAPTER *a = (const D3DPT_ADAPTER *)h;
    const DXGK_MONITORSOURCEMODESET_INTERFACE *mi = r->pMonitorSourceModeSetInterface;
    ULONG i, added = 0;

    for (i = 0; i < a->nmodes; i++) {
        D3DKMDT_MONITOR_SOURCE_MODE *m;

        if (!NT_SUCCESS(mi->pfnCreateNewModeInfo(r->hMonitorSourceModeSet, &m))) {
            break;
        }
        signal_info(&m->VideoSignalInfo, a->modes[i].w, a->modes[i].h, a->modes[i].hz);
        m->ColorBasis = D3DKMDT_CB_SRGB;
        m->ColorCoeffDynamicRanges.FirstChannel = 8;
        m->ColorCoeffDynamicRanges.SecondChannel = 8;
        m->ColorCoeffDynamicRanges.ThirdChannel = 8;
        m->ColorCoeffDynamicRanges.FourthChannel = 8;
        m->Origin = D3DKMDT_MCO_DRIVER;
        m->Preference = preferred(a, i) ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        if (NT_SUCCESS(mi->pfnAddMode(r->hMonitorSourceModeSet, m))) {
            added++;
        } else {                                /* a mode the set has already */
            mi->pfnReleaseModeInfo(r->hMonitorSourceModeSet, m);
        }
    }
    dbg_hex("d3dptkmd: RecommendMonitorModes added ", added);
    dbg_puts("\n");
    return STATUS_SUCCESS;
}

/* Every mode of the table on a source, or only the one whose size the
 * target has pinned. */
static NTSTATUS fill_source_modes(const D3DPT_ADAPTER *a, const DXGK_VIDPN_INTERFACE *vi,
                                  D3DKMDT_HVIDPN hvidpn, D3DDDI_VIDEO_PRESENT_SOURCE_ID src,
                                  const D3DKMDT_VIDPN_TARGET_MODE *pinned_target)
{
    D3DKMDT_HVIDPNSOURCEMODESET hset;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *si;
    NTSTATUS st;
    ULONG i;

    st = vi->pfnCreateNewSourceModeSet(hvidpn, src, &hset, &si);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    for (i = 0; i < a->nmodes; i++) {
        D3DKMDT_VIDPN_SOURCE_MODE *m;
        ULONG w = a->modes[i].w, ht = a->modes[i].h;

        if (pinned_target && (pinned_target->VideoSignalInfo.ActiveSize.cx != w ||
                              pinned_target->VideoSignalInfo.ActiveSize.cy != ht)) {
            continue;
        }
        if (!NT_SUCCESS(si->pfnCreateNewModeInfo(hset, &m))) {
            break;
        }
        m->Type = D3DKMDT_RMT_GRAPHICS;
        m->Format.Graphics.PrimSurfSize.cx = w;
        m->Format.Graphics.PrimSurfSize.cy = ht;
        m->Format.Graphics.VisibleRegionSize = m->Format.Graphics.PrimSurfSize;
        m->Format.Graphics.Stride = w * 4;
        /* A8R8G8B8, the format cdd.dll creates the primary in (with X8R8G8B8
         * here the primary was refused and the desktop never shown) */
        m->Format.Graphics.PixelFormat = D3DDDIFMT_A8R8G8B8;
        m->Format.Graphics.ColorBasis = D3DKMDT_CB_SRGB;
        m->Format.Graphics.PixelValueAccessMode = D3DKMDT_PVAM_DIRECT;
        if (!NT_SUCCESS(si->pfnAddMode(hset, m))) {   /* two refresh rates, one size */
            si->pfnReleaseModeInfo(hset, m);
        }
    }
    st = vi->pfnAssignSourceModeSet(hvidpn, src, hset);
    if (!NT_SUCCESS(st)) {
        vi->pfnReleaseSourceModeSet(hvidpn, hset);
    }
    return st;
}

/* Every mode on a target, or only the pinned source's size. */
static NTSTATUS fill_target_modes(const D3DPT_ADAPTER *a, const DXGK_VIDPN_INTERFACE *vi,
                                  D3DKMDT_HVIDPN hvidpn, D3DDDI_VIDEO_PRESENT_TARGET_ID tgt,
                                  const D3DKMDT_VIDPN_SOURCE_MODE *pinned_source)
{
    D3DKMDT_HVIDPNTARGETMODESET hset;
    const DXGK_VIDPNTARGETMODESET_INTERFACE *ti;
    NTSTATUS st;
    ULONG i;

    st = vi->pfnCreateNewTargetModeSet(hvidpn, tgt, &hset, &ti);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    for (i = 0; i < a->nmodes; i++) {
        D3DKMDT_VIDPN_TARGET_MODE *m;

        if (pinned_source && (pinned_source->Format.Graphics.PrimSurfSize.cx != a->modes[i].w ||
                              pinned_source->Format.Graphics.PrimSurfSize.cy != a->modes[i].h)) {
            continue;
        }
        if (!NT_SUCCESS(ti->pfnCreateNewModeInfo(hset, &m))) {
            break;
        }
        signal_info(&m->VideoSignalInfo, a->modes[i].w, a->modes[i].h, a->modes[i].hz);
        m->Preference = preferred(a, i) ? D3DKMDT_MP_PREFERRED : D3DKMDT_MP_NOTPREFERRED;
        if (!NT_SUCCESS(ti->pfnAddMode(hset, m))) {
            ti->pfnReleaseModeInfo(hset, m);
        }
    }
    st = vi->pfnAssignTargetModeSet(hvidpn, tgt, hset);
    if (!NT_SUCCESS(st)) {
        vi->pfnReleaseTargetModeSet(hvidpn, hset);
    }
    return st;
}

/*
 * The modes each path can take given what is pinned already. For every
 * path: a source mode set unless the source's mode is pinned (or the
 * source is the pivot dxgkrnl is enumerating around), the same for the
 * target, and the path's scaling and rotation, identity only, unless
 * already pinned. One source and one target, so no cross-path rules.
 */
static DXGKDDI_ENUMVIDPNCOFUNCMODALITY d3dpt_enum_vidpn_cofunc_modality;
static NTSTATUS APIENTRY d3dpt_enum_vidpn_cofunc_modality(IN_CONST_HANDLE h,
                                                          IN_CONST_PDXGKARG_ENUMVIDPNCOFUNCMODALITY_CONST e)
{
    const D3DPT_ADAPTER *a = (const D3DPT_ADAPTER *)h;
    const DXGK_VIDPN_INTERFACE *vi;
    const DXGK_VIDPNTOPOLOGY_INTERFACE *topi;
    D3DKMDT_HVIDPNTOPOLOGY top;
    const D3DKMDT_VIDPN_PRESENT_PATH *path, *next;
    NTSTATUS st;

    dbg_hex("d3dptkmd: EnumVidPnCofuncModality pivot=", (ULONG)e->EnumPivotType);
    dbg_puts("\n");
    st = a->dxgk.DxgkCbQueryVidPnInterface(e->hConstrainingVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = vi->pfnGetTopology(e->hConstrainingVidPn, &top, &topi);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = topi->pfnAcquireFirstPathInfo(top, &path);
    while (st == STATUS_SUCCESS) {
        D3DKMDT_HVIDPNSOURCEMODESET hsrc;
        D3DKMDT_HVIDPNTARGETMODESET htgt;
        const DXGK_VIDPNSOURCEMODESET_INTERFACE *si;
        const DXGK_VIDPNTARGETMODESET_INTERFACE *ti;
        const D3DKMDT_VIDPN_SOURCE_MODE *psrc = NULL;
        const D3DKMDT_VIDPN_TARGET_MODE *ptgt = NULL;
        D3DKMDT_VIDPN_SOURCE_MODE src_pin;
        D3DKMDT_VIDPN_TARGET_MODE tgt_pin;
        BOOLEAN has_src = FALSE, has_tgt = FALSE;
        BOOLEAN src_pivot = e->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE &&
                            e->EnumPivot.VidPnSourceId == path->VidPnSourceId;
        BOOLEAN tgt_pivot = e->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET &&
                            e->EnumPivot.VidPnTargetId == path->VidPnTargetId;

        /* What is pinned on either end, copied out: a mode set still
         * acquired cannot be replaced, so every set is released before
         * the new ones are assigned. */
        if (NT_SUCCESS(vi->pfnAcquireSourceModeSet(e->hConstrainingVidPn, path->VidPnSourceId, &hsrc, &si))) {
            if (NT_SUCCESS(si->pfnAcquirePinnedModeInfo(hsrc, &psrc)) && psrc) {
                src_pin = *psrc;
                has_src = TRUE;
                si->pfnReleaseModeInfo(hsrc, psrc);
            }
            vi->pfnReleaseSourceModeSet(e->hConstrainingVidPn, hsrc);
        }
        if (NT_SUCCESS(vi->pfnAcquireTargetModeSet(e->hConstrainingVidPn, path->VidPnTargetId, &htgt, &ti))) {
            if (NT_SUCCESS(ti->pfnAcquirePinnedModeInfo(htgt, &ptgt)) && ptgt) {
                tgt_pin = *ptgt;
                has_tgt = TRUE;
                ti->pfnReleaseModeInfo(htgt, ptgt);
            }
            vi->pfnReleaseTargetModeSet(e->hConstrainingVidPn, htgt);
        }

        if (!has_src && !src_pivot) {
            NTSTATUS fs = fill_source_modes(a, vi, e->hConstrainingVidPn, path->VidPnSourceId,
                                            has_tgt ? &tgt_pin : NULL);
            if (!NT_SUCCESS(fs)) {
                dbg_hex("d3dptkmd: cofunc: source modes ", (ULONG)fs);
                dbg_puts("\n");
            }
        }
        if (!has_tgt && !tgt_pivot) {
            NTSTATUS ft = fill_target_modes(a, vi, e->hConstrainingVidPn, path->VidPnTargetId,
                                            has_src ? &src_pin : NULL);
            if (!NT_SUCCESS(ft)) {
                dbg_hex("d3dptkmd: cofunc: target modes ", (ULONG)ft);
                dbg_puts("\n");
            }
        }

        /* identity scaling and rotation, where not pinned yet */
        if (e->EnumPivotType != D3DKMDT_EPT_SCALING && e->EnumPivotType != D3DKMDT_EPT_ROTATION &&
            (path->ContentTransformation.Scaling == D3DKMDT_VPPS_UNPINNED ||
             path->ContentTransformation.Rotation == D3DKMDT_VPPR_UNPINNED)) {
            D3DKMDT_VIDPN_PRESENT_PATH upd = *path;

            RtlZeroMemory(&upd.ContentTransformation.ScalingSupport,
                          sizeof(upd.ContentTransformation.ScalingSupport));
            upd.ContentTransformation.ScalingSupport.Identity = 1;
            RtlZeroMemory(&upd.ContentTransformation.RotationSupport,
                          sizeof(upd.ContentTransformation.RotationSupport));
            upd.ContentTransformation.RotationSupport.Identity = 1;
            topi->pfnUpdatePathSupportInfo(top, &upd);
        }

        st = topi->pfnAcquireNextPathInfo(top, path, &next);
        topi->pfnReleasePathInfo(top, path);
        path = next;
    }
    if (st != STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET) {
        dbg_hex("d3dptkmd: EnumVidPnCofuncModality -> ", (ULONG)st);
        dbg_puts("\n");
        return st;
    }
    return STATUS_SUCCESS;
}

/* One source, one target, any of the table's modes: whatever dxgkrnl
 * proposes from what EnumVidPnCofuncModality offered is supported. */
static DXGKDDI_ISSUPPORTEDVIDPN d3dpt_is_supported_vidpn;
static NTSTATUS APIENTRY d3dpt_is_supported_vidpn(IN_CONST_HANDLE h, INOUT_PDXGKARG_ISSUPPORTEDVIDPN s)
{
    UNREFERENCED_PARAMETER(h);
    s->IsVidPnSupported = TRUE;
    dbg_line("IsSupportedVidPn: yes");
    return STATUS_SUCCESS;
}

static DXGKDDI_RECOMMENDFUNCTIONALVIDPN d3dpt_recommend_functional_vidpn;
static NTSTATUS APIENTRY d3dpt_recommend_functional_vidpn(IN_CONST_HANDLE h,
                                                          IN_CONST_PDXGKARG_RECOMMENDFUNCTIONALVIDPN_CONST r)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(r);
    dbg_line("RecommendFunctionalVidPn: none");
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

static DXGKDDI_RECOMMENDVIDPNTOPOLOGY d3dpt_recommend_vidpn_topology;
static NTSTATUS APIENTRY d3dpt_recommend_vidpn_topology(IN_CONST_HANDLE h,
                                                        IN_CONST_PDXGKARG_RECOMMENDVIDPNTOPOLOGY_CONST r)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(r);
    dbg_line("RecommendVidPnTopology: none");
    return STATUS_GRAPHICS_NO_RECOMMENDED_VIDPN_TOPOLOGY;
}

/* The mode the functional VidPN pinned on the source: the scanout's size
 * and pitch, as the XP driver's mode switch programs them. ENABLE waits
 * for SetVidPnSourceVisibility, OFFSET for SetVidPnSourceAddress. */
static DXGKDDI_COMMITVIDPN d3dpt_commit_vidpn;
static NTSTATUS APIENTRY d3dpt_commit_vidpn(IN_CONST_HANDLE h, IN_CONST_PDXGKARG_COMMITVIDPN_CONST c)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;
    const DXGK_VIDPN_INTERFACE *vi;
    D3DKMDT_HVIDPNSOURCEMODESET hsrc;
    const DXGK_VIDPNSOURCEMODESET_INTERFACE *si;
    const D3DKMDT_VIDPN_SOURCE_MODE *m = NULL;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID src = c->AffectedVidPnSourceId == D3DDDI_ID_ALL ? 0 : c->AffectedVidPnSourceId;
    NTSTATUS st;

    st = a->dxgk.DxgkCbQueryVidPnInterface(c->hFunctionalVidPn, DXGK_VIDPN_INTERFACE_VERSION_V1, &vi);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    st = vi->pfnAcquireSourceModeSet(c->hFunctionalVidPn, src, &hsrc, &si);
    if (!NT_SUCCESS(st)) {
        return st;
    }
    si->pfnAcquirePinnedModeInfo(hsrc, &m);
    if (m && m->Type == D3DKMDT_RMT_GRAPHICS) {
        ULONG w = m->Format.Graphics.PrimSurfSize.cx, ht = m->Format.Graphics.PrimSurfSize.cy, i;

        a->cur_w = w;
        a->cur_h = ht;
        a->cur_pitch = m->Format.Graphics.Stride;
        a->cur_hz = 60;
        for (i = 0; i < a->nmodes; i++) {
            if (a->modes[i].w == w && a->modes[i].h == ht) {
                a->cur_hz = a->modes[i].hz;
                break;
            }
        }
        a->regs[D3DPT_FB_REG_WIDTH / 4] = w;
        a->regs[D3DPT_FB_REG_HEIGHT / 4] = ht;
        a->regs[D3DPT_FB_REG_BPP / 4] = 32;
        a->regs[D3DPT_FB_REG_PITCH / 4] = a->cur_pitch;
        a->regs[D3DPT_FB_REG_HZ / 4] = a->cur_hz;
        dbg_hex("d3dptkmd: CommitVidPn ", w);
        dbg_hex(" x ", ht);
        dbg_hex(" pitch ", a->cur_pitch);
        dbg_puts("\n");
        si->pfnReleaseModeInfo(hsrc, m);
    } else {
        dbg_line("CommitVidPn: no pinned source mode (the source is off)");
    }
    vi->pfnReleaseSourceModeSet(c->hFunctionalVidPn, hsrc);
    return STATUS_SUCCESS;
}

/* The primary's address in segment 1 (VRAM) is the scanout's offset. */
static DXGKDDI_SETVIDPNSOURCEADDRESS d3dpt_set_vidpn_source_address;
static NTSTATUS APIENTRY d3dpt_set_vidpn_source_address(IN_CONST_HANDLE h,
                                                        IN_CONST_PDXGKARG_SETVIDPNSOURCEADDRESS s)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;

    alloc_at(s->hAllocation, s->PrimarySegment, s->PrimaryAddress.LowPart);
    if (s->PrimarySegment == 1) {
        a->regs[D3DPT_FB_REG_OFFSET / 4] = s->PrimaryAddress.LowPart;
        a->scan_addr = s->PrimaryAddress.LowPart;
    }
    dbg_hex("d3dptkmd: SetVidPnSourceAddress segment ", s->PrimarySegment);
    dbg_hex(" at ", s->PrimaryAddress.LowPart);
    dbg_puts("\n");
    return STATUS_SUCCESS;
}

static DXGKDDI_SETVIDPNSOURCEVISIBILITY d3dpt_set_vidpn_source_visibility;
static NTSTATUS APIENTRY d3dpt_set_vidpn_source_visibility(IN_CONST_HANDLE h,
                                                           IN_CONST_PDXGKARG_SETVIDPNSOURCEVISIBILITY v)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;

    a->regs[D3DPT_FB_REG_ENABLE / 4] = v->Visible ? 1 : 0;
    dbg_hex("d3dptkmd: SetVidPnSourceVisibility ", (ULONG)v->Visible);
    dbg_puts("\n");
    return STATUS_SUCCESS;
}

/* The path's gamma ramp: D3DKMTSetGammaRamp (Direct3D 9's SetGammaRamp,
 * DXGI's SetGammaControl, GDI's SetDeviceGammaRamp) into the device's
 * GAMMA block (register set v5), as the XP driver's DrvIcmSetDeviceGammaRamp
 * loads it. The user-mode driver claims D3DCAPS2_FULLSCREENGAMMA on it,
 * which d3d10level9 requires for any feature level (and so DWM). */
static DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH d3dpt_update_active_vidpn_present_path;
static NTSTATUS APIENTRY d3dpt_update_active_vidpn_present_path(IN_CONST_HANDLE h,
    IN_CONST_PDXGKARG_UPDATEACTIVEVIDPNPRESENTPATH_CONST u)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;
    const D3DKMDT_GAMMA_RAMP *g = &u->VidPnPresentPathInfo.GammaRamp;
    static ULONG logged;
    ULONG i;

    if (!(a->regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_GAMMA)) {
        return STATUS_SUCCESS;
    }
    if (g->Type == D3DDDI_GAMMARAMP_RGB256x3x16 && g->Data.pRgb256x3x16 &&
        g->DataSize >= sizeof(D3DDDI_GAMMA_RAMP_RGB256x3x16)) {
        const D3DDDI_GAMMA_RAMP_RGB256x3x16 *r = g->Data.pRgb256x3x16;

        for (i = 0; i < D3DPT_FB_GAMMA_SIZE; i++) {
            a->regs[D3DPT_FB_REG_GAMMA / 4 + i] = ((ULONG)(r->Red[i] >> 8) << 16) |
                                                   ((ULONG)(r->Green[i] >> 8) << 8) | (r->Blue[i] >> 8);
        }
        a->regs[D3DPT_FB_REG_GAMMA_ENABLE / 4] = 1;
    } else if (g->Type == D3DDDI_GAMMARAMP_DEFAULT) {
        a->regs[D3DPT_FB_REG_GAMMA_ENABLE / 4] = 0;
    } else if (g->Type != D3DDDI_GAMMARAMP_UNINITIALIZED) {
        dbg_hex("d3dptkmd: gamma ramp type not loaded ", (ULONG)g->Type);
        dbg_puts("\n");
        return STATUS_SUCCESS;
    }
    if (logged < 8) {
        logged++;
        dbg_hex("d3dptkmd: gamma ramp type ", (ULONG)g->Type);
        dbg_hex(", 128 -> ", a->regs[D3DPT_FB_REG_GAMMA / 4 + 128]);
        dbg_puts("\n");
    }
    return STATUS_SUCCESS;
}

/* Where the scanout is, from the device's vertical-blank counter clock
 * (FRAMES is whole frames; inside one, nothing finer yet). */
static DXGKDDI_GETSCANLINE d3dpt_get_scan_line;
static NTSTATUS APIENTRY d3dpt_get_scan_line(IN_CONST_HANDLE h, INOUT_PDXGKARG_GETSCANLINE g)
{
    UNREFERENCED_PARAMETER(h);
    g->InVerticalBlank = FALSE;
    g->ScanLine = 0;
    return STATUS_SUCCESS;
}
STUB2(DXGKDDI_STOPCAPTURE, d3dpt_stop_capture, IN_CONST_HANDLE, IN_CONST_PDXGKARG_STOPCAPTURE)
STUB2(DXGKDDI_CREATEOVERLAY, d3dpt_create_overlay, IN_CONST_HANDLE, INOUT_PDXGKARG_CREATEOVERLAY)
/* a user-mode driver's command buffers: none exists yet (plan step 6) */
STUB2(DXGKDDI_UPDATEOVERLAY, d3dpt_update_overlay, IN_CONST_HANDLE, IN_CONST_PDXGKARG_UPDATEOVERLAY)
STUB2(DXGKDDI_FLIPOVERLAY, d3dpt_flip_overlay, IN_CONST_HANDLE, IN_CONST_PDXGKARG_FLIPOVERLAY)
STUB1(DXGKDDI_DESTROYOVERLAY, d3dpt_destroy_overlay, IN_CONST_HANDLE)
STUB2(DXGKDDI_SETDISPLAYPRIVATEDRIVERFORMAT, d3dpt_set_display_private_driver_format, IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETDISPLAYPRIVATEDRIVERFORMAT)
STUB2(DXGKDDI_QUERYVIDPNHWCAPABILITY, d3dpt_query_vidpn_hw_capability, IN_CONST_HANDLE, INOUT_PDXGKARG_QUERYVIDPNHWCAPABILITY)

/* ------------------------------------------------------ vertical blank */

static KSYNCHRONIZE_ROUTINE notify_vsync;
static BOOLEAN notify_vsync(PVOID ctx)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n;

    RtlZeroMemory(&n, sizeof(n));
    n.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    n.CrtcVsync.VidPnTargetId = 0;
    n.CrtcVsync.PhysicalAddress.LowPart = a->scan_addr;
    a->dxgk.DxgkCbNotifyInterrupt(a->dxgk.DeviceHandle, &n);
    a->dxgk.DxgkCbQueueDpc(a->dxgk.DeviceHandle);
    return TRUE;
}

/* Every refresh while enabled: what the scanout shows now, which also
 * retires a flip queued on the vertical blank. */
static KDEFERRED_ROUTINE vsync_tick;
static VOID vsync_tick(PKDPC dpc, PVOID ctx, PVOID a1, PVOID a2)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)ctx;
    BOOLEAN ret;

    UNREFERENCED_PARAMETER(dpc);
    UNREFERENCED_PARAMETER(a1);
    UNREFERENCED_PARAMETER(a2);
    if (a->vsync_on && a->regs) {
        a->dxgk.DxgkCbSynchronizeExecution(a->dxgk.DeviceHandle, notify_vsync, a, 0, &ret);
    }
}

static void vsync_stop(D3DPT_ADAPTER *a)
{
    InterlockedExchange(&a->vsync_on, 0);
    if (a->vsync_irq && a->regs) {
        a->irq_mask &= ~D3DPT_FB_IRQ_VBLANK;
        a->regs[D3DPT_FB_REG_IRQ_ENABLE / 4] = a->irq_mask;   /* a pending vertical blank goes with it */
    }
    KeCancelTimer(&a->vsync_timer);
}

/* CONTROLINTERRUPT takes two values, not a structure. The vertical blank
 * is the only interrupt dxgkrnl switches: the device's own at the refresh
 * HZ says (CommitVidPn writes it), or a timer at the committed mode's
 * refresh, which the system clock's granularity (~15.6 ms) rounds. */
static DXGKDDI_CONTROLINTERRUPT d3dpt_control_interrupt;
static NTSTATUS APIENTRY d3dpt_control_interrupt(IN_CONST_HANDLE h,
                                                 IN_CONST_DXGK_INTERRUPT_TYPE type,
                                                 IN_BOOLEAN enable)
{
    D3DPT_ADAPTER *a = (D3DPT_ADAPTER *)h;
    LARGE_INTEGER due;
    ULONG hz = a->cur_hz ? a->cur_hz : 60, period_ms = 1000 / hz;

    dbg_hex("d3dptkmd: ControlInterrupt type=", (ULONG)type);
    dbg_hex(" enable=", (ULONG)enable);
    dbg_puts("\n");
    if (type != DXGK_INTERRUPT_CRTC_VSYNC) {
        return STATUS_NOT_SUPPORTED;
    }
    if (!enable) {
        if (a->vsync_irq) {
            dbg_hex("d3dptkmd: vertical blank interrupts taken: ", a->vsync_isr);
            dbg_puts("\n");
        }
        vsync_stop(a);
        return STATUS_SUCCESS;
    }
    if (a->vsync_irq) {
        InterlockedExchange(&a->vsync_on, 1);
        a->irq_mask |= D3DPT_FB_IRQ_VBLANK;
        a->regs[D3DPT_FB_REG_IRQ_ENABLE / 4] = a->irq_mask;
        return STATUS_SUCCESS;
    }
    if (!InterlockedExchange(&a->vsync_on, 1)) {
        due.QuadPart = -(LONGLONG)period_ms * 10000;
        KeSetTimerEx(&a->vsync_timer, due, period_ms ? period_ms : 1, &a->vsync_dpc);
    }
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------- entry */

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    DRIVER_INITIALIZATION_DATA init;
    NTSTATUS st;

    RtlZeroMemory(&init, sizeof(init));
    init.Version = DXGKDDI_INTERFACE_VERSION_WIN7;

    init.DxgkDdiAddDevice = d3dpt_add_device;
    init.DxgkDdiStartDevice = d3dpt_start_device;
    init.DxgkDdiStopDevice = d3dpt_stop_device;
    init.DxgkDdiRemoveDevice = d3dpt_remove_device;
    init.DxgkDdiDispatchIoRequest = d3dpt_dispatch_io_request;
    init.DxgkDdiInterruptRoutine = d3dpt_interrupt;
    init.DxgkDdiDpcRoutine = d3dpt_dpc;
    init.DxgkDdiQueryChildRelations = d3dpt_query_child_relations;
    init.DxgkDdiQueryChildStatus = d3dpt_query_child_status;
    init.DxgkDdiQueryDeviceDescriptor = d3dpt_query_device_descriptor;
    init.DxgkDdiSetPowerState = d3dpt_set_power_state;
    init.DxgkDdiNotifyAcpiEvent = d3dpt_notify_acpi_event;
    init.DxgkDdiResetDevice = d3dpt_reset_device;
    init.DxgkDdiUnload = d3dpt_unload;
    init.DxgkDdiQueryInterface = d3dpt_query_interface;
    init.DxgkDdiControlEtwLogging = d3dpt_control_etw_logging;

    init.DxgkDdiQueryAdapterInfo = d3dpt_query_adapter_info;
    init.DxgkDdiCreateDevice = d3dpt_create_device;
    init.DxgkDdiCreateAllocation = d3dpt_create_allocation;
    init.DxgkDdiDestroyAllocation = d3dpt_destroy_allocation;
    init.DxgkDdiDescribeAllocation = d3dpt_describe_allocation;
    init.DxgkDdiGetStandardAllocationDriverData = d3dpt_get_standard_allocation_driver_data;
    init.DxgkDdiAcquireSwizzlingRange = d3dpt_acquire_swizzling_range;
    init.DxgkDdiReleaseSwizzlingRange = d3dpt_release_swizzling_range;
    init.DxgkDdiPatch = d3dpt_patch;
    init.DxgkDdiSubmitCommand = d3dpt_submit_command;
    init.DxgkDdiPreemptCommand = d3dpt_preempt_command;
    init.DxgkDdiBuildPagingBuffer = d3dpt_build_paging_buffer;
    init.DxgkDdiSetPalette = d3dpt_set_palette;
    init.DxgkDdiSetPointerPosition = d3dpt_set_pointer_position;
    init.DxgkDdiSetPointerShape = d3dpt_set_pointer_shape;
    init.DxgkDdiResetFromTimeout = d3dpt_reset_from_timeout;
    init.DxgkDdiRestartFromTimeout = d3dpt_restart_from_timeout;
    init.DxgkDdiEscape = d3dpt_escape;
    init.DxgkDdiCollectDbgInfo = d3dpt_collect_dbg_info;
    init.DxgkDdiQueryCurrentFence = d3dpt_query_current_fence;
    init.DxgkDdiIsSupportedVidPn = d3dpt_is_supported_vidpn;
    init.DxgkDdiRecommendFunctionalVidPn = d3dpt_recommend_functional_vidpn;
    init.DxgkDdiEnumVidPnCofuncModality = d3dpt_enum_vidpn_cofunc_modality;
    init.DxgkDdiSetVidPnSourceAddress = d3dpt_set_vidpn_source_address;
    init.DxgkDdiSetVidPnSourceVisibility = d3dpt_set_vidpn_source_visibility;
    init.DxgkDdiCommitVidPn = d3dpt_commit_vidpn;
    init.DxgkDdiUpdateActiveVidPnPresentPath = d3dpt_update_active_vidpn_present_path;
    init.DxgkDdiRecommendMonitorModes = d3dpt_recommend_monitor_modes;
    init.DxgkDdiRecommendVidPnTopology = d3dpt_recommend_vidpn_topology;
    init.DxgkDdiGetScanLine = d3dpt_get_scan_line;
    init.DxgkDdiStopCapture = d3dpt_stop_capture;
    init.DxgkDdiControlInterrupt = d3dpt_control_interrupt;
    init.DxgkDdiCreateOverlay = d3dpt_create_overlay;

    init.DxgkDdiDestroyDevice = d3dpt_destroy_device;
    init.DxgkDdiOpenAllocation = d3dpt_open_allocation;
    init.DxgkDdiCloseAllocation = d3dpt_close_allocation;
    init.DxgkDdiRender = d3dpt_render;
    init.DxgkDdiPresent = d3dpt_present;

    init.DxgkDdiUpdateOverlay = d3dpt_update_overlay;
    init.DxgkDdiFlipOverlay = d3dpt_flip_overlay;
    init.DxgkDdiDestroyOverlay = d3dpt_destroy_overlay;

    init.DxgkDdiCreateContext = d3dpt_create_context;
    init.DxgkDdiDestroyContext = d3dpt_destroy_context;

    /* LinkDevice stays NULL: it is for linked adapters (an SLI-style
     * chain), and dxgkrnl calls it right after AddDevice when it is set,
     * dropping the device when it fails */
    init.DxgkDdiSetDisplayPrivateDriverFormat = d3dpt_set_display_private_driver_format;

    init.DxgkDdiQueryVidPnHWCapability = d3dpt_query_vidpn_hw_capability;

    st = DxgkInitialize(drv, reg, &init);
    dbg_hex("d3dptkmd: DriverEntry: DxgkInitialize ", st);
    dbg_puts("\n");
    return st;
}
