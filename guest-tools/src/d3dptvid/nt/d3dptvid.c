/*
 * d3dptvid.c: the XP video miniport for the d3dpt-vga adapter (doc 15,
 * ADR-008 / M7a). Kernel mode, loaded by videoprt.sys, no CRT.
 *
 * The miniport is the part of an NT display driver that owns the hardware.
 * It finds the PCI device, maps its BARs, enumerates the modes and switches
 * them. Everything that draws lives in the display driver DLL
 * (d3dptdisp.c), which reaches the miniport through the IOCTL_VIDEO_*
 * requests videoprt hands to HwStartIO.
 *
 * The register set is d3dpt/d3dpt_fb.h. The mode list is the host's table
 * read from the register BAR, so the player, not the driver, decides what
 * XP can pick. A mode switch programs WIDTH/HEIGHT/BPP/PITCH/OFFSET and
 * ENABLE. A reset clears ENABLE so the VGA core takes over again, and
 * videoprt does the BIOS int10 mode 3 for BSODs and reboots because
 * HwResetHw returns FALSE.
 *
 * Build: guest-tools/build-driver.sh (mingw-w64 i686, -nostdlib, native
 * subsystem, entry DriverEntry, libvideoprt + the few ntoskrnl imports of
 * the cached VRAM mappings).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/* the video-miniport header set: ntdef.h for the base types, miniport.h
 * instead of ntddk.h (the two conflict), then the video port headers */
#include <ntdef.h>
#include <ddk/dderror.h>
#include <devioctl.h>
#include <ddk/miniport.h>
#include <ntddvdeo.h>
#include <ddk/video.h>
#include "../../../../d3dpt/d3dpt_fb.h"

#define DEBUG_LOG 1

/*
 * VRAM mappings. videoprt's VideoPortMapMemory maps frame buffers uncached
 * or write-combined (VIDEO_MEMORY_SPACE_P6CACHE). That suits a PCI
 * aperture, but BAR 0 here is plain guest RAM that QEMU reads coherently,
 * and uncached reads (GDI scrolling, DirectDraw's HEL copying from a VRAM
 * surface) run at tens of MB/s. So the miniport maps VRAM itself, cached.
 * MmMapIoSpace(MmCached) gives the kernel view the display driver draws
 * through. A view of \Device\PhysicalMemory into the process serves
 * DirectDraw's user-mode Lock (what videoprt does inside
 * VideoPortMapMemory, minus PAGE_NOCACHE). These are the only ntoskrnl
 * imports. The register BAR stays a videoprt (uncached) mapping.
 */
typedef enum _MEMORY_CACHING_TYPE { MmNonCached = 0, MmCached = 1, MmWriteCombined = 2 } MEMORY_CACHING_TYPE;
typedef enum _SECTION_INHERIT { ViewShare = 1, ViewUnmap = 2 } SECTION_INHERIT;
#define SECTION_MAP_WRITE_ 0x0002
#define SECTION_MAP_READ_  0x0004
#define PAGE_READWRITE_    0x04
NTSYSAPI PVOID NTAPI MmMapIoSpace(PHYSICAL_ADDRESS PhysicalAddress, SIZE_T NumberOfBytes, MEMORY_CACHING_TYPE CacheType);
NTSYSAPI VOID NTAPI MmUnmapIoSpace(PVOID BaseAddress, SIZE_T NumberOfBytes);
NTSYSAPI NTSTATUS NTAPI ZwOpenSection(PHANDLE SectionHandle, ULONG DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes);
NTSYSAPI NTSTATUS NTAPI ZwMapViewOfSection(HANDLE SectionHandle, HANDLE ProcessHandle, PVOID *BaseAddress,
                                           ULONG_PTR ZeroBits, SIZE_T CommitSize, PLARGE_INTEGER SectionOffset,
                                           PSIZE_T ViewSize, SECTION_INHERIT InheritDisposition,
                                           ULONG AllocationType, ULONG Win32Protect);
