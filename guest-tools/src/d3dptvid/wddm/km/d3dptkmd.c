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
} D3DPT_ADAPTER;

/* The DEBUG register of the adapter that started last: stubs called with
 * no adapter handle (Unload, ControlEtwLogging) log through it too. */
static volatile ULONG *g_regs;

/* -------------------------------------------------------------- debug */

static void dbg_puts(const char *s)
{
    volatile ULONG *r = g_regs;

    if (!r) {
        return;
    }
    while (*s) {
        r[D3DPT_FB_REG_DEBUG / 4] = (ULONG)(unsigned char)*s++;
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
    return STATUS_SUCCESS;
}

/* BAR 0 is the first memory resource, BAR 1 (the registers) the second,
 * in the order the PCI bus driver lists them, as the XP miniport's
 * VideoPortGetAccessRanges hands them over. */
static NTSTATUS find_bars(D3DPT_ADAPTER *a)
{
    PCM_RESOURCE_LIST list = a->info.TranslatedResourceList;
    ULONG f, i, mem = 0;

    if (!list) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    for (f = 0; f < list->Count; f++) {
        PCM_PARTIAL_RESOURCE_LIST pl = &list->List[f].PartialResourceList;

        for (i = 0; i < pl->Count; i++) {
            PCM_PARTIAL_RESOURCE_DESCRIPTOR d = &pl->PartialDescriptors[i];

            if (d->Type != CmResourceTypeMemory) {
                continue;
            }
            if (mem == 0) {
                a->vram_phys = d->u.Memory.Start;
                a->vram_len = d->u.Memory.Length;
            } else if (mem == 1) {
                a->regs_phys = d->u.Memory.Start;
                a->regs_len = d->u.Memory.Length;
            }
            mem++;
        }
    }
    return mem >= 2 ? STATUS_SUCCESS : STATUS_DEVICE_CONFIGURATION_ERROR;
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
}

/* ---------------------------------------------- what dxgkrnl asks next */

static DXGKDDI_QUERYADAPTERINFO d3dpt_query_adapter_info;
static NTSTATUS APIENTRY d3dpt_query_adapter_info(IN_CONST_HANDLE h,
                                                  IN_CONST_PDXGKARG_QUERYADAPTERINFO q)
{
    UNREFERENCED_PARAMETER(h);
    dbg_hex("d3dptkmd: QueryAdapterInfo type=", (ULONG)q->Type);
    dbg_hex(" out=", q->OutputDataSize);
    dbg_puts("\n");
    return STATUS_NOT_SUPPORTED;
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
    UNREFERENCED_PARAMETER(enable);
    UNREFERENCED_PARAMETER(flags);
    UNREFERENCED_PARAMETER(level);
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
static NTSTATUS d3dpt_query_child_relations(IN_CONST_PVOID ctx,
                                            PDXGK_CHILD_DESCRIPTOR rel, ULONG size)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(rel);
    UNREFERENCED_PARAMETER(size);
    dbg_line("QueryChildRelations");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_QUERY_CHILD_STATUS d3dpt_query_child_status;
static NTSTATUS d3dpt_query_child_status(IN_CONST_PVOID ctx, INOUT_PDXGK_CHILD_STATUS st,
                                         IN_BOOLEAN non_destructive)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(st);
    UNREFERENCED_PARAMETER(non_destructive);
    dbg_line("QueryChildStatus");
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_QUERY_DEVICE_DESCRIPTOR d3dpt_query_device_descriptor;
static NTSTATUS d3dpt_query_device_descriptor(IN_CONST_PVOID ctx, IN_ULONG uid,
                                              INOUT_PDXGK_DEVICE_DESCRIPTOR desc)
{
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(uid);
    UNREFERENCED_PARAMETER(desc);
    dbg_line("QueryDeviceDescriptor");
    return STATUS_MONITOR_NO_MORE_DESCRIPTOR_DATA;
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
    UNREFERENCED_PARAMETER(qi);
    return STATUS_NOT_SUPPORTED;
}

static DXGKDDI_LINK_DEVICE d3dpt_link_device;
static NTSTATUS d3dpt_link_device(IN_CONST_PDEVICE_OBJECT pdo, IN_CONST_PVOID ctx,
                                  INOUT_PLINKED_DEVICE linked)
{
    UNREFERENCED_PARAMETER(pdo);
    UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(linked);
    dbg_line("LinkDevice");
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
STUB2(DXGKDDI_ISSUPPORTEDVIDPN, d3dpt_is_supported_vidpn, IN_CONST_HANDLE, INOUT_PDXGKARG_ISSUPPORTEDVIDPN)
STUB2(DXGKDDI_RECOMMENDFUNCTIONALVIDPN, d3dpt_recommend_functional_vidpn, IN_CONST_HANDLE, IN_CONST_PDXGKARG_RECOMMENDFUNCTIONALVIDPN_CONST)
STUB2(DXGKDDI_ENUMVIDPNCOFUNCMODALITY, d3dpt_enum_vidpn_cofunc_modality, IN_CONST_HANDLE, IN_CONST_PDXGKARG_ENUMVIDPNCOFUNCMODALITY_CONST)
STUB2(DXGKDDI_SETVIDPNSOURCEADDRESS, d3dpt_set_vidpn_source_address, IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETVIDPNSOURCEADDRESS)
STUB2(DXGKDDI_SETVIDPNSOURCEVISIBILITY, d3dpt_set_vidpn_source_visibility, IN_CONST_HANDLE, IN_CONST_PDXGKARG_SETVIDPNSOURCEVISIBILITY)
STUB2(DXGKDDI_COMMITVIDPN, d3dpt_commit_vidpn, IN_CONST_HANDLE, IN_CONST_PDXGKARG_COMMITVIDPN_CONST)
STUB2(DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH, d3dpt_update_active_vidpn_present_path, IN_CONST_HANDLE, IN_CONST_PDXGKARG_UPDATEACTIVEVIDPNPRESENTPATH_CONST)
STUB2(DXGKDDI_RECOMMENDMONITORMODES, d3dpt_recommend_monitor_modes, IN_CONST_HANDLE, IN_CONST_PDXGKARG_RECOMMENDMONITORMODES_CONST)
STUB2(DXGKDDI_RECOMMENDVIDPNTOPOLOGY, d3dpt_recommend_vidpn_topology, IN_CONST_HANDLE, IN_CONST_PDXGKARG_RECOMMENDVIDPNTOPOLOGY_CONST)
STUB2(DXGKDDI_GETSCANLINE, d3dpt_get_scan_line, IN_CONST_HANDLE, INOUT_PDXGKARG_GETSCANLINE)
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

    init.DxgkDdiLinkDevice = d3dpt_link_device;
    init.DxgkDdiSetDisplayPrivateDriverFormat = d3dpt_set_display_private_driver_format;

    init.DxgkDdiQueryVidPnHWCapability = d3dpt_query_vidpn_hw_capability;

    return DxgkInitialize(drv, reg, &init);
}
