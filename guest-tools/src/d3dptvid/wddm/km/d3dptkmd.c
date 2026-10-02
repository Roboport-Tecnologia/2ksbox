/*
 * d3dptkmd.c: the WDDM kernel-mode driver for the d3dpt-vga adapter on
 * Windows 7 (M18 step 2, ADR-022). Loaded by dxgkrnl.sys through
 * DxgkInitialize; the XP-model miniport (../../nt/d3dptvid.c) stays the
 * driver for XP and the fallback on Windows 7.
 *
 * This is plan step 2's empty driver: the device-level callbacks are
 * real (add, start, stop, remove), every other DDI is a stub that names
 * itself in the QEMU log and fails. The log is the device's DEBUG
 * register, one character per write (doc 15's rule: the QEMU log, never
 * a debugger), so the first boot shows what dxgkrnl asks for next.
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

#define D3DPT_TAG 'kd3d'

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
} D3DPT_ADAPTER;

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
    if (magic != D3DPT_FB_MAGIC || version < D3DPT_FB_VERSION) {
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

    read_modes(a);

    /* one scanout, one monitor */
    *sources = 1;
    *children = 1;
    return STATUS_SUCCESS;
}

static void unmap(D3DPT_ADAPTER *a)
{
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
 * WDDM 1.0 GPU with one engine, 32-bit addresses, no overlays, no swizzling
 * and, for now, no hardware cursor (the XP driver's CURSOR registers come
 * with the VidPN work). */
static NTSTATUS driver_caps(DXGK_DRIVERCAPS *c)
{
    RtlZeroMemory(c, sizeof(*c));
    c->HighestAcceptableAddress.QuadPart = 0xffffffffull;
    c->MaxAllocationListSlotId = 16;
    c->MaxQueuedFlipOnVSync = 1;
    c->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
    c->WDDMVersion = DXGKDDI_WDDMv1;
    return STATUS_SUCCESS;
}

/* One memory segment: VRAM below the command window, linear and CPU
 * visible (BAR 0). Its GPU address is the VRAM offset itself, so an
 * allocation's segment address is what the OFFSET register takes. The
 * paging buffer is in system memory (segment 0). */
static NTSTATUS query_segment(const D3DPT_ADAPTER *a, const DXGKARG_QUERYADAPTERINFO *q)
{
    DXGK_QUERYSEGMENTOUT *o = (DXGK_QUERYSEGMENTOUT *)q->pOutputData;
    DXGK_SEGMENTDESCRIPTOR *d;

    if (q->OutputDataSize < sizeof(*o)) {
        return STATUS_INVALID_PARAMETER;
    }
    if (!o->pSegmentDescriptor) {               /* the first call asks how many */
        o->NbSegment = 1;
        return STATUS_SUCCESS;
    }
    d = o->pSegmentDescriptor;
    RtlZeroMemory(d, sizeof(*d));
    d->BaseAddress.QuadPart = 0;
    d->CpuTranslatedAddress = a->vram_phys;
    d->Size = a->seg_size;
    d->CommitLimit = a->seg_size;
    d->Flags.CpuVisible = 1;
    o->NbSegment = 1;
    o->PagingBufferSegmentId = 0;
    o->PagingBufferSize = 64 * 1024;
    o->PagingBufferPrivateDataSize = 0;
    return STATUS_SUCCESS;
}

static DXGKDDI_QUERYADAPTERINFO d3dpt_query_adapter_info;
static NTSTATUS APIENTRY d3dpt_query_adapter_info(IN_CONST_HANDLE h,
                                                  IN_CONST_PDXGKARG_QUERYADAPTERINFO q)
{
    const D3DPT_ADAPTER *a = (const D3DPT_ADAPTER *)h;
    NTSTATUS st;

    switch (q->Type) {
    case DXGKQAITYPE_DRIVERCAPS:
        st = q->OutputDataSize < sizeof(DXGK_DRIVERCAPS)
             ? STATUS_INVALID_PARAMETER : driver_caps((DXGK_DRIVERCAPS *)q->pOutputData);
        break;
    case DXGKQAITYPE_QUERYSEGMENT:
        st = query_segment(a, q);
        break;
    default:                                    /* the user-mode driver's private data, later */
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    dbg_hex("d3dptkmd: QueryAdapterInfo type=", (ULONG)q->Type);
    dbg_hex(" out=", q->OutputDataSize);
    dbg_hex(" -> ", (ULONG)st);
    dbg_puts("\n");
    return st;
}

static DXGKDDI_INTERRUPT_ROUTINE d3dpt_interrupt;
static BOOLEAN d3dpt_interrupt(IN_CONST_PVOID ctx, IN_ULONG msg)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(msg);
    return FALSE;       /* the device has no interrupt yet (plan step 4) */
}

static DXGKDDI_DPC_ROUTINE d3dpt_dpc;
static VOID d3dpt_dpc(IN_CONST_PVOID ctx)
{
    UNREFERENCED_PARAMETER(ctx);
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

/* No EDID yet: the monitor's modes come from RecommendMonitorModes, out of
 * the host's mode table (the XP driver's list). */
static DXGKDDI_QUERY_DEVICE_DESCRIPTOR d3dpt_query_device_descriptor;
static NTSTATUS d3dpt_query_device_descriptor(IN_CONST_PVOID ctx, IN_ULONG uid,
                                              INOUT_PDXGK_DEVICE_DESCRIPTOR desc)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(uid);
    UNREFERENCED_PARAMETER(desc);
    dbg_line("QueryDeviceDescriptor");
    return STATUS_MONITOR_NO_DESCRIPTOR;
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

/* the adapter, device, context and overlay halves (d3dkmddi.h) */
STUB2(DXGKDDI_CREATEDEVICE, d3dpt_create_device, IN_CONST_HANDLE, INOUT_PDXGKARG_CREATEDEVICE)
STUB2(DXGKDDI_CREATEALLOCATION, d3dpt_create_allocation, IN_CONST_HANDLE, INOUT_PDXGKARG_CREATEALLOCATION)
STUB2(DXGKDDI_DESTROYALLOCATION, d3dpt_destroy_allocation, IN_CONST_HANDLE, IN_CONST_PDXGKARG_DESTROYALLOCATION)
STUB2(DXGKDDI_DESCRIBEALLOCATION, d3dpt_describe_allocation, IN_CONST_HANDLE, INOUT_PDXGKARG_DESCRIBEALLOCATION)
STUB2(DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA, d3dpt_get_standard_allocation_driver_data, IN_CONST_HANDLE, INOUT_PDXGKARG_GETSTANDARDALLOCATIONDRIVERDATA)
STUB2(DXGKDDI_ACQUIRESWIZZLINGRANGE, d3dpt_acquire_swizzling_range, IN_CONST_HANDLE, INOUT_PDXGKARG_ACQUIRESWIZZLINGRANGE)
STUB2(DXGKDDI_RELEASESWIZZLINGRANGE, d3dpt_release_swizzling_range, IN_CONST_HANDLE, IN_CONST_PDXGKARG_RELEASESWIZZLINGRANGE)
STUB2(DXGKDDI_PATCH, d3dpt_patch, IN_CONST_HANDLE, IN_CONST_PDXGKARG_PATCH)
STUB2(DXGKDDI_SUBMITCOMMAND, d3dpt_submit_command, IN_CONST_HANDLE, IN_CONST_PDXGKARG_SUBMITCOMMAND)
STUB2(DXGKDDI_PREEMPTCOMMAND, d3dpt_preempt_command, IN_CONST_HANDLE, IN_CONST_PDXGKARG_PREEMPTCOMMAND)
STUB2(DXGKDDI_BUILDPAGINGBUFFER, d3dpt_build_paging_buffer, IN_CONST_HANDLE, IN_PDXGKARG_BUILDPAGINGBUFFER)
STUB2(DXGKDDI_SETPALETTE, d3dpt_set_palette, IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETPALETTE)
STUB2(DXGKDDI_SETPOINTERPOSITION, d3dpt_set_pointer_position, IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETPOINTERPOSITION)
STUB2(DXGKDDI_SETPOINTERSHAPE, d3dpt_set_pointer_shape, IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETPOINTERSHAPE)
STUB1(DXGKDDI_RESETFROMTIMEOUT, d3dpt_reset_from_timeout, IN_CONST_HANDLE)
STUB1(DXGKDDI_RESTARTFROMTIMEOUT, d3dpt_restart_from_timeout, IN_CONST_HANDLE)
STUB2(DXGKDDI_ESCAPE, d3dpt_escape, IN_CONST_HANDLE, IN_CONST_PDXGKARG_ESCAPE)
STUB2(DXGKDDI_COLLECTDBGINFO, d3dpt_collect_dbg_info, IN_CONST_HANDLE, IN_CONST_PDXGKARG_COLLECTDBGINFO)
STUB2(DXGKDDI_QUERYCURRENTFENCE, d3dpt_query_current_fence, IN_CONST_HANDLE, INOUT_PDXGKARG_QUERYCURRENTFENCE)
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
        m->Format.Graphics.PixelFormat = D3DDDIFMT_X8R8G8B8;
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
        BOOLEAN src_pivot = e->EnumPivotType == D3DKMDT_EPT_VIDPNSOURCE &&
                            e->EnumPivot.VidPnSourceId == path->VidPnSourceId;
        BOOLEAN tgt_pivot = e->EnumPivotType == D3DKMDT_EPT_VIDPNTARGET &&
                            e->EnumPivot.VidPnTargetId == path->VidPnTargetId;

        /* what is pinned on either end */
        if (NT_SUCCESS(vi->pfnAcquireSourceModeSet(e->hConstrainingVidPn, path->VidPnSourceId, &hsrc, &si))) {
            si->pfnAcquirePinnedModeInfo(hsrc, &psrc);
        } else {
            hsrc = NULL;
        }
        if (NT_SUCCESS(vi->pfnAcquireTargetModeSet(e->hConstrainingVidPn, path->VidPnTargetId, &htgt, &ti))) {
            ti->pfnAcquirePinnedModeInfo(htgt, &ptgt);
        } else {
            htgt = NULL;
        }

        if (!psrc && !src_pivot) {
            fill_source_modes(a, vi, e->hConstrainingVidPn, path->VidPnSourceId, ptgt);
        }
        if (!ptgt && !tgt_pivot) {
            fill_target_modes(a, vi, e->hConstrainingVidPn, path->VidPnTargetId, psrc);
        }

        if (psrc) {
            si->pfnReleaseModeInfo(hsrc, psrc);
        }
        if (hsrc) {
            vi->pfnReleaseSourceModeSet(e->hConstrainingVidPn, hsrc);
        }
        if (ptgt) {
            ti->pfnReleaseModeInfo(htgt, ptgt);
        }
        if (htgt) {
            vi->pfnReleaseTargetModeSet(e->hConstrainingVidPn, htgt);
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
    return st == STATUS_GRAPHICS_NO_MORE_ELEMENTS_IN_DATASET ? STATUS_SUCCESS : st;
}

/* One source, one target, any of the table's modes: whatever dxgkrnl
 * proposes from what EnumVidPnCofuncModality offered is supported. */
static DXGKDDI_ISSUPPORTEDVIDPN d3dpt_is_supported_vidpn;
static NTSTATUS APIENTRY d3dpt_is_supported_vidpn(IN_CONST_HANDLE h, INOUT_PDXGKARG_ISSUPPORTEDVIDPN s)
{
    UNREFERENCED_PARAMETER(h);
    s->IsVidPnSupported = TRUE;
    return STATUS_SUCCESS;
}

static DXGKDDI_RECOMMENDFUNCTIONALVIDPN d3dpt_recommend_functional_vidpn;
static NTSTATUS APIENTRY d3dpt_recommend_functional_vidpn(IN_CONST_HANDLE h,
                                                          IN_CONST_PDXGKARG_RECOMMENDFUNCTIONALVIDPN_CONST r)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(r);
    return STATUS_GRAPHICS_NO_RECOMMENDED_FUNCTIONAL_VIDPN;
}

static DXGKDDI_RECOMMENDVIDPNTOPOLOGY d3dpt_recommend_vidpn_topology;
static NTSTATUS APIENTRY d3dpt_recommend_vidpn_topology(IN_CONST_HANDLE h,
                                                        IN_CONST_PDXGKARG_RECOMMENDVIDPNTOPOLOGY_CONST r)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(r);
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

    if (s->PrimarySegment == 1) {
        a->regs[D3DPT_FB_REG_OFFSET / 4] = s->PrimaryAddress.LowPart;
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

static DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH d3dpt_update_active_vidpn_present_path;
static NTSTATUS APIENTRY d3dpt_update_active_vidpn_present_path(IN_CONST_HANDLE h,
    IN_CONST_PDXGKARG_UPDATEACTIVEVIDPNPRESENTPATH_CONST u)
{
    UNREFERENCED_PARAMETER(h);
    UNREFERENCED_PARAMETER(u);
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
STUB1(DXGKDDI_DESTROYDEVICE, d3dpt_destroy_device, IN_CONST_HANDLE)
STUB2(DXGKDDI_OPENALLOCATIONINFO, d3dpt_open_allocation, IN_CONST_HANDLE, IN_CONST_PDXGKARG_OPENALLOCATION)
STUB2(DXGKDDI_CLOSEALLOCATION, d3dpt_close_allocation, IN_CONST_HANDLE, IN_CONST_PDXGKARG_CLOSEALLOCATION)
STUB2(DXGKDDI_RENDER, d3dpt_render, IN_CONST_HANDLE, INOUT_PDXGKARG_RENDER)
STUB2(DXGKDDI_PRESENT, d3dpt_present, IN_CONST_HANDLE, INOUT_PDXGKARG_PRESENT)
STUB2(DXGKDDI_UPDATEOVERLAY, d3dpt_update_overlay, IN_CONST_HANDLE, IN_CONST_PDXGKARG_UPDATEOVERLAY)
STUB2(DXGKDDI_FLIPOVERLAY, d3dpt_flip_overlay, IN_CONST_HANDLE, IN_CONST_PDXGKARG_FLIPOVERLAY)
STUB1(DXGKDDI_DESTROYOVERLAY, d3dpt_destroy_overlay, IN_CONST_HANDLE)
STUB2(DXGKDDI_CREATECONTEXT, d3dpt_create_context, IN_CONST_HANDLE, INOUT_PDXGKARG_CREATECONTEXT)
STUB1(DXGKDDI_DESTROYCONTEXT, d3dpt_destroy_context, IN_CONST_HANDLE)
STUB2(DXGKDDI_SETDISPLAYPRIVATEDRIVERFORMAT, d3dpt_set_display_private_driver_format, IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETDISPLAYPRIVATEDRIVERFORMAT)
STUB2(DXGKDDI_QUERYVIDPNHWCAPABILITY, d3dpt_query_vidpn_hw_capability, IN_CONST_HANDLE, INOUT_PDXGKARG_QUERYVIDPNHWCAPABILITY)

/* CONTROLINTERRUPT takes two values, not a structure */
static DXGKDDI_CONTROLINTERRUPT d3dpt_control_interrupt;
static NTSTATUS APIENTRY d3dpt_control_interrupt(IN_CONST_HANDLE h,
                                                 IN_CONST_DXGK_INTERRUPT_TYPE type,
                                                 IN_BOOLEAN enable)
{
    UNREFERENCED_PARAMETER(h);
    dbg_hex("d3dptkmd: ControlInterrupt type=", (ULONG)type);
    dbg_hex(" enable=", (ULONG)enable);
    dbg_puts("\n");
    return STATUS_NOT_SUPPORTED;
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