NTSYSAPI NTSTATUS NTAPI ZwUnmapViewOfSection(HANDLE ProcessHandle, PVOID BaseAddress);
NTSYSAPI NTSTATUS NTAPI ZwClose(HANDLE Handle);
NTSYSAPI BOOLEAN NTAPI PsGetVersion(PULONG MajorVersion, PULONG MinorVersion, PULONG BuildNumber, PVOID CSDVersion);

/*
 * The bottom of VRAM on Windows Vista and 7 (track M18). Their boot screen
 * draws through VBE on this adapter's linear frame buffer (1024x768x24,
 * the first 2.25 MiB) and the kernel keeps that view, uncached, for the
 * blue screen. From Vista on the memory manager gives every later mapping
 * of the same physical pages the same cache type, so the cached views
 * above came out uncached over exactly that range: the primary and a
 * full-screen flip chain, where DDTEST wrote its frames at 30 frames/s
 * and a windowed blit took 70 ms. So on NT 6 the screen starts above it.
 * The display driver learns the offset from FrameBufferBase -
 * VideoRamBase; an older display driver sees 0 there and needs nothing
 * else, since the primary is then at the start as before. 4 MiB covers
 * the boot screen at 32 bits too.
 */
#define NT6_FB_BASE (4u << 20)

static WCHAR physmem_name[] = L"\\Device\\PhysicalMemory";

/* a cached view of [vram_phys + offset, +len) in ProcessHandle */
static VP_STATUS map_vram_user(PHYSICAL_ADDRESS base, ULONG offset, ULONG len, HANDLE process, PVOID *va)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    HANDLE section;
    LARGE_INTEGER off;
    SIZE_T view = len;
    NTSTATUS st;

    name.Buffer = physmem_name;
    name.Length = sizeof(physmem_name) - sizeof(WCHAR);
    name.MaximumLength = sizeof(physmem_name);
    InitializeObjectAttributes(&oa, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    st = ZwOpenSection(&section, SECTION_MAP_READ_ | SECTION_MAP_WRITE_, &oa);
    if (st < 0) {
        return ERROR_INVALID_PARAMETER;
    }
    off.QuadPart = base.QuadPart + offset;
    *va = NULL;
    st = ZwMapViewOfSection(section, process, va, 0, 0, &off, &view, ViewUnmap, 0, PAGE_READWRITE_);
    ZwClose(section);
    return st < 0 ? ERROR_NOT_ENOUGH_MEMORY : NO_ERROR;
}

typedef struct _DEVICE_EXTENSION {
    volatile ULONG *regs;                 /* BAR 1 mapped (kernel VA) */
    PHYSICAL_ADDRESS regs_phys;
    ULONG regs_len;
    PHYSICAL_ADDRESS vram_phys;           /* BAR 0 */
    ULONG vram_len;
    PUCHAR vram;                          /* BAR 0 mapped (kernel VA), for clearing */
    ULONG fb_base;                        /* where the screen starts in VRAM: NT6_FB_BASE on Vista and later, else 0 */
    ULONG num_modes;
    PVIDEO_MODE_INFORMATION modes;        /* num_modes entries */
    ULONG cur_mode;                       /* index into modes, or ~0 */
    BOOLEAN enabled;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

/* ------------------------------------------------------------ registers */

static ULONG reg_read(PDEVICE_EXTENSION d, ULONG off)
{
    return VideoPortReadRegisterUlong((PULONG)((PUCHAR)d->regs + off));
}

static void reg_write(PDEVICE_EXTENSION d, ULONG off, ULONG val)
{
    VideoPortWriteRegisterUlong((PULONG)((PUCHAR)d->regs + off), val);
}

/* debug text into the QEMU log through REG_DEBUG (one character per write) */
static void dbg_puts(PDEVICE_EXTENSION d, const char *s)
{
    if (!DEBUG_LOG || !d->regs) {
        return;
    }
    while (*s) {
        reg_write(d, D3DPT_FB_REG_DEBUG, (ULONG)(unsigned char)*s++);
    }
}

static void dbg_hex(PDEVICE_EXTENSION d, const char *tag, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[16];
    int i;

    if (!DEBUG_LOG || !d->regs) {
        return;
    }
    dbg_puts(d, tag);
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - 4 * i)) & 0xf];
    }
    buf[10] = 0;
    dbg_puts(d, buf);
}

/* -------------------------------------------------------------- modes */

static ULONG bpp_pitch(ULONG w, ULONG bpp)
{
    /* 32-byte aligned lines keep every row cache-line aligned */
    return ((w * (bpp / 8)) + 31) & ~31u;
}

static VP_STATUS build_mode_table(PDEVICE_EXTENSION d)
{
    ULONG n, i, kept = 0;

    n = reg_read(d, D3DPT_FB_REG_MODE_COUNT);
    if (n == 0 || n > 256) {
        return ERROR_DEV_NOT_EXIST;
    }
    d->modes = VideoPortAllocatePool(d, VpPagedPool, n * sizeof(VIDEO_MODE_INFORMATION), 0x64336d64);
    if (!d->modes) {
        return ERROR_NOT_ENOUGH_MEMORY;
    }
    VideoPortZeroMemory(d->modes, n * sizeof(VIDEO_MODE_INFORMATION));

    for (i = 0; i < n; i++) {
        ULONG w, h, bpp, hz, pitch;
        PVIDEO_MODE_INFORMATION m;

        reg_write(d, D3DPT_FB_REG_MODE_SEL, i);
        w = reg_read(d, D3DPT_FB_REG_MODE_W);
        h = reg_read(d, D3DPT_FB_REG_MODE_H);
        bpp = reg_read(d, D3DPT_FB_REG_MODE_BPP);
        hz = reg_read(d, D3DPT_FB_REG_MODE_HZ);
        if (!w || !h || (bpp != 8 && bpp != 16 && bpp != 32)) {
            continue;
        }
        pitch = bpp_pitch(w, bpp);
        if (pitch * h > d->vram_len) {
            continue;
        }
        m = &d->modes[kept];
        m->Length = sizeof(*m);
        m->ModeIndex = kept;
        m->VisScreenWidth = w;
        m->VisScreenHeight = h;
        m->ScreenStride = pitch;
        m->NumberOfPlanes = 1;
        m->BitsPerPlane = bpp;
        m->Frequency = hz;
        m->XMillimeter = 320;                  /* a 4:3 monitor of 16" diagonal */
        m->YMillimeter = 240;
        m->AttributeFlags = VIDEO_MODE_COLOR | VIDEO_MODE_GRAPHICS | VIDEO_MODE_NO_OFF_SCREEN;
        if (bpp == 32) {
            m->NumberRedBits = m->NumberGreenBits = m->NumberBlueBits = 8;
            m->RedMask = 0x00ff0000; m->GreenMask = 0x0000ff00; m->BlueMask = 0x000000ff;
        } else if (bpp == 16) {
            m->NumberRedBits = 5; m->NumberGreenBits = 6; m->NumberBlueBits = 5;
            m->RedMask = 0xf800; m->GreenMask = 0x07e0; m->BlueMask = 0x001f;
        } else {
            /* 8 bpp: indices into the device's PALETTE block (8 bits per
             * gun). GDI manages the palette and sets it through
             * IOCTL_VIDEO_SET_COLOR_REGISTERS / the display driver. */
            m->NumberRedBits = m->NumberGreenBits = m->NumberBlueBits = 8;
            m->RedMask = m->GreenMask = m->BlueMask = 0;
            m->AttributeFlags |= VIDEO_MODE_PALETTE_DRIVEN | VIDEO_MODE_MANAGED_PALETTE;
        }
        m->VideoMemoryBitmapWidth = w;
        m->VideoMemoryBitmapHeight = h;
        m->DriverSpecificAttributeFlags = 0;
        kept++;
    }
    d->num_modes = kept;
    dbg_hex(d, "modes: ", kept);
    dbg_puts(d, "\n");
    return kept ? NO_ERROR : ERROR_DEV_NOT_EXIST;
}

static VP_STATUS set_mode(PDEVICE_EXTENSION d, ULONG index, BOOLEAN zero)
{
    PVIDEO_MODE_INFORMATION m;

    if (index >= d->num_modes) {
        return ERROR_INVALID_PARAMETER;
    }
    m = &d->modes[index];
    reg_write(d, D3DPT_FB_REG_ENABLE, 0);
    /* As on real adapters, the new mode comes up black, not with the old
     * desktop bytes reinterpreted at the new pitch. */
    if (zero && d->vram) {
        VideoPortZeroMemory(d->vram + d->fb_base, m->ScreenStride * m->VisScreenHeight);
    }
    reg_write(d, D3DPT_FB_REG_WIDTH, m->VisScreenWidth);
    reg_write(d, D3DPT_FB_REG_HEIGHT, m->VisScreenHeight);
    reg_write(d, D3DPT_FB_REG_BPP, m->BitsPerPlane);
    reg_write(d, D3DPT_FB_REG_PITCH, m->ScreenStride);
    reg_write(d, D3DPT_FB_REG_OFFSET, d->fb_base);
    reg_write(d, D3DPT_FB_REG_HZ, m->Frequency);
    reg_write(d, D3DPT_FB_REG_ENABLE, 1);
    d->cur_mode = index;
    d->enabled = TRUE;
    return NO_ERROR;
}

static void reset_to_vga(PDEVICE_EXTENSION d)
{
    if (d->regs) {
        reg_write(d, D3DPT_FB_REG_ENABLE, 0);
    }
    d->enabled = FALSE;
}

/* ------------------------------------------------------- miniport entry */

static VP_STATUS NTAPI HwFindAdapter(PVOID ext, PVOID ctx, PWSTR args,
                                     PVIDEO_PORT_CONFIG_INFO cfg, PUCHAR again)
{
    PDEVICE_EXTENSION d = ext;
    VIDEO_ACCESS_RANGE ranges[2];
    USHORT vendor = D3DPT_FB_PCI_VENDOR, device = D3DPT_FB_PCI_DEVICE;
    ULONG slot = 0;
    VP_STATUS st;
    ULONG magic;

    if (cfg->Length < sizeof(VIDEO_PORT_CONFIG_INFO)) {
        return ERROR_INVALID_PARAMETER;
    }
    VideoPortZeroMemory(ranges, sizeof(ranges));
    /* PnP: videoprt already knows the device; this fetches its BARs */
    st = VideoPortGetAccessRanges(d, 0, NULL, 2, ranges, &vendor, &device, &slot);
    if (st != NO_ERROR) {
        VideoPortDebugPrint(Error, "d3dptvid: GetAccessRanges %x\n", st);
        return ERROR_DEV_NOT_EXIST;
    }
    if (ranges[0].RangeLength == 0 || ranges[1].RangeLength < D3DPT_FB_REGS_SIZE ||
        ranges[0].RangeInIoSpace || ranges[1].RangeInIoSpace) {
        return ERROR_DEV_NOT_EXIST;
    }
    d->vram_phys = ranges[0].RangeStart;
    d->vram_len = ranges[0].RangeLength;
    d->regs_phys = ranges[1].RangeStart;
    d->regs_len = D3DPT_FB_REGS_SIZE;

    d->regs = VideoPortGetDeviceBase(d, d->regs_phys, d->regs_len, VIDEO_MEMORY_SPACE_MEMORY);
    if (!d->regs) {
        return ERROR_DEV_NOT_EXIST;
    }
    magic = reg_read(d, D3DPT_FB_REG_MAGIC);
    /* Accept our version or newer. A newer register set only adds
     * registers (d3dpt_fb.h), so an installed driver keeps working when
     * QEMU moves on. Only an older one is refused. */
    if (magic != D3DPT_FB_MAGIC || reg_read(d, D3DPT_FB_REG_VERSION) < D3DPT_FB_VERSION) {
        VideoPortDebugPrint(Error, "d3dptvid: bad magic %x or register set %u older than %u\n",
                            magic, reg_read(d, D3DPT_FB_REG_VERSION), D3DPT_FB_VERSION);
        VideoPortFreeDeviceBase(d, (PVOID)d->regs);
        d->regs = NULL;
        return ERROR_DEV_NOT_EXIST;
    }
    /* All of VRAM in kernel space, for mode-set clearing and the display
     * driver's DirectDraw heap. 32 MiB of system PTEs is what any real
     * adapter's miniport takes. */
    d->vram = MmMapIoSpace(d->vram_phys, d->vram_len, MmCached);
    {
        ULONG major = 0;
        PsGetVersion(&major, NULL, NULL, NULL);
        d->fb_base = major >= 6 && d->vram_len >= 4 * NT6_FB_BASE ? NT6_FB_BASE : 0;
    }
    dbg_puts(d, "d3dptvid: adapter found\n");
    dbg_hex(d, "  vram ", d->vram_phys.LowPart);
    dbg_hex(d, " len ", d->vram_len);
    dbg_hex(d, " regs ", d->regs_phys.LowPart);
    dbg_hex(d, " screen at ", d->fb_base);
    dbg_puts(d, "\n");

    st = build_mode_table(d);
    if (st != NO_ERROR) {
        return st;
    }
    d->cur_mode = ~0u;

    /* What the display driver sees in the registry. The chip strings only
     * show in Display Properties' Adapter tab. */
    {
        static WCHAR chip[] = L"2ksbox d3dpt-vga";
        static WCHAR dac[] = L"paravirtual";
        static WCHAR adapter[] = L"d3dpt-vga paravirtual framebuffer";
        ULONG mem = d->vram_len;
        VideoPortSetRegistryParameters(d, L"HardwareInformation.ChipType", chip, sizeof(chip));
        VideoPortSetRegistryParameters(d, L"HardwareInformation.DacType", dac, sizeof(dac));
        VideoPortSetRegistryParameters(d, L"HardwareInformation.AdapterString", adapter, sizeof(adapter));
        VideoPortSetRegistryParameters(d, L"HardwareInformation.MemorySize", &mem, sizeof(mem));
    }

    cfg->NumEmulatorAccessEntries = 0;
    cfg->EmulatorAccessEntries = NULL;
    cfg->EmulatorAccessEntriesContext = 0;
    cfg->VdmPhysicalVideoMemoryAddress.QuadPart = 0;
    cfg->VdmPhysicalVideoMemoryLength = 0;
    cfg->HardwareStateSize = 0;
    *again = 0;
    return NO_ERROR;
}

static BOOLEAN NTAPI HwInitialize(PVOID ext)
{
    return TRUE;
}

static BOOLEAN NTAPI HwResetHw(PVOID ext, ULONG columns, ULONG rows)
{
    /* Back to the VGA core. FALSE makes videoprt do the int10 mode 3. */
    reset_to_vga(ext);
    return FALSE;
}

static VP_STATUS NTAPI HwGetPowerState(PVOID ext, ULONG id, PVIDEO_POWER_MANAGEMENT pm)
{
    return NO_ERROR;
}

static VP_STATUS NTAPI HwSetPowerState(PVOID ext, ULONG id, PVIDEO_POWER_MANAGEMENT pm)
{
    return NO_ERROR;
}

static VP_STATUS NTAPI HwGetVideoChildDescriptor(PVOID ext, PVIDEO_CHILD_ENUM_INFO info,
                                             PVIDEO_CHILD_TYPE type, PUCHAR desc,
                                             PULONG uid, PULONG unused)
{
    /* No children. XP attaches its default monitor and the mode picker
     * uses our list. */
    return VIDEO_ENUM_NO_MORE_DEVICES;
}

static BOOLEAN NTAPI HwStartIO(PVOID ext, PVIDEO_REQUEST_PACKET rp)
{
    PDEVICE_EXTENSION d = ext;
    VP_STATUS st = NO_ERROR;

    rp->StatusBlock->Information = 0;

    switch (rp->IoControlCode) {
    case IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES: {
        PVIDEO_NUM_MODES nm = rp->OutputBuffer;
        if (rp->OutputBufferLength < sizeof(*nm)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        nm->NumModes = d->num_modes;
        nm->ModeInformationLength = sizeof(VIDEO_MODE_INFORMATION);
        rp->StatusBlock->Information = sizeof(*nm);
        break;
    }
    case IOCTL_VIDEO_QUERY_AVAIL_MODES: {
        ULONG need = d->num_modes * sizeof(VIDEO_MODE_INFORMATION);
        if (rp->OutputBufferLength < need) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        VideoPortMoveMemory(rp->OutputBuffer, d->modes, need);
        rp->StatusBlock->Information = need;
        break;
    }
    case IOCTL_VIDEO_QUERY_CURRENT_MODE: {
        if (rp->OutputBufferLength < sizeof(VIDEO_MODE_INFORMATION)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        if (d->cur_mode >= d->num_modes) {
            st = ERROR_INVALID_FUNCTION;
            break;
        }
        VideoPortMoveMemory(rp->OutputBuffer, &d->modes[d->cur_mode], sizeof(VIDEO_MODE_INFORMATION));
        rp->StatusBlock->Information = sizeof(VIDEO_MODE_INFORMATION);
        break;
    }
    case IOCTL_VIDEO_SET_CURRENT_MODE: {
        PVIDEO_MODE vm = rp->InputBuffer;
        if (rp->InputBufferLength < sizeof(*vm)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        dbg_hex(d, "d3dptvid: set mode ", vm->RequestedMode);
        dbg_puts(d, "\n");
        st = set_mode(d, vm->RequestedMode & ~(VIDEO_MODE_NO_ZERO_MEMORY | VIDEO_MODE_MAP_MEM_LINEAR),
                      !(vm->RequestedMode & VIDEO_MODE_NO_ZERO_MEMORY));
        break;
    }
    case IOCTL_VIDEO_RESET_DEVICE:
        dbg_puts(d, "d3dptvid: reset device\n");
        reset_to_vga(d);
        break;

    case IOCTL_VIDEO_MAP_VIDEO_MEMORY: {
        PVIDEO_MEMORY in = rp->InputBuffer;
        PVIDEO_MEMORY_INFORMATION out = rp->OutputBuffer;
        if (rp->InputBufferLength < sizeof(*in) || rp->OutputBufferLength < sizeof(*out)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        if (!d->vram) {
            st = ERROR_NOT_ENOUGH_MEMORY;
            break;
        }
        /* the display driver draws through the miniport's cached kernel view */
        out->VideoRamBase = d->vram;
        out->VideoRamLength = d->vram_len;
        out->FrameBufferBase = d->vram + d->fb_base;
        out->FrameBufferLength = d->vram_len - d->fb_base;
        rp->StatusBlock->Information = sizeof(*out);
        break;
    }
    case IOCTL_VIDEO_UNMAP_VIDEO_MEMORY:
        /* the kernel view lives as long as the adapter */
        break;
    case IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES: {
        /* The register page for the display driver (debug log, the
         * DirectDraw/Direct3D DDI registers). */
        PVIDEO_PUBLIC_ACCESS_RANGES out = rp->OutputBuffer;
        /* A kernel mapping, because the display driver runs in kernel mode
         * in any process context. A user-mode mapping would only be valid
         * in the process that created the PDEV. */
        ULONG len = d->regs_len, space = VIDEO_MEMORY_SPACE_MEMORY;
        if (rp->OutputBufferLength < sizeof(*out)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        out->InIoSpace = FALSE;
        out->MappedInIoSpace = FALSE;
        out->VirtualAddress = NULL;
        st = VideoPortMapMemory(d, d->regs_phys, &len, &space, &out->VirtualAddress);
        if (st == NO_ERROR) {
            rp->StatusBlock->Information = sizeof(*out);
        }
        break;
    }
    case IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES: {
        PVIDEO_MEMORY in = rp->InputBuffer;
        if (rp->InputBufferLength < sizeof(*in)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        st = VideoPortUnmapMemory(d, in->RequestedVirtualAddress, NULL);
        break;
    }
    case IOCTL_VIDEO_SHARE_VIDEO_MEMORY: {
        /* DirectDraw (DdMapMemory) maps VRAM into a user process. By the
         * port driver's convention the process handle goes in as the
         * virtual address and the user-mode pointer comes back in its
         * place. */
        PVIDEO_SHARE_MEMORY in = rp->InputBuffer;
        PVIDEO_SHARE_MEMORY_INFORMATION out = rp->OutputBuffer;
        PVOID va;
        if (rp->InputBufferLength < sizeof(*in) || rp->OutputBufferLength < sizeof(*out)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        if (in->ViewOffset > d->vram_len || in->ViewSize > d->vram_len - in->ViewOffset) {
            st = ERROR_INVALID_PARAMETER;
            break;
        }
        st = map_vram_user(d->vram_phys, in->ViewOffset, in->ViewSize, in->ProcessHandle, &va);
        if (st != NO_ERROR) {
            dbg_puts(d, "d3dptvid: user VRAM view failed\n");
            break;
        }
        out->SharedViewOffset = in->ViewOffset;
        out->SharedViewSize = in->ViewSize;
        out->VirtualAddress = va;
        rp->StatusBlock->Information = sizeof(*out);
        break;
    }
    case IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY: {
        PVIDEO_SHARE_MEMORY in = rp->InputBuffer;
        if (rp->InputBufferLength < sizeof(*in)) {
            st = ERROR_INSUFFICIENT_BUFFER;
            break;
        }
        ZwUnmapViewOfSection(in->ProcessHandle, in->RequestedVirtualAddress);
        break;
    }
    case IOCTL_VIDEO_SET_COLOR_REGISTERS: {
        /* 8 bpp modes. The CLUT goes into the device's PALETTE block, one
         * x8r8g8b8 register per entry, and the host applies it at its next
         * refresh. */
        PVIDEO_CLUT clut = rp->InputBuffer;
        ULONG i;
        if (rp->InputBufferLength < sizeof(VIDEO_CLUT) ||
            rp->InputBufferLength < 4 + (ULONG)clut->NumEntries * 4 ||
            clut->FirstEntry >= D3DPT_FB_PALETTE_SIZE ||
            clut->NumEntries > D3DPT_FB_PALETTE_SIZE - clut->FirstEntry) {
            st = ERROR_INVALID_PARAMETER;
            break;
        }
        for (i = 0; i < clut->NumEntries; i++) {
            const VIDEO_CLUTDATA *c = &clut->LookupTable[i].RgbArray;
            reg_write(d, D3DPT_FB_REG_PALETTE + 4 * (clut->FirstEntry + i),
                      ((ULONG)c->Red << 16) | ((ULONG)c->Green << 8) | c->Blue);
        }
        break;
    }
    default:
        st = ERROR_INVALID_FUNCTION;
        break;
    }

    rp->StatusBlock->Status = st;
    return TRUE;
}

ULONG NTAPI DriverEntry(PVOID ctx1, PVOID ctx2)
{
    VIDEO_HW_INITIALIZATION_DATA init;

    VideoPortZeroMemory(&init, sizeof(init));
    init.HwInitDataSize = sizeof(init);
    init.AdapterInterfaceType = PCIBus;
    init.HwFindAdapter = HwFindAdapter;
    init.HwInitialize = HwInitialize;
    init.HwStartIO = HwStartIO;
    init.HwResetHw = HwResetHw;
    init.HwGetPowerState = HwGetPowerState;
    init.HwSetPowerState = HwSetPowerState;
    init.HwGetVideoChildDescriptor = HwGetVideoChildDescriptor;
    init.HwDeviceExtensionSize = sizeof(DEVICE_EXTENSION);
    return VideoPortInitialize(ctx1, ctx2, &init, NULL);
}
