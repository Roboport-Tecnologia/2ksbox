/*
 * d3dptdisp.c: the XP display driver DLL for the d3dpt-vga adapter (doc
 * 15, ADR-008). Kernel mode (win32k loads it), no CRT, the GDI DDI of
 * winddi.h. The DirectDraw/Direct3D logic shared with the 9x driver lives
 * in ../core/, and this file is the NT layer around it.
 *
 * GDI (M7a). The driver exposes the modes its miniport (d3dptvid.sys)
 * enumerates, switches to one and maps the linear frame buffer. GDI draws
 * every pixel itself, straight into guest VRAM, which QEMU shows without a
 * copy and the player uploads by dirty rectangle. No GDI drawing is
 * accelerated. The pointer is a hardware cursor sprite (register set v4).
 *
 * DirectDraw (M7b). The surface is a device surface GDI still draws on
 * (EngModifySurface with pvScan0). VRAM after the primary is one linear
 * heap dxg.sys allocates DirectDraw surfaces from. DdMapMemory maps VRAM
 * into the game's process. DdFlip writes the back buffer's offset into the
 * device's OFFSET register (a real page flip, no copy), and
 * DdWaitForVerticalBlank waits for the device's frame counter. DirectDraw's
 * HEL does every blit on the mapped VRAM.
 *
 * Direct3D (M7c). A DX7 non-T&L HAL plus the DX8 DDI. Every surface dxg
 * creates is registered with the host by its VRAM offset
 * (DdCreateSurfaceEx). A context is a render target + Z pair
 * (D3dContextCreate). D3dDrawPrimitives2 copies the runtime's DP2 token
 * stream and vertex buffer into the device's command window (top 64 MiB
 * of VRAM, d3dpt_proto.h layout, encoder d3dpt/d3dpt_enc.h) and rings the
 * DOORBELL register. The host executor replays the tokens and, at
 * EndScene / Lock / Flip, writes the rendered frame back into the render
 * target's VRAM (READBACK), so flips and HEL blits see it.
 *
 * Build: guest-tools/build-driver.sh.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdarg.h>
#include <windef.h>
#include <wingdi.h>
#include <winddi.h>
#include <devioctl.h>
#include <ntddvdeo.h>
#include <d3dnthal.h>
#include "../core/d3dpt_core.h"

#define ALLOC_TAG 0x64336d64   /* 'dm3d' */

/* The core defines its own copies of the three DirectDraw-internal bits
 * it uses, because it includes no DDK header (doc 19 §19). Both are
 * visible here, so this is where they are checked. */
typedef char d3dpt_bits_assert[
    (DDSCAPS_EXECUTEBUFFER_ == DDSCAPS_EXECUTEBUFFER &&
     DDRAWISURF_HASCKEYSRCBLT_ == DDRAWISURF_HASCKEYSRCBLT &&
     DDRAWISURF_HASPIXELFORMAT_ == DDRAWISURF_HASPIXELFORMAT) ? 1 : -1];

typedef struct _PDEV {
    d3dpt_core core;            /* first: a PDEV *is* a d3dpt_core to the core */
    HANDLE hDriver;             /* the miniport, for EngDeviceIoControl */
    HDEV hdev;                  /* GDI's handle for this PDEV */
    HSURF hsurf;
    HPALETTE hpal;
    ULONG mode_index;           /* miniport mode index */
    ULONG hz;
    ULONG rmask, gmask, bmask;
    BOOL device_surface;        /* EngCreateDeviceSurface took: DirectDraw possible */
    ULONG pal[256];             /* 8 bpp: GDI's default palette (PALETTEENTRY form) */

    ULONG refusals;             /* pixel formats logged by DdCanCreateSurface */
    ULONG gamma_lines;          /* gamma ramps logged by DrvIcmSetDeviceGammaRamp */
    ULONG blt_lines;            /* the first DdBlt calls logged */
    ULONG flip_lines;           /* the first flips logged: the two buffers' handles and offsets */

    /* the hardware cursor (register set v4). Its image lives in the
     * D3DPT_FB_CURSOR_BYTES above the DirectDraw heap. */
    ULONG cursor_lines;         /* the first shapes logged */
} PDEV, *PPDEV;

/* The PDEV whose Direct3D is on (the primary display). The core keeps the
 * pointer, and the cast back is sound because the core is the PDEV's
 * first member. */
static PPDEV nt_d3d_pdev(void)
{
    return (PPDEV)d3d_core;
}

/* --------------------------------------------------------------- modes */

/* the miniport's mode list. The caller frees *out with EngFreeMem. */
static ULONG get_modes(HANDLE hDriver, PVIDEO_MODE_INFORMATION *out)
{
    VIDEO_NUM_MODES nm;
    DWORD ret;
    ULONG bytes;
    PVIDEO_MODE_INFORMATION modes;

    *out = NULL;
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_NUM_AVAIL_MODES, NULL, 0,
                           &nm, sizeof(nm), &ret) != 0 || nm.NumModes == 0 ||
        nm.ModeInformationLength != sizeof(VIDEO_MODE_INFORMATION)) {
        return 0;
    }
    bytes = nm.NumModes * nm.ModeInformationLength;
    modes = EngAllocMem(FL_ZERO_MEMORY, bytes, ALLOC_TAG);
    if (!modes) {
        return 0;
    }
    if (EngDeviceIoControl(hDriver, IOCTL_VIDEO_QUERY_AVAIL_MODES, NULL, 0,
                           modes, bytes, &ret) != 0) {
        EngFreeMem(modes);
        return 0;
    }
    *out = modes;
    return nm.NumModes;
}

static WCHAR device_name[] = L"d3dptdisp";

ULONG APIENTRY DrvGetModes(HANDLE hDriver, ULONG cjSize, DEVMODEW *pdm)
{
    PVIDEO_MODE_INFORMATION modes;
    ULONG n, i, bytes;

    n = get_modes(hDriver, &modes);
    if (n == 0) {
        return 0;
    }
    bytes = n * sizeof(DEVMODEW);
    if (pdm == NULL) {
        EngFreeMem(modes);
        return bytes;
    }
    if (cjSize < bytes) {
        n = cjSize / sizeof(DEVMODEW);
        bytes = n * sizeof(DEVMODEW);
    }
    for (i = 0; i < n; i++) {
        DEVMODEW *dm = &pdm[i];
        ULONG j;
        for (j = 0; j < sizeof(*dm) / 4; j++) {
            ((ULONG *)dm)[j] = 0;
        }
        for (j = 0; device_name[j]; j++) {
            dm->dmDeviceName[j] = device_name[j];
        }
        dm->dmSpecVersion = DM_SPECVERSION;
        dm->dmDriverVersion = DM_SPECVERSION;
        dm->dmSize = sizeof(DEVMODEW);
        dm->dmDriverExtra = 0;
        dm->dmBitsPerPel = modes[i].NumberOfPlanes * modes[i].BitsPerPlane;
        dm->dmPelsWidth = modes[i].VisScreenWidth;
        dm->dmPelsHeight = modes[i].VisScreenHeight;
        dm->dmDisplayFrequency = modes[i].Frequency;
        dm->dmDisplayFlags = 0;
        dm->dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                       DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    }
    EngFreeMem(modes);
    return bytes;
}

/* pick the miniport mode for a DEVMODE; zero fields mean "any" */
static BOOL pick_mode(PPDEV p, DEVMODEW *pdm)
{
    PVIDEO_MODE_INFORMATION modes, best = NULL;
    ULONG n, i;

    n = get_modes(p->hDriver, &modes);
    if (n == 0) {
        return FALSE;
    }
    for (i = 0; i < n; i++) {
        PVIDEO_MODE_INFORMATION m = &modes[i];
        if (pdm->dmPelsWidth && m->VisScreenWidth != pdm->dmPelsWidth) continue;
        if (pdm->dmPelsHeight && m->VisScreenHeight != pdm->dmPelsHeight) continue;
        if (pdm->dmBitsPerPel && m->BitsPerPlane * m->NumberOfPlanes != pdm->dmBitsPerPel) continue;
        if (pdm->dmDisplayFrequency && pdm->dmDisplayFrequency != 1 &&
            m->Frequency != pdm->dmDisplayFrequency) continue;
        best = m;
        break;
    }
    if (!best) {
        EngFreeMem(modes);
        return FALSE;
    }
    p->mode_index = best->ModeIndex;
    p->core.w = best->VisScreenWidth;
    p->core.h = best->VisScreenHeight;
    p->core.bpp = best->BitsPerPlane * best->NumberOfPlanes;
    p->core.pitch = best->ScreenStride;
    p->hz = best->Frequency;
    p->rmask = best->RedMask;
    p->gmask = best->GreenMask;
    p->bmask = best->BlueMask;
    EngFreeMem(modes);
    return TRUE;
}

/* ------------------------------------------------------------ the PDEV */

/* GDI's standard fonts and the sRGB-ish COLORINFO of the DDK's framebuf */
static const LOGFONTW font_system = {
    16, 7, 0, 0, FW_BOLD, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, VARIABLE_PITCH | FF_DONTCARE, L"System"
};
static const LOGFONTW font_ansi_var = {
    12, 9, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_STROKE_PRECIS, PROOF_QUALITY, VARIABLE_PITCH | FF_DONTCARE, L"MS Sans Serif"
};
static const LOGFONTW font_ansi_fix = {
    12, 9, 0, 0, FW_NORMAL, 0, 0, 0, ANSI_CHARSET, OUT_DEFAULT_PRECIS,
    CLIP_STROKE_PRECIS, PROOF_QUALITY, FIXED_PITCH | FF_DONTCARE, L"Courier"
};
static const COLORINFO color_info = {
    { 6700, 3300, 0 }, { 2100, 7100, 0 }, { 1400, 800, 0 },
    { 1750, 3950, 0 }, { 4050, 2050, 0 }, { 4400, 5200, 0 },
    { 3127, 3290, 0 },
    20000, 20000, 20000,
    0, 0, 0, 0, 0, 0
};

static ULONG bmf_of(PPDEV p)
{
    return p->core.bpp == 32 ? BMF_32BPP : p->core.bpp == 16 ? BMF_16BPP : BMF_8BPP;
}

/* the 20 Windows system colours (PALETTEENTRY form: red in the low byte) */
static const ULONG base_colors[20] = {
    0x000000, 0x000080, 0x008000, 0x008080, 0x800000, 0x800080, 0x808000, 0xc0c0c0,
    0xc0dcc0, 0xf0caa6,
    0xf0fbff, 0xa4a0a0, 0x808080, 0x0000ff, 0x00ff00, 0x00ffff, 0xff0000, 0xff00ff,
    0xffff00, 0xffffff,
};

/* GDI's default 8 bpp palette: system colours at 0-9 and 246-255, a 6x6x6
 * cube at 10-225, 20 greys at 226-245 */
static void build_palette(ULONG *pal)
{
    ULONG i, r, g, b;

    for (i = 0; i < 10; i++) {
        pal[i] = base_colors[i];
        pal[246 + i] = base_colors[10 + i];
    }
    i = 10;
    for (r = 0; r < 6; r++) {
        for (g = 0; g < 6; g++) {
            for (b = 0; b < 6; b++) {
                pal[i++] = (r * 51) | ((g * 51) << 8) | ((b * 51) << 16);
            }
        }
    }
    for (; i < 246; i++) {
        ULONG v = (i - 226) * 255 / 19;
        pal[i] = v | (v << 8) | (v << 16);
    }
}

/* Writes n PALETTEENTRY-form colours into the device's PALETTE registers
 * from entry start. The host applies them at its next refresh. Before the
 * register page is mapped, this goes through the miniport's IOCTL. */
static void set_clut(PPDEV p, ULONG start, ULONG n, const ULONG *rgb)
{
    ULONG i;

    if (start >= 256) {
        return;
    }
    if (n > 256 - start) {
        n = 256 - start;
    }
    if (p->core.regs) {
        for (i = 0; i < n; i++) {
            ULONG c = rgb[i];
            p->core.regs[(D3DPT_FB_REG_PALETTE + 4 * (start + i)) / 4] =
                ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
        }
    } else {
        UCHAR buf[4 + 256 * 4];
        PVIDEO_CLUT clut = (PVIDEO_CLUT)buf;
        DWORD ret;
        clut->NumEntries = (USHORT)n;
        clut->FirstEntry = (USHORT)start;
        for (i = 0; i < n; i++) {
            ULONG c = rgb[i];
            clut->LookupTable[i].RgbArray.Red = (UCHAR)c;
            clut->LookupTable[i].RgbArray.Green = (UCHAR)(c >> 8);
            clut->LookupTable[i].RgbArray.Blue = (UCHAR)(c >> 16);
            clut->LookupTable[i].RgbArray.Unused = 0;
        }
        EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_SET_COLOR_REGISTERS, clut, 4 + n * 4,
                           NULL, 0, &ret);
    }
}

/* Palette-managed 8 bpp. GDI calls this with its system palette at mode
 * set and whenever a palette is realized. */
BOOL APIENTRY DrvSetPalette(DHPDEV dhpdev, PALOBJ *ppalo, FLONG fl, ULONG iStart, ULONG cColors)
{
    PPDEV p = (PPDEV)dhpdev;
    ULONG colors[256];

    if (p->core.bpp != 8 || iStart >= 256) {
        return FALSE;
    }
    if (cColors > 256 - iStart) {
        cColors = 256 - iStart;
    }
    if (PALOBJ_cGetColors(ppalo, iStart, cColors, colors) != cColors) {
        return FALSE;
    }
    set_clut(p, iStart, cColors, colors);
    return TRUE;
}

/* GDI's gamma ramp into the adapter's GAMMA block (register set v5). On NT
 * DirectDraw's gamma control and Direct3D 8's SetGammaRamp also arrive
 * here. The driver writes the high byte of each of the three 256-word
 * ramps, then GAMMA_ENABLE, which makes the adapter take them. */
BOOL APIENTRY DrvIcmSetDeviceGammaRamp(DHPDEV dhpdev, ULONG iFormat, LPVOID lpRamp)
{
    PPDEV p = (PPDEV)dhpdev;
    const USHORT *r = (const USHORT *)lpRamp;
    ULONG i;

    if (iFormat != IGRF_RGB_256WORDS || !r || !p->core.regs || (ddflags(&p->core) & DDF_NO_GAMMA)) {
        return FALSE;
    }
    for (i = 0; i < D3DPT_FB_GAMMA_SIZE; i++) {
        p->core.regs[D3DPT_FB_REG_GAMMA / 4 + i] = ((ULONG)(r[i] >> 8) << 16) | ((ULONG)(r[256 + i] >> 8) << 8) |
                                                   (ULONG)(r[512 + i] >> 8);
    }
    p->core.regs[D3DPT_FB_REG_GAMMA_ENABLE / 4] = 1;
    if (p->gamma_lines < 8) {
        p->gamma_lines++;
        dbg_hex(&p->core, "d3dptdisp: gamma ramp, 128 -> ", p->core.regs[D3DPT_FB_REG_GAMMA / 4 + 128]);
        dbg_puts(&p->core, "\n");
    }
    return TRUE;
}

DHPDEV APIENTRY DrvEnablePDEV(DEVMODEW *pdm, LPWSTR pwszLogAddress, ULONG cPat,
                              HSURF *phsurfPatterns, ULONG cjCaps, ULONG *pdevcaps,
                              ULONG cjDevInfo, DEVINFO *pdi, HDEV hdev,
                              LPWSTR pwszDeviceName, HANDLE hDriver)
{
    PPDEV p;
    GDIINFO *gi = (GDIINFO *)pdevcaps;
    GDIINFO g;
    DEVINFO d;
    ULONG i;

    if (cjCaps < sizeof(GDIINFO) || cjDevInfo < sizeof(DEVINFO)) {
        return NULL;
    }
    p = EngAllocMem(FL_ZERO_MEMORY, sizeof(*p), ALLOC_TAG);
    if (!p) {
        return NULL;
    }
    p->hDriver = hDriver;
    if (!pick_mode(p, pdm)) {
        EngFreeMem(p);
        return NULL;
    }

    for (i = 0; i < sizeof(g) / 4; i++) ((ULONG *)&g)[i] = 0;
    for (i = 0; i < sizeof(d) / 4; i++) ((ULONG *)&d)[i] = 0;

    g.ulVersion = GDI_DRIVER_VERSION;
    g.ulTechnology = DT_RASDISPLAY;
    g.ulHorzSize = 320;
    g.ulVertSize = 240;
    g.ulHorzRes = p->core.w;
    g.ulVertRes = p->core.h;
    g.cBitsPixel = p->core.bpp;
    g.cPlanes = 1;
    g.ulNumColors = p->core.bpp == 8 ? 20 : (ULONG)-1;
    g.ulVRefresh = p->hz;
    g.ulBltAlignment = 1;
    g.ulLogPixelsX = pdm->dmLogPixels ? pdm->dmLogPixels : 96;
    g.ulLogPixelsY = g.ulLogPixelsX;
    g.flTextCaps = TC_RA_ABLE;
    if (p->core.bpp == 32) {
        g.ulDACRed = g.ulDACGreen = g.ulDACBlue = 8;
        g.ulHTOutputFormat = HT_FORMAT_32BPP;
    } else if (p->core.bpp == 16) {
        g.ulDACRed = 5; g.ulDACGreen = 6; g.ulDACBlue = 5;
        g.ulHTOutputFormat = HT_FORMAT_16BPP;
    } else {
        g.ulDACRed = g.ulDACGreen = g.ulDACBlue = 8;
        g.ulHTOutputFormat = HT_FORMAT_8BPP;
    }
    g.ulAspectX = 36;
    g.ulAspectY = 36;
    g.ulAspectXY = 51;
    g.xStyleStep = 1;
    g.yStyleStep = 1;
    g.denStyleStep = 3;
    g.ulNumPalReg = p->core.bpp == 8 ? 256 : 0;
    g.ciDevice = color_info;
    g.ulDevicePelsDPI = 0;
    g.ulPrimaryOrder = PRIMARY_ORDER_CBA;
    g.ulHTPatternSize = HT_PATSIZE_4x4_M;
    g.flHTFlags = HT_FLAG_ADDITIVE_PRIMS;
    g.ulPhysicalPixelCharacteristics = PPC_UNDEFINED;
    g.ulPhysicalPixelGamma = PPG_DEFAULT;
    *gi = g;

    d.flGraphicsCaps = GCAPS_ASYNCMOVE;     /* the hardware cursor moves at any time (v4) */
    d.lfDefaultFont = font_system;
    d.lfAnsiVarFont = font_ansi_var;
    d.lfAnsiFixFont = font_ansi_fix;
    d.cFonts = 0;
    d.iDitherFormat = bmf_of(p);
    d.cxDither = 0;
    d.cyDither = 0;
    if (p->core.bpp == 8) {
        /* Palette-managed. GDI owns the 256 entries and hands them to
         * DrvSetPalette. The default palette has the 20 system colours
         * where GDI expects them. */
        build_palette(p->pal);
        d.flGraphicsCaps = GCAPS_PALMANAGED | GCAPS_COLOR_DITHER | GCAPS_ASYNCMOVE;
        d.cxDither = d.cyDither = 8;
        d.hpalDefault = EngCreatePalette(PAL_INDEXED, 256, p->pal, 0, 0, 0);
    } else {
        d.hpalDefault = EngCreatePalette(PAL_BITFIELDS, 0, NULL, p->rmask, p->gmask, p->bmask);
    }
    if (!d.hpalDefault) {
        EngFreeMem(p);
        return NULL;
    }
    p->hpal = d.hpalDefault;
    d.flGraphicsCaps2 = GCAPS2_CHANGEGAMMARAMP;    /* DrvIcmSetDeviceGammaRamp: the adapter's GAMMA block (v5) */
    *pdi = d;

    return (DHPDEV)p;
}

VOID APIENTRY DrvCompletePDEV(DHPDEV dhpdev, HDEV hdev)
{
    ((PPDEV)dhpdev)->hdev = hdev;
}

VOID APIENTRY DrvDisablePDEV(DHPDEV dhpdev)
{
    PPDEV p = (PPDEV)dhpdev;

    if (p->hpal) {
        EngDeletePalette(p->hpal);
    }
    EngFreeMem(p);
}

/* ---------------------------------------------------------- the surface */

static BOOL set_mode(PPDEV p)
{
    VIDEO_MODE vm;
    DWORD ret;

    vm.RequestedMode = p->mode_index;
    return EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_SET_CURRENT_MODE, &vm, sizeof(vm),
                              NULL, 0, &ret) == 0;
}

static void map_regs(PPDEV p)
{
    VIDEO_PUBLIC_ACCESS_RANGES r;
    DWORD ret;

    if (p->core.regs) {
        return;
    }
    if (EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_QUERY_PUBLIC_ACCESS_RANGES, NULL, 0,
                           &r, sizeof(r), &ret) == 0 && r.VirtualAddress) {
        p->core.regs = r.VirtualAddress;
    }
}

static void unmap_regs(PPDEV p)
{
    VIDEO_MEMORY vm;
    DWORD ret;

    if (!p->core.regs) {
        return;
    }
    vm.RequestedVirtualAddress = (PVOID)p->core.regs;
    EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_FREE_PUBLIC_ACCESS_RANGES, &vm, sizeof(vm),
                       NULL, 0, &ret);
    p->core.regs = NULL;
}

HSURF APIENTRY DrvEnableSurface(DHPDEV dhpdev)
{
    PPDEV p = (PPDEV)dhpdev;
    VIDEO_MEMORY vm;
    VIDEO_MEMORY_INFORMATION vmi;
    DWORD ret;
    SIZEL sizl;
    HSURF hsurf;

    map_regs(p);
    dbg_puts(&p->core, "d3dptdisp: enable surface ");
    dbg_hex(&p->core, "mode ", p->mode_index);
    dbg_puts(&p->core, "\n");
    p->core.cmd_offset = (p->core.regs && (p->core.regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_D3D)) ?
                    p->core.regs[D3DPT_FB_REG_CMD_OFFSET / 4] : 0;

    if (!set_mode(p)) {
        dbg_puts(&p->core, "d3dptdisp: set mode failed\n");
        return NULL;
    }
    vm.RequestedVirtualAddress = NULL;
    if (EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_MAP_VIDEO_MEMORY, &vm, sizeof(vm),
                           &vmi, sizeof(vmi), &ret) != 0) {
        dbg_puts(&p->core, "d3dptdisp: map failed\n");
        return NULL;
    }
    /* VRAM from its start, and the screen FrameBufferBase - VideoRamBase
     * into it (Vista and later: above the boot screen's uncached pages,
     * d3dptvid.c). Every offset the device and DirectDraw see is VRAM's. */
    p->core.fb = vmi.VideoRamBase;
    p->core.fb_len = vmi.VideoRamLength;
    p->core.fb_base = (ULONG)((PUCHAR)vmi.FrameBufferBase - (PUCHAR)vmi.VideoRamBase);
    if (p->core.bpp == 8) {
        set_clut(p, 0, 256, p->pal);
    }

    sizl.cx = p->core.w;
    sizl.cy = p->core.h;
    /* A device surface (DirectDraw needs one) that GDI still draws on
     * itself, with EngModifySurface handing it the frame buffer bytes. The
     * hook / flag combinations win32k accepts are not documented
     * consistently, so try them in order and log which one took. An engine
     * bitmap is the last resort (the desktop works, DirectDraw does not). */
    hsurf = (p->core.regs && (p->core.regs[D3DPT_FB_REG_DDFLAGS / 4] & DDF_ENGINE_BITMAP)) ? NULL :
            EngCreateDeviceSurface((DHSURF)p, sizl, bmf_of(p));
    if (hsurf) {
        static const struct { FLONG hooks, surf; } variants[] = {
            { HOOK_SYNCHRONIZE, MS_NOTSYSTEMMEMORY },
            { 0, MS_NOTSYSTEMMEMORY },
            { HOOK_SYNCHRONIZE, 0 },
            { 0, 0 },
        };
        ULONG v;
        for (v = 0; v < sizeof(variants) / sizeof(variants[0]); v++) {
            if (EngModifySurface(hsurf, p->hdev, variants[v].hooks, variants[v].surf,
                                 (DHSURF)p, (PUCHAR)p->core.fb + p->core.fb_base, (LONG)p->core.pitch, NULL)) {
                dbg_hex(&p->core, "d3dptdisp: device surface, variant ", v);
                dbg_puts(&p->core, "\n");
                p->device_surface = TRUE;
                break;
            }
        }
        if (!p->device_surface) {
            dbg_puts(&p->core, "d3dptdisp: EngModifySurface refused every variant\n");
            EngDeleteSurface(hsurf);
            hsurf = NULL;
        }
    } else {
        dbg_puts(&p->core, "d3dptdisp: EngCreateDeviceSurface failed\n");
    }
    if (!hsurf) {
        hsurf = (HSURF)EngCreateBitmap(sizl, p->core.pitch, bmf_of(p),
                                       BMF_TOPDOWN | BMF_NOZEROINIT, (PUCHAR)p->core.fb + p->core.fb_base);
        if (!hsurf) {
            dbg_puts(&p->core, "d3dptdisp: EngCreateBitmap failed\n");
            goto unmap;
        }
        if (!EngAssociateSurface(hsurf, p->hdev, 0)) {
            dbg_puts(&p->core, "d3dptdisp: EngAssociateSurface failed\n");
            EngDeleteSurface(hsurf);
            goto unmap;
        }
        dbg_puts(&p->core, "d3dptdisp: engine bitmap surface (no DirectDraw)\n");
    }
    p->hsurf = hsurf;
    dbg_hex(&p->core, "d3dptdisp: surface ", (ULONG)(ULONG_PTR)p->core.fb + p->core.fb_base);
    dbg_hex(&p->core, " pitch ", p->core.pitch);
    dbg_puts(&p->core, "\n");
    return hsurf;

unmap:
    vm.RequestedVirtualAddress = p->core.fb;
    EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY, &vm, sizeof(vm), NULL, 0, &ret);
    p->core.fb = NULL;
    return NULL;
}

VOID APIENTRY DrvDisableSurface(DHPDEV dhpdev)
{
    PPDEV p = (PPDEV)dhpdev;
    VIDEO_MEMORY vm;
    DWORD ret;

    dbg_puts(&p->core, "d3dptdisp: disable surface\n");
    if (p->core.d3d) {
        p->core.d3d = FALSE;
        if (nt_d3d_pdev() == p) {
            d3d_core = NULL;
        }
    }
    if (p->hsurf) {
        EngDeleteSurface(p->hsurf);
        p->hsurf = NULL;
    }
    if (p->core.fb) {
        vm.RequestedVirtualAddress = p->core.fb;
        EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_UNMAP_VIDEO_MEMORY, &vm, sizeof(vm),
                           NULL, 0, &ret);
        p->core.fb = NULL;
    }
    unmap_regs(p);
}

/* GDI calls this before touching the surface when HOOK_SYNCHRONIZE is set.
 * There is nothing to wait for, since every access is a plain memory
 * write. */
VOID APIENTRY DrvSynchronizeSurface(SURFOBJ *pso, RECTL *prcl, FLONG fl)
{
}

BOOL APIENTRY DrvAssertMode(DHPDEV dhpdev, BOOL bEnable)
{
    PPDEV p = (PPDEV)dhpdev;
    DWORD ret;

    if (bEnable) {
        dbg_puts(&p->core, "d3dptdisp: assert mode on\n");
        /* A kept PDEV coming back (the desktop after a full-screen mode)
         * gets no DrvGetDirectDrawInfo, so its Direct3D is on again here:
         * the other PDEV's DrvDisableSurface left no d3d_core, and every
         * context call failed (Reset D3DERR_INVALIDCALL, finding 5) */
        if (p->core.d3d) {
            d3d_core = &p->core;
        }
        return set_mode(p);
    }
    dbg_puts(&p->core, "d3dptdisp: assert mode off\n");
    if (p->core.regs) {
        p->core.regs[D3DPT_FB_REG_CURSOR_ENABLE / 4] = 0;    /* no sprite over the VGA text */
    }
    /* Another PDEV (a full-screen console, the logon desktop switching)
     * takes the screen, so go back to VGA text through the miniport. */
    return EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_RESET_DEVICE, NULL, 0,
                              NULL, 0, &ret) == 0;
}

/* ------------------------------------------------------ hardware cursor
 * (register set v4, doc 15 "The hardware cursor"). GDI hands the pointer
 * as a 1 bpp mask surface (AND rows over XOR rows) and, for a colour
 * pointer, a colour surface with a translation to the screen format. The
 * driver converts it to a8r8g8b8 in the VRAM area above the DirectDraw
 * heap and tells the device, which passes it to the host as a cursor
 * sprite. Pointers larger than D3DPT_FB_CURSOR_MAX stay with GDI's
 * software pointer (SPS_DECLINE). */
#ifndef SPS_ALPHA
#define SPS_ALPHA 0x00000010
#endif

static void cursor_show(PPDEV p, LONG x, LONG y)
{
    if (!p->core.regs) {
        return;
    }
    if (x == -1) {
        p->core.regs[D3DPT_FB_REG_CURSOR_ENABLE / 4] = 0;
        return;
    }
    p->core.regs[D3DPT_FB_REG_CURSOR_X / 4] = (ULONG)x;
    p->core.regs[D3DPT_FB_REG_CURSOR_Y / 4] = (ULONG)y;
    p->core.regs[D3DPT_FB_REG_CURSOR_ENABLE / 4] = 1;
}

static ULONG mask_bit(const SURFOBJ *so, ULONG row, ULONG x)
{
    const UCHAR *b = (const UCHAR *)so->pvScan0 + (LONG)row * so->lDelta;
    return (b[x >> 3] >> (7 - (x & 7))) & 1;
}

ULONG APIENTRY DrvSetPointerShape(SURFOBJ *pso, SURFOBJ *psoMask, SURFOBJ *psoColor, XLATEOBJ *pxlo,
                                  LONG xHot, LONG yHot, LONG x, LONG y, RECTL *prcl, FLONG fl)
{
    PPDEV p = (PPDEV)pso->dhpdev;
    ULONG w, h, i, j;
    ULONG *img;
    BOOL alpha = (fl & SPS_ALPHA) != 0;

    if (!p->core.regs || !p->core.fb || !(p->core.regs[D3DPT_FB_REG_CAPS / 4] & D3DPT_FB_CAP_CURSOR)) {
        return SPS_DECLINE;
    }
    if (!psoMask && !psoColor) {
        /* no shape: hide the pointer */
        cursor_show(p, -1, 0);
        return SPS_ACCEPT_NOEXCLUDE;
    }
    if (psoMask) {
        w = psoMask->sizlBitmap.cx;
        h = psoMask->sizlBitmap.cy / 2;
        if (psoMask->iBitmapFormat != BMF_1BPP) {
            return SPS_DECLINE;
        }
    } else {
        w = psoColor->sizlBitmap.cx;
        h = psoColor->sizlBitmap.cy;
    }
    if (!w || !h || w > D3DPT_FB_CURSOR_MAX || h > D3DPT_FB_CURSOR_MAX ||
        xHot < 0 || yHot < 0 || (ULONG)xHot >= w || (ULONG)yHot >= h ||
        (psoColor && ((ULONG)psoColor->sizlBitmap.cx < w || (ULONG)psoColor->sizlBitmap.cy < h))) {
        return SPS_DECLINE;
    }
    img = (ULONG *)((UCHAR *)p->core.fb + cursor_offset(&p->core));

    if (psoColor) {
        /* The colour pointer as 32 bpp. Copy it directly when it already
         * is, otherwise go through a 32 bpp engine bitmap and the
         * translation. */
        if (psoColor->iBitmapFormat == BMF_32BPP) {
            for (j = 0; j < h; j++) {
                const ULONG *row = (const ULONG *)((const UCHAR *)psoColor->pvScan0 + (LONG)j * psoColor->lDelta);
                for (i = 0; i < w; i++) {
                    img[j * w + i] = alpha ? row[i] : (row[i] | 0xff000000u);
                }
            }
        } else {
            SIZEL sz;
            HBITMAP hb;
            SURFOBJ *so;
            RECTL r;
            POINTL pt;

            sz.cx = w;
            sz.cy = h;
            hb = EngCreateBitmap(sz, w * 4, BMF_32BPP, BMF_TOPDOWN, NULL);
            so = hb ? EngLockSurface((HSURF)hb) : NULL;
            if (!so) {
                if (hb) EngDeleteSurface((HSURF)hb);
                return SPS_DECLINE;
            }
            r.left = 0; r.top = 0; r.right = w; r.bottom = h;
            pt.x = 0; pt.y = 0;
            EngCopyBits(so, psoColor, NULL, pxlo, &r, &pt);
            for (j = 0; j < h; j++) {
                const ULONG *row = (const ULONG *)((const UCHAR *)so->pvScan0 + (LONG)j * so->lDelta);
                for (i = 0; i < w; i++) {
                    img[j * w + i] = row[i] | 0xff000000u;
                }
            }
            EngUnlockSurface(so);
            EngDeleteSurface((HSURF)hb);
            alpha = FALSE;
        }
        if (psoMask && !alpha) {
            /* the AND mask: 1 = the screen shows through */
            for (j = 0; j < h; j++) {
                for (i = 0; i < w; i++) {
                    if (mask_bit(psoMask, j, i)) {
                        img[j * w + i] &= 0x00ffffffu;
                    }
                }
            }
        }
    } else {
        /* A monochrome pointer. AND 1 / XOR 0 is transparent, AND 0 is
         * black or white by XOR. AND 1 / XOR 1 (invert the screen) becomes
         * black, because a sprite cannot invert. */
        for (j = 0; j < h; j++) {
            for (i = 0; i < w; i++) {
                ULONG a = mask_bit(psoMask, j, i), xr = mask_bit(psoMask, h + j, i);
                img[j * w + i] = (a && !xr) ? 0 : xr && !a ? 0xffffffffu : 0xff000000u;
            }
        }
    }

    p->core.regs[D3DPT_FB_REG_CURSOR_ADDR / 4] = cursor_offset(&p->core);
    p->core.regs[D3DPT_FB_REG_CURSOR_W / 4] = w;
    p->core.regs[D3DPT_FB_REG_CURSOR_H / 4] = h;
    p->core.regs[D3DPT_FB_REG_CURSOR_HOT_X / 4] = (ULONG)xHot;
    p->core.regs[D3DPT_FB_REG_CURSOR_HOT_Y / 4] = (ULONG)yHot;
    p->core.regs[D3DPT_FB_REG_CURSOR_DEFINE / 4] = 1;
    cursor_show(p, x, y);
    if (p->cursor_lines < 8) {
        p->cursor_lines++;
        dbg_hex(&p->core, "d3dptdisp: pointer ", w);
        dbg_hex(&p->core, "x", h);
        dbg_puts(&p->core, psoColor ? " colour" : " mono");
        dbg_hex(&p->core, " hot ", (ULONG)xHot);
        dbg_hex(&p->core, ",", (ULONG)yHot);
        dbg_hex(&p->core, " flags ", fl);
        dbg_puts(&p->core, "\n");
    }
    return SPS_ACCEPT_NOEXCLUDE;
}

VOID APIENTRY DrvMovePointer(SURFOBJ *pso, LONG x, LONG y, RECTL *prcl)
{
    cursor_show((PPDEV)pso->dhpdev, x, y);
}

/* ------------------------------------------------------------ DirectDraw
 * dxg.sys drives these (ddrawint.h, the NT DirectDraw DDI). Surfaces come
 * out of one linear heap in VRAM behind the primary. The runtime does the
 * allocation and the HEL blits. The driver does memory mapping, flips and
 * vblank. */

/* Forward declarations, because dxg's tables are built before these
 * functions are defined. */
static DWORD APIENTRY DdSetColorKey(PDD_SETCOLORKEYDATA d);
static DWORD APIENTRY DdBlt(PDD_BLTDATA d);
static DWORD APIENTRY DdCreateSurface(PDD_CREATESURFACEDATA d);
static DWORD APIENTRY DdCreateSurfaceEx(PDD_CREATESURFACEEXDATA d);
static DWORD APIENTRY DdGetDriverState(PDD_GETDRIVERSTATEDATA d);
static DWORD APIENTRY DdDestroySurface(PDD_DESTROYSURFACEDATA d);
static DWORD APIENTRY DdLock(PDD_LOCKDATA d);
static DWORD APIENTRY DdUnlock(PDD_UNLOCKDATA d);
static DWORD APIENTRY D3dClear2(LPD3DNTHAL_CLEAR2DATA d);
static DWORD APIENTRY D3dValidateTextureStageState(LPD3DNTHAL_VALIDATETEXTURESTAGESTATEDATA d);
static DWORD APIENTRY D3dDrawPrimitives2(LPD3DNTHAL_DRAWPRIMITIVES2DATA d);
static DWORD APIENTRY D3dSetRenderTarget(LPD3DNTHAL_SETRENDERTARGETDATA d);
static DWORD APIENTRY D3dCanCreateD3DBuffer(PDD_CANCREATESURFACEDATA d);
static DWORD APIENTRY D3dCreateD3DBuffer(PDD_CREATESURFACEDATA d);
static DWORD APIENTRY D3dDestroyD3DBuffer(PDD_DESTROYSURFACEDATA d);
static DWORD APIENTRY D3dLockD3DBuffer(PDD_LOCKDATA d);
static DWORD APIENTRY D3dUnlockD3DBuffer(PDD_UNLOCKDATA d);
static DWORD APIENTRY D3dContextCreate(LPD3DNTHAL_CONTEXTCREATEDATA d);
static DWORD APIENTRY D3dContextDestroy(LPD3DNTHAL_CONTEXTDESTROYDATA d);
static DWORD APIENTRY D3dContextDestroyAll(LPD3DNTHAL_CONTEXTDESTROYALLDATA d);
static DWORD APIENTRY D3dSceneCapture(LPD3DNTHAL_SCENECAPTUREDATA d);
static ULONG surf_handle(PDD_SURFACE_LOCAL s);
static void nt_register(PPDEV p, PDD_SURFACE_LOCAL s);

/* The callback tables dxg is handed. They point at this file's functions,
 * so they belong to the NT layer. The caps they claim are core_caps.c's. */
static D3DNTHAL_CALLBACKS d3d_callbacks;
static DD_D3DBUFCALLBACKS d3d_bufcallbacks;

static void d3d_callbacks_init(void)
{
    ULONG i;

    for (i = 0; i < sizeof(d3d_callbacks) / 4; i++) ((ULONG *)&d3d_callbacks)[i] = 0;
    d3d_callbacks.dwSize = sizeof(d3d_callbacks);
    d3d_callbacks.ContextCreate = D3dContextCreate;
    d3d_callbacks.ContextDestroy = D3dContextDestroy;
    d3d_callbacks.ContextDestroyAll = D3dContextDestroyAll;
    d3d_callbacks.SceneCapture = D3dSceneCapture;

    /* DrawPrimitives2's command / vertex buffers. dxg allocates them in
     * system memory once the driver says so (NOTHANDLED + the caps). */
    for (i = 0; i < sizeof(d3d_bufcallbacks) / 4; i++) ((ULONG *)&d3d_bufcallbacks)[i] = 0;
    d3d_bufcallbacks.dwSize = sizeof(d3d_bufcallbacks);
    d3d_bufcallbacks.dwFlags = DDHAL_D3DBUFCB32_CANCREATED3DBUF | DDHAL_D3DBUFCB32_CREATED3DBUF |
                               DDHAL_D3DBUFCB32_DESTROYD3DBUF | DDHAL_D3DBUFCB32_LOCKD3DBUF | DDHAL_D3DBUFCB32_UNLOCKD3DBUF;
    d3d_bufcallbacks.CanCreateD3DBuffer = D3dCanCreateD3DBuffer;
    d3d_bufcallbacks.CreateD3DBuffer = D3dCreateD3DBuffer;
    d3d_bufcallbacks.DestroyD3DBuffer = D3dDestroyD3DBuffer;
    d3d_bufcallbacks.LockD3DBuffer = D3dLockD3DBuffer;
    d3d_bufcallbacks.UnlockD3DBuffer = D3dUnlockD3DBuffer;
}


static DWORD APIENTRY DdMapMemory(PDD_MAPMEMORYDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    VIDEO_SHARE_MEMORY sh;
    VIDEO_SHARE_MEMORY_INFORMATION info;
    DWORD ret;

    sh.ProcessHandle = d->hProcess;
    sh.ViewOffset = 0;
    sh.ViewSize = p->core.fb_len;
    if (d->bMap) {
        sh.RequestedVirtualAddress = NULL;
        if (EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_SHARE_VIDEO_MEMORY, &sh, sizeof(sh),
                               &info, sizeof(info), &ret) != 0) {
            dbg_puts(&p->core, "d3dptdisp: share video memory failed\n");
            d->ddRVal = DDERR_GENERIC;
            return DDHAL_DRIVER_HANDLED;
        }
        d->fpProcess = (FLATPTR)info.VirtualAddress;
        dbg_hex(&p->core, "d3dptdisp: dd map ", (ULONG)d->fpProcess);
        dbg_puts(&p->core, "\n");
    } else {
        sh.RequestedVirtualAddress = (PVOID)d->fpProcess;
        EngDeviceIoControl(p->hDriver, IOCTL_VIDEO_UNSHARE_VIDEO_MEMORY, &sh, sizeof(sh),
                           NULL, 0, &ret);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdWaitForVerticalBlank(PDD_WAITFORVERTICALBLANKDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    switch (d->dwFlags) {
    case DDWAITVB_I_TESTVB:
        d->bIsInVB = vb_test(&p->core);
        d->ddRVal = DD_OK;
        break;
    case DDWAITVB_BLOCKBEGIN:
    case DDWAITVB_BLOCKEND:
        wait_frame(&p->core);
        d->ddRVal = DD_OK;
        break;
    default:
        d->ddRVal = DDERR_UNSUPPORTED;
        break;
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdCanCreateSurface(PDD_CANCREATESURFACEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (p->core.reg_lines < 4096 && d->lpDDSurfaceDesc && (d->lpDDSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER)) {
        p->core.reg_lines++;
        dbg_hex(&p->core, "d3dptdisp: can create buffer, caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
        dbg_hex(&p->core, " flags ", d->lpDDSurfaceDesc->dwFlags);
        dbg_hex(&p->core, " width ", d->lpDDSurfaceDesc->dwWidth);
        dbg_hex(&p->core, " height ", d->lpDDSurfaceDesc->dwHeight);
        dbg_hex(&p->core, " linear ", d->lpDDSurfaceDesc->dwLinearSize);
        dbg_hex(&p->core, " different pf ", d->bIsDifferentPixelFormat);
        dbg_puts(&p->core, "\n");
    }
    /* Always the display format. With Direct3D, also the texture and Z
     * formats the host mirrors (pf_format). */
    if (!d->bIsDifferentPixelFormat) {
        d->ddRVal = DD_OK;
    } else if (p->core.d3d && pf_format(&d->lpDDSurfaceDesc->ddpfPixelFormat) != 0) {
        d->ddRVal = DD_OK;
    } else {
        /* A refused surface is why a game falls back to its software
         * renderer, so log which format was refused. Only the first few,
         * because a game that keeps asking would flood the log. */
        if (p->refusals < 8) {
            const DDPIXELFORMAT *f = &d->lpDDSurfaceDesc->ddpfPixelFormat;
            p->refusals++;
            dbg_hex(&p->core, "d3dptdisp: refused pixel format, flags ", f->dwFlags);
            dbg_hex(&p->core, " fourcc ", f->dwFourCC);
            dbg_hex(&p->core, " bits ", f->dwRGBBitCount);
            dbg_hex(&p->core, " r ", f->dwRBitMask);
            dbg_hex(&p->core, " g ", f->dwGBitMask);
            dbg_hex(&p->core, " b ", f->dwBBitMask);
            dbg_hex(&p->core, " a ", f->dwRGBAlphaBitMask);
            dbg_hex(&p->core, " caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
            dbg_puts(&p->core, "\n");
        }
        d->ddRVal = DDERR_INVALIDPIXELFORMAT;
        if (p->core.reg_lines < 4096) {
            p->core.reg_lines++;
            dbg_hex(&p->core, "d3dptdisp: cannot create surface, pixel format flags ", d->lpDDSurfaceDesc->ddpfPixelFormat.dwFlags);
            dbg_hex(&p->core, " fourcc ", d->lpDDSurfaceDesc->ddpfPixelFormat.dwFourCC);
            dbg_hex(&p->core, " bits ", d->lpDDSurfaceDesc->ddpfPixelFormat.dwRGBBitCount);
            dbg_hex(&p->core, " caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
            dbg_puts(&p->core, "\n");
        }
    }
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdFlip(PDD_FLIPDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (!p->core.regs) {
        d->ddRVal = DDERR_UNSUPPORTED;
        return DDHAL_DRIVER_HANDLED;
    }
    /* The previous flip is still on its way to the screen. This is where a
     * double-buffered game waits for the refresh, as on a real card.
     * Nothing may change state before this point, because without
     * DDFLIP_WAIT the runtime hands DDERR_WASSTILLDRAWING straight to the
     * game. */
    if (!flip_done(&p->core)) {
        if (!(d->dwFlags & DDFLIP_WAIT)) {
            d->ddRVal = DDERR_WASSTILLDRAWING;
            return DDHAL_DRIVER_HANDLED;
        }
        wait_frame(&p->core);
        p->core.flip_pending = FALSE;
    }
    if (p->flip_lines < 8 && d->lpSurfCurr && d->lpSurfCurr->lpGbl && d->lpSurfTarg->lpGbl) {
        /* Which object is where. Under dxg's model the offsets never
         * change and the runtime's render target handle alternates. A
         * runtime that swaps memory shows the same handle at alternating
         * offsets. */
        p->flip_lines++;
        dbg_hex(&p->core, "d3dptdisp: flip curr ", surf_handle(d->lpSurfCurr));
        dbg_hex(&p->core, " at ", (ULONG)d->lpSurfCurr->lpGbl->fpVidMem);
        dbg_hex(&p->core, " targ ", surf_handle(d->lpSurfTarg));
        dbg_hex(&p->core, " at ", (ULONG)d->lpSurfTarg->lpGbl->fpVidMem);
        dbg_puts(&p->core, "\n");
    }
    if (d3d_ctx_live) {
        /* What Direct3D rendered into the back buffer must be in the VRAM
         * the target has *now* before it is scanned out. A surface the host
         * knows at another offset is registered again first (a no-op under
         * dxg's model below). */
        d3d_register_moved(&p->core, d->lpSurfCurr);
        d3d_register_moved(&p->core, d->lpSurfTarg);
        d3d_readback(&p->core, surf_handle(d->lpSurfTarg));
    }
    /* The page flip scans out from the target's VRAM offset and swaps
     * nothing. On NT dxg does not exchange the two surfaces' memory. It
     * exchanges their roles (the PRIMARYSURFACE caps move, each handle
     * keeps its VRAM, the application's "back buffer" is the other object
     * from now on) and tells the driver with a CreateSurfaceEx pair.
     * Re-registering the target at the current surface's offset here (and
     * vice versa) put the host's render target in the displayed buffer
     * every other frame (CKTEST). */
    p->core.regs[D3DPT_FB_REG_OFFSET / 4] = (ULONG)d->lpSurfTarg->lpGbl->fpVidMem;
    flip_issued(&p->core);
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdGetBltStatus(PDD_GETBLTSTATUSDATA d)
{
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdGetFlipStatus(PDD_GETFLIPSTATUSDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    /* DDGFS_CANFLIP and DDGFS_ISFLIPDONE both ask whether the last flip is
     * on the screen. The runtime spins here for DDFLIP_WAIT and passes
     * DDERR_WASSTILLDRAWING to the game without it. */
    d->ddRVal = (p->core.regs && !flip_done(&p->core)) ? DDERR_WASSTILLDRAWING : DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdSetExclusiveMode(PDD_SETEXCLUSIVEMODEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    dbg_puts(&p->core, d->dwEnterExcl ? "d3dptdisp: dd exclusive on\n" : "d3dptdisp: dd exclusive off\n");
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdFlipToGDISurface(PDD_FLIPTOGDISURFACEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (d->dwToGDI && p->core.regs) {
        p->core.regs[D3DPT_FB_REG_OFFSET / 4] = p->core.fb_base;
        p->core.flip_pending = FALSE;
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static BOOL guid_eq(const GUID *a, const GUID *b)
{
    const ULONG *x = (const ULONG *)a, *y = (const ULONG *)b;
    return x[0] == y[0] && x[1] == y[1] && x[2] == y[2] && x[3] == y[3];
}

/* GUID_NTCallbacks of ddrawint.h, spelled out because INITGUID would
 * define every GUID of ddrawint.h and d3dnthal.h twice */
static const GUID guid_ntcallbacks = {
    0x6fe9ecde, 0xdf89, 0x11d1, { 0x9d, 0xb0, 0x00, 0x60, 0x08, 0x27, 0x71, 0xba }
};

static const GUID guid_d3dcallbacks2 = {
    0x0ba584e1, 0x70b6, 0x11d0, { 0x88, 0x9d, 0x00, 0xaa, 0x00, 0xbb, 0xb7, 0x6a }
};
static const GUID guid_d3dcallbacks3 = {
    0xddf41230, 0xec0a, 0x11d0, { 0xa9, 0xb6, 0x00, 0xaa, 0x00, 0xc0, 0x99, 0x3e }
};
static const GUID guid_d3dextendedcaps = {
    0x7de41f80, 0x9d93, 0x11d0, { 0x89, 0xab, 0x00, 0xa0, 0xc9, 0x05, 0x41, 0x29 }
};
static const GUID guid_zpixelformats = {
    0x93869880, 0x36cf, 0x11d1, { 0x9b, 0x1b, 0x00, 0xaa, 0x00, 0xbb, 0xb8, 0xae }
};
static const GUID guid_misc2callbacks = {
    0x406b2f00, 0x3e5a, 0x11d1, { 0xb6, 0x40, 0x00, 0xaa, 0x00, 0xa1, 0xf9, 0x6a }
};
static const GUID guid_parseunknown = {
    0x2e04ffa0, 0x98e4, 0x11d1, { 0x8c, 0xe1, 0x00, 0xa0, 0xc9, 0x06, 0x29, 0xa8 }
};
/* GUID_DDStereoMode doubles as GUID_GetDriverInfo2 (DX8 DDI) when the data
 * carries the D3DGDI2 magic. A real stereo query is refused. */
static const GUID guid_stereomode = {
    0xf828169c, 0xa8e8, 0x11d2, { 0xa1, 0xf2, 0x00, 0xa0, 0xc9, 0x83, 0xea, 0xf6 }
};

/* The runtime's GetDriverInfo2 queries: the core answers (core_caps.c) */
static void gdi2_answer(PPDEV p, PDD_GETDRIVERINFODATA d)
{
    ULONG actual;

    d->ddRVal = core_gdi2_answer(&p->core, d->lpvData, &actual);
    d->dwActualSize = actual;
}

static void info_copy(PDD_GETDRIVERINFODATA d, const void *src, ULONG n)
{
    ULONG i;
    if (n > d->dwExpectedSize) n = d->dwExpectedSize;
    for (i = 0; i < n; i++) ((UCHAR *)d->lpvData)[i] = ((const UCHAR *)src)[i];
    d->dwActualSize = n;
    d->ddRVal = DD_OK;
}

static DWORD APIENTRY DdGetDriverInfo(PDD_GETDRIVERINFODATA d)
{
    PPDEV p = (PPDEV)d->dhpdev;

    dbg_hex(&p->core, "d3dptdisp: dd getinfo ", d->guidInfo.Data1);
    dbg_hex(&p->core, " expected ", d->dwExpectedSize);
    dbg_puts(&p->core, "\n");
    if (guid_eq(&d->guidInfo, &guid_ntcallbacks)) {
        DD_NTCALLBACKS nt;
        ULONG i;
        for (i = 0; i < sizeof(nt) / 4; i++) ((ULONG *)&nt)[i] = 0;
        nt.dwSize = sizeof(nt);
        nt.dwFlags = DDHAL_NTCB32_SETEXCLUSIVEMODE | DDHAL_NTCB32_FLIPTOGDISURFACE;
        nt.SetExclusiveMode = DdSetExclusiveMode;
        nt.FlipToGDISurface = DdFlipToGDISurface;
        info_copy(d, &nt, sizeof(nt));
    } else if (p->core.d3d && (ddflags(&p->core) & DDF_NO_D3D_INFO)) {
        d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    } else if (p->core.d3d && !(ddflags(&p->core) & DDF_NO_D3D_CB3) && guid_eq(&d->guidInfo, &guid_d3dcallbacks3)) {
        D3DNTHAL_CALLBACKS3 cb;
        ULONG i;
        for (i = 0; i < sizeof(cb) / 4; i++) ((ULONG *)&cb)[i] = 0;
        cb.dwSize = sizeof(cb);
        cb.dwFlags = D3DNTHAL3_CB32_CLEAR2 | D3DNTHAL3_CB32_VALIDATETEXTURESTAGESTATE | D3DNTHAL3_CB32_DRAWPRIMITIVES2;
        cb.Clear2 = D3dClear2;
        cb.ValidateTextureStageState = D3dValidateTextureStageState;
        cb.DrawPrimitives2 = D3dDrawPrimitives2;
        info_copy(d, &cb, sizeof(cb));
    } else if (p->core.d3d && guid_eq(&d->guidInfo, &guid_d3dcallbacks2)) {
        D3DNTHAL_CALLBACKS2 cb;
        ULONG i;
        for (i = 0; i < sizeof(cb) / 4; i++) ((ULONG *)&cb)[i] = 0;
        cb.dwSize = sizeof(cb);
        cb.dwFlags = D3DNTHAL2_CB32_SETRENDERTARGET;
        cb.SetRenderTarget = D3dSetRenderTarget;
        info_copy(d, &cb, sizeof(cb));
    } else if (p->core.d3d && guid_eq(&d->guidInfo, &guid_d3dextendedcaps)) {
        info_copy(d, &d3d_extcaps, sizeof(d3d_extcaps));
    } else if (p->core.d3d && guid_eq(&d->guidInfo, &guid_zpixelformats)) {
        info_copy(d, &d3d_zformats, sizeof(d3d_zformats));
    } else if (p->core.d3d && !(ddflags(&p->core) & DDF_NO_MISC2) && guid_eq(&d->guidInfo, &guid_misc2callbacks)) {
        DD_MISCELLANEOUS2CALLBACKS cb;
        ULONG i;
        for (i = 0; i < sizeof(cb) / 4; i++) ((ULONG *)&cb)[i] = 0;
        cb.dwSize = sizeof(cb);
        /* GetDriverState is not optional. ddraw.dll drops the whole HAL
         * (DDCAPS_NOHARDWARE) when a device with DRAWPRIMITIVES2EX or T&L
         * caps answers Miscellaneous2Callbacks without it (found by
         * bisection and ddraw disassembly). */
        cb.dwFlags = DDHAL_MISC2CB32_CREATESURFACEEX | DDHAL_MISC2CB32_GETDRIVERSTATE;
        cb.CreateSurfaceEx = DdCreateSurfaceEx;
        cb.GetDriverState = DdGetDriverState;
        info_copy(d, &cb, sizeof(cb));
    } else if (p->core.d3d && !(ddflags(&p->core) & DDF_NO_PARSEUNKNOWN) && guid_eq(&d->guidInfo, &guid_parseunknown)) {
        /* The runtime hands over its parser (lpvData is the function
         * itself, dwExpectedSize 0). walk uses it to skip the legacy
         * execute-buffer opcodes the DX3 path leaves in a DrawPrimitives2
         * stream. */
        p->core.parse_unknown = (HRESULT (APIENTRY *)(PVOID, PVOID *))d->lpvData;
        d->dwActualSize = d->dwExpectedSize;
        d->ddRVal = DD_OK;
    } else if (p->core.d3d && !(ddflags(&p->core) & DDF_NO_DX8) && guid_eq(&d->guidInfo, &guid_stereomode) &&
               d->lpvData && d->dwExpectedSize >= sizeof(DD_GETDRIVERINFO2DATA_) &&
               ((DD_GETDRIVERINFO2DATA_ *)d->lpvData)->dwMagic == D3DGDI2_MAGIC_) {
        /* The DX8 DDI. Without this answer d3d8.dll treats the driver as
         * DirectX 7 (software vertex processing, the DX7 token set). */
        gdi2_answer(p, d);
    } else {
        d->ddRVal = DDERR_CURRENTLYNOTAVAIL;
    }
    return DDHAL_DRIVER_HANDLED;
}

BOOL APIENTRY DrvGetDirectDrawInfo(DHPDEV dhpdev, DD_HALINFO *pHalInfo, DWORD *pdwNumHeaps,
                                   VIDEOMEMORY *pvmList, DWORD *pdwNumFourCCCodes, DWORD *pdwFourCC)
{
    PPDEV p = (PPDEV)dhpdev;
    ULONG i, start = heap_start(&p->core);

    dbg_hex(&p->core, "d3dptdisp: dd info call, lists ", pvmList ? 1 : 0);
    dbg_hex(&p->core, " halinfo ", sizeof(*pHalInfo));
    dbg_hex(&p->core, " corecaps ", sizeof(DDNTCORECAPS));
    dbg_hex(&p->core, " cb ", sizeof(DD_CALLBACKS));
    dbg_hex(&p->core, " scb ", sizeof(DD_SURFACECALLBACKS));
    dbg_puts(&p->core, "\n");
    if (!p->core.fb || start >= heap_end(&p->core)) {
        return FALSE;
    }
    if (!p->device_surface && !(ddflags(&p->core) & DDF_ENGINE_BITMAP)) {
        return FALSE;
    }
    if (p->core.cmd_offset + D3DPT_SHM_SIZE > p->core.fb_len) {
        p->core.cmd_offset = 0;
    }
    d3d_callbacks_init();       /* the NT layer's half: which functions dxg calls */
    p->core.gamma = !(ddflags(&p->core) & DDF_NO_GAMMA);   /* DrvIcmSetDeviceGammaRamp (v5): D3D8's FULLSCREENGAMMA */
    p->core.dx9 = TRUE;                                     /* the DirectX 9 face (M16): d3d9.dll's GETDDIVERSION / GETD3DCAPS9 */
    d3d_init(&p->core);
    *pdwNumHeaps = 1;
    /* The FOURCC surfaces DirectDraw may create at all. It checks this list
     * before the pixel-format callbacks. The list holds the compressed
     * textures and the DX8 and DX9 formats with no DDPIXELFORMAT, which
     * the runtime creates as a FOURCC of its D3DFORMAT. The first call
     * asks for the count, the second for the codes. */
    *pdwNumFourCCCodes = p->core.d3d ? 15 : 0;
    if (pdwFourCC && p->core.d3d) {
        pdwFourCC[0] = 0x31545844;      /* 'DXT1' (FOURCC_ is defined further down) */
        pdwFourCC[1] = 0x33545844;      /* 'DXT3' */
        pdwFourCC[2] = 0x35545844;      /* 'DXT5' */
        pdwFourCC[3] = 0x32545844;      /* 'DXT2' (DXT3 with premultiplied alpha: the host takes it as it is) */
        pdwFourCC[4] = 0x34545844;      /* 'DXT4' (DXT5's) */
        pdwFourCC[5] = 63;              /* D3DFMT_Q8W8V8U8 (core_caps.c) */
        /* d3d9.dll's (M16 step 3): A16B16G16R16, A2W10V10U10, Q16W16V16U16
         * and the six float formats */
        pdwFourCC[6] = 36;
        pdwFourCC[7] = 67;
        for (i = 0; i < 7; i++) pdwFourCC[8 + i] = 110 + i;
    }

    for (i = 0; i < sizeof(*pHalInfo) / 4; i++) ((ULONG *)pHalInfo)[i] = 0;
    pHalInfo->dwSize = sizeof(*pHalInfo);
    pHalInfo->vmiData.fpPrimary = p->core.fb_base;
    pHalInfo->vmiData.dwDisplayWidth = p->core.w;
    pHalInfo->vmiData.dwDisplayHeight = p->core.h;
    pHalInfo->vmiData.lDisplayPitch = (LONG)p->core.pitch;
    pHalInfo->vmiData.ddpfDisplay.dwSize = sizeof(DDPIXELFORMAT);
    pHalInfo->vmiData.ddpfDisplay.dwFlags = p->core.bpp == 8 ? DDPF_RGB | DDPF_PALETTEINDEXED8 : DDPF_RGB;
    pHalInfo->vmiData.ddpfDisplay.dwRGBBitCount = p->core.bpp;
    pHalInfo->vmiData.ddpfDisplay.dwRBitMask = p->rmask;
    pHalInfo->vmiData.ddpfDisplay.dwGBitMask = p->gmask;
    pHalInfo->vmiData.ddpfDisplay.dwBBitMask = p->bmask;
    pHalInfo->vmiData.dwOffscreenAlign = 32;
    pHalInfo->vmiData.dwOverlayAlign = 32;
    pHalInfo->vmiData.dwTextureAlign = 32;
    pHalInfo->vmiData.dwZBufferAlign = 32;
    pHalInfo->vmiData.dwAlphaAlign = 32;
    pHalInfo->vmiData.pvPrimary = (PUCHAR)p->core.fb + p->core.fb_base;

    pHalInfo->ddCaps.dwSize = sizeof(DDNTCORECAPS);
    /* The caps dxg accepts, found by bisection (doc 15 "dxg's caps
     * rules"). No blit caps (DirectDraw's HEL blits on the mapped VRAM),
     * wide surfaces, primary, offscreen and flip chains. DDCAPS_GDI in
     * dwCaps makes dxg drop the HAL altogether (NOHARDWARE, system-memory
     * surfaces). */
    pHalInfo->ddCaps.dwCaps = 0;
    pHalInfo->ddCaps.dwCaps2 = DDCAPS2_WIDESURFACES | (p->core.gamma ? DDCAPS2_PRIMARYGAMMA : 0);   /* gamma control: GDI's ramp */
    pHalInfo->ddCaps.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_OFFSCREENPLAIN |
                                       DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER;
    if (ddflags(&p->core) & DDF_GDI_CAP) {
        pHalInfo->ddCaps.dwCaps |= DDCAPS_GDI;
    }
    /* No DDCAPS_BLT / BLTSTRETCH / BLTCOLORFILL (doc 15 "Blit caps and the
     * HEL"). On XP a DdBlt that returns DDHAL_DRIVER_NOTHANDLED is
     * E_NOTIMPL to the application, not a fallback to the HEL (DDTEST's
     * windowed colour fill failed), so these caps need a real blitter in
     * the driver. They also did not change FIFA 2000's 1:1 videos, which
     * never Blt at all. */
    /* Source colour keys on video-memory textures (doc 15 "Palettized
     * textures and colour keying"). Without these caps user-mode ddraw
     * keeps a texture's key to itself (SetColorKey succeeds, nothing
     * reaches the kernel). With them dxg calls DdSetColorKey and records
     * the key in the surface, but only if the driver also has a Blt
     * callback. Otherwise dxg drops the whole HAL (DDCAPS_NOHARDWARE,
     * found with CKTEST; DDF_CKEY_NOBLTCB reproduces it). There is no
     * DDCAPS_BLT, so the HEL still does every blit and DdBlt is never
     * called. */
    if (p->core.d3d && !(ddflags(&p->core) & DDF_NO_CKEY)) {
        pHalInfo->ddCaps.dwCaps |= DDCAPS_COLORKEY;
        pHalInfo->ddCaps.dwCKeyCaps = DDCKEYCAPS_SRCBLT;
    }
    /* d3d8.dll's UpdateTexture from system memory goes to the driver (a
     * DP2 TEXBLT / VOLUMEBLT) only with DDCAPS_BLT here; otherwise the
     * runtime copies level by level itself, and that copy reads past the
     * source's levels when the source has fewer than the target (Wine's
     * test_update_volumetexture faults in d3d8.dll) */
    if (p->core.d3d) {
        pHalInfo->ddCaps.dwSVBCaps = DDCAPS_BLT;
    }
    pHalInfo->ddCaps.dwVidMemTotal = dd_heap_end(&p->core) - start;
    pHalInfo->ddCaps.dwVidMemFree = dd_heap_end(&p->core) - start;
    /* dwPalCaps stays 0 at 8 bpp too. XP's dxg.sys drops the whole HAL when
     * a driver reports palette caps (its post-enable validation, next to
     * the DDCAPS_GDI check, found by disassembly). On NT the primary's
     * palette is GDI's, and SetPalette / SetEntries reach DrvSetPalette. */
    if (!(ddflags(&p->core) & DDF_NO_GETDRIVERINFO)) {
        pHalInfo->GetDriverInfo = DdGetDriverInfo;
        pHalInfo->dwFlags = DDHALINFO_GETDRIVERINFOSET;
        /* The DX8 runtime sends its GetDriverInfo2 queries (D3DCAPS8, the
         * format list) only when this is set. Without it d3d8.dll treats
         * the driver as DirectX 7 whatever the answers would have been. */
        if (p->core.d3d && !(ddflags(&p->core) & DDF_NO_DX8)) {
            pHalInfo->dwFlags |= DDHALINFO_GETDRIVERINFO2;
        }
    }
    if (p->core.d3d) {
        /* the Direct3D HAL: caps here, callbacks through GetDriverInfo */
        if (!(ddflags(&p->core) & DDF_NO_3D_CAP)) {
            pHalInfo->ddCaps.dwCaps |= DDCAPS_3D;
            pHalInfo->ddCaps.ddsCaps.dwCaps |= DDSCAPS_3DDEVICE | DDSCAPS_TEXTURE | DDSCAPS_ZBUFFER | DDSCAPS_MIPMAP;
            pHalInfo->ddCaps.dwZBufferBitDepths = DDBD_16_ | DDBD_24_ | DDBD_32_;
        }
        pHalInfo->lpD3DGlobalDriverData = &d3d_global;
        pHalInfo->lpD3DHALCallbacks = &d3d_callbacks;
        if (!(ddflags(&p->core) & DDF_NO_D3D_BUF)) {
            pHalInfo->lpD3DBufCallbacks = &d3d_bufcallbacks;
        }
        dbg_puts(&p->core, "d3dptdisp: d3d caps offered\n");
    }

    if (pvmList) {
        for (i = 0; i < sizeof(*pvmList) / 4; i++) ((ULONG *)pvmList)[i] = 0;
        pvmList->dwFlags = VIDMEM_ISLINEAR;
        pvmList->fpStart = start;
        pvmList->fpEnd = dd_heap_end(&p->core) - 1;
        pvmList->ddsCaps.dwCaps = 0;             /* any surface type */
        pvmList->ddsCapsAlt.dwCaps = 0;
        dbg_hex(&p->core, "d3dptdisp: dd heap ", start);
        dbg_hex(&p->core, "..", heap_end(&p->core) - 1);
        dbg_puts(&p->core, "\n");
    }
    return TRUE;
}

BOOL APIENTRY DrvEnableDirectDraw(DHPDEV dhpdev, DD_CALLBACKS *cb, DD_SURFACECALLBACKS *scb,
                                  DD_PALETTECALLBACKS *pcb)
{
    ULONG i;

    for (i = 0; i < sizeof(*cb) / 4; i++) ((ULONG *)cb)[i] = 0;
    for (i = 0; i < sizeof(*scb) / 4; i++) ((ULONG *)scb)[i] = 0;
    for (i = 0; i < sizeof(*pcb) / 4; i++) ((ULONG *)pcb)[i] = 0;
    cb->dwSize = sizeof(*cb);
    cb->dwFlags = DDHAL_CB32_WAITFORVERTICALBLANK | DDHAL_CB32_MAPMEMORY | DDHAL_CB32_CANCREATESURFACE;
    cb->WaitForVerticalBlank = DdWaitForVerticalBlank;
    cb->MapMemory = DdMapMemory;
    cb->CanCreateSurface = DdCanCreateSurface;
    scb->dwSize = sizeof(*scb);
    if (ddflags(&((PPDEV)dhpdev)->core) & DDF_NO_SURFACE_CB) {
        cb->dwFlags = DDHAL_CB32_MAPMEMORY | DDHAL_CB32_CANCREATESURFACE;
    } else {
        scb->dwFlags = DDHAL_SURFCB32_FLIP | DDHAL_SURFCB32_GETBLTSTATUS | DDHAL_SURFCB32_GETFLIPSTATUS;
        scb->Flip = DdFlip;
        scb->GetBltStatus = DdGetBltStatus;
        scb->GetFlipStatus = DdGetFlipStatus;
        if (((PPDEV)dhpdev)->core.d3d) {
            /* Direct3D. DestroySurface drops the host mirror, Lock reads
             * back a rendered target, Unlock marks texels the guest wrote. */
            scb->dwFlags |= DDHAL_SURFCB32_DESTROYSURFACE | DDHAL_SURFCB32_LOCK | DDHAL_SURFCB32_UNLOCK;
            scb->DestroySurface = DdDestroySurface;
            scb->Lock = DdLock;
            scb->Unlock = DdUnlock;
            if (!(ddflags(&((PPDEV)dhpdev)->core) & DDF_NO_CKEY)) {
                scb->dwFlags |= DDHAL_SURFCB32_SETCOLORKEY;     /* a texture's source colour key -> the host */
                scb->SetColorKey = DdSetColorKey;
            }
            if (!(ddflags(&((PPDEV)dhpdev)->core) & DDF_CKEY_NOBLTCB)) {
                /* Declines every blit back to the HEL. Its presence keeps
                 * the HAL alive with the colour-key caps. */
                scb->dwFlags |= DDHAL_SURFCB32_BLT;
                scb->Blt = DdBlt;
            }
        }
    }
    /* CreateSurface only sizes compressed textures for dxg's allocator */
    cb->dwFlags |= DDHAL_CB32_CREATESURFACE;
    cb->CreateSurface = DdCreateSurface;
    pcb->dwSize = sizeof(*pcb);         /* no palette callbacks: see dwPalCaps above */
    dbg_puts(&((PPDEV)dhpdev)->core, "d3dptdisp: dd enabled\n");
    return TRUE;
}

VOID APIENTRY DrvDisableDirectDraw(DHPDEV dhpdev)
{
    dbg_puts(&((PPDEV)dhpdev)->core, "d3dptdisp: dd disabled\n");
}


/* -------------------------------------------------------------- Direct3D
 * The DX7 HAL (doc 15, M7c). dxg.sys calls these with its device lock
 * held, so one encoder per PDEV is enough. Surfaces are dxg's, in VRAM.
 * The host mirrors the ones Direct3D touches by their VRAM offset. */

/* --- D3D buffers (the runtime's command and vertex buffers): system memory, dxg's --- */

static DWORD APIENTRY D3dCanCreateD3DBuffer(PDD_CANCREATESURFACEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (p && p->core.reg_lines < 4096 && d->lpDDSurfaceDesc) {
        p->core.reg_lines++;
        dbg_hex(&p->core, "d3dptdisp: can create d3d buffer, caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
        dbg_hex(&p->core, " flags ", d->lpDDSurfaceDesc->dwFlags);
        dbg_hex(&p->core, " width ", d->lpDDSurfaceDesc->dwWidth);
        dbg_hex(&p->core, " linear ", d->lpDDSurfaceDesc->dwLinearSize);
        dbg_puts(&p->core, "\n");
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dCreateD3DBuffer(PDD_CREATESURFACEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    ULONG i;

    if (p && p->core.reg_lines < 4096 && d->lpDDSurfaceDesc) {
        p->core.reg_lines++;
        dbg_hex(&p->core, "d3dptdisp: create d3d buffer, caps ", d->lpDDSurfaceDesc->ddsCaps.dwCaps);
        dbg_hex(&p->core, " flags ", d->lpDDSurfaceDesc->dwFlags);
        dbg_hex(&p->core, " width ", d->lpDDSurfaceDesc->dwWidth);
        dbg_hex(&p->core, " linear ", d->lpDDSurfaceDesc->dwLinearSize);
        dbg_hex(&p->core, " count ", d->dwSCnt);
        if (d->dwSCnt && d->lplpSList[0] && d->lplpSList[0]->lpGbl) {
            dbg_hex(&p->core, " lcl caps ", d->lplpSList[0]->ddsCaps.dwCaps);
            dbg_hex(&p->core, " gbl linear ", d->lplpSList[0]->lpGbl->dwLinearSize);
            dbg_hex(&p->core, " vidmem ", (ULONG)d->lplpSList[0]->lpGbl->fpVidMem);
        }
        dbg_puts(&p->core, "\n");
    }
    /* A vertex / index buffer the runtime wants in video memory (it asks
     * only under D3DDEVCAPS_HWVERTEXBUFFER / HWINDEXBUFFER, v9). dxg takes
     * it from the linear heap when told the block size, as for a compressed
     * texture (DdCreateSurface). dwLinearSize is its size in bytes. Command
     * buffers and everything else stay in dxg's system memory. */
    if (p && p->core.reg_lines < 4096 && d->lpDDSurfaceDesc && d->dwSCnt && d->lplpSList[0] && d->lplpSList[0]->lpSurfMore) {
        p->core.reg_lines++;
        dbg_hex(&p->core, "d3dptdisp:   caps2 ", d->lplpSList[0]->lpSurfMore->ddsCapsEx.dwCaps2);
        dbg_puts(&p->core, "\n");
    }
    /* The request's caps decide, not ddsCapsEx. The runtime's vertex buffers
     * come without DDSCAPS2_VERTEXBUFFER (DDSD_FVF marks them, the index
     * buffers carry DDSCAPS2_INDEXBUFFER). A buffer refused here after dxg
     * had picked the heap for it ended up with SYSTEMMEMORY caps on a heap
     * offset, and D3DGAME8 crashed in its first Lock. */
    if (p && !(ddflags(&p->core) & DDF_NO_HWVB) && d->lpDDSurfaceDesc &&
        (d->lpDDSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_VIDEOMEMORY) &&
        (d->lpDDSurfaceDesc->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER) && d->lpDDSurfaceDesc->dwLinearSize &&
        d->dwSCnt && d->lplpSList[0]) {
        for (i = 0; i < d->dwSCnt; i++) {
            PDD_SURFACE_GLOBAL g = d->lplpSList[i]->lpGbl;
            if (!g) {
                continue;
            }
            g->dwLinearSize = d->lpDDSurfaceDesc->dwLinearSize;
            g->dwBlockSizeX = d->lpDDSurfaceDesc->dwLinearSize;
            g->dwBlockSizeY = 1;
            g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
        }
        d->lpDDSurfaceDesc->dwFlags |= DDSD_LINEARSIZE;
        d->ddRVal = DD_OK;
        return DDHAL_DRIVER_NOTHANDLED;
    }
    for (i = 0; i < d->dwSCnt; i++) {
        d->lplpSList[i]->ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
        d->lplpSList[i]->ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
    }
    d->lpDDSurfaceDesc->ddsCaps.dwCaps |= DDSCAPS_SYSTEMMEMORY;
    d->lpDDSurfaceDesc->ddsCaps.dwCaps &= ~DDSCAPS_VIDEOMEMORY;
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY D3dDestroyD3DBuffer(PDD_DESTROYSURFACEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (d->lpDDSurface) {
        ULONG handle = surf_handle(d->lpDDSurface);

        surf_forget(handle);
        if (p && !(d->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
            d3d_handle_op(&p->core, D3DPT_OP_VRAM_RELEASE, handle);   /* a VRAM buffer (v9) */
        }
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY D3dLockD3DBuffer(PDD_LOCKDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (p && p->core.reg_lines < 4096 && d->lpDDSurface && d->lpDDSurface->lpGbl) {
        p->core.reg_lines++;
        dbg_hex(&p->core, "d3dptdisp: lock d3d buffer ", surf_handle(d->lpDDSurface));
        dbg_hex(&p->core, " caps ", d->lpDDSurface->ddsCaps.dwCaps);
        dbg_hex(&p->core, " vidmem ", (ULONG)d->lpDDSurface->lpGbl->fpVidMem);
        dbg_hex(&p->core, " linear ", d->lpDDSurface->lpGbl->dwLinearSize);
        dbg_hex(&p->core, " flags ", d->dwFlags);
        if (d->bHasRect) {
            dbg_hex(&p->core, " range ", (ULONG)d->rArea.left);
            dbg_hex(&p->core, "..", (ULONG)d->rArea.right);
        }
        dbg_puts(&p->core, "\n");
    }
    /* A VRAM buffer (v9). Remember what the runtime locks, for the
     * VRAM_DIRTY_RANGE the Unlock sends: the byte range in rArea
     * (left..right) when it gives one, the whole buffer otherwise. dxg
     * hands the caller the pointer (NOTHANDLED). DISCARD / NOOVERWRITE need
     * nothing here, because every draw before this Lock has already run
     * (the DP2 records execute in the doorbell write). */
    if (p && d->lpDDSurface && d->lpDDSurface->lpGbl && !(d->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        surf_lock_range(surf_handle(d->lpDDSurface), d->bHasRect, d->rArea.left, d->rArea.right);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY D3dUnlockD3DBuffer(PDD_UNLOCKDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    /* a VRAM buffer (v9): the guest has written the locked range */
    if (p && d->lpDDSurface && !(d->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        surf_unlock_dirty(&p->core, surf_handle(d->lpDDSurface));
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* --- the command window --- */

/* --------------------------------------- what the core asks of NT
 *
 * The four OS services the core cannot provide itself (doc 19 §19), plus
 * the surface accessors. NT's DD_SURFACE_LOCAL and 9x's
 * DDRAWI_DDRAWSURFACE_LCL hold the same facts at different offsets, and
 * this is the NT half of that translation. */

void *d3dpt_os_alloc(ULONG bytes)
{
    return EngAllocMem(FL_ZERO_MEMORY, bytes, ALLOC_TAG);
}

void d3dpt_os_free(void *p)
{
    EngFreeMem(p);
}

void d3dpt_os_ticks(LONGLONG *now, LONGLONG *freq)
{
    if (freq) {
        EngQueryPerformanceFrequency(freq);
    }
    EngQueryPerformanceCounter(now);
}

BOOL d3dpt_os_surf(d3dpt_core *c, void *os, d3dpt_surf_desc *out)
{
    PDD_SURFACE_LOCAL s = (PDD_SURFACE_LOCAL)os;

    if (!s || !s->lpGbl) {
        return FALSE;
    }
    out->os = os;
    out->handle = s->lpSurfMore ? s->lpSurfMore->dwSurfaceHandle : 0;
    out->caps = s->ddsCaps.dwCaps;
    out->caps2 = s->lpSurfMore ? s->lpSurfMore->ddsCapsEx.dwCaps2 : 0;
    out->depth = (out->caps2 & DDSCAPS2_VOLUME_) ? (s->lpSurfMore->ddsCapsEx.dwCaps4 & 0xffff) : 0;
    out->samples = s->lpSurfMore ? (s->lpSurfMore->ddsCapsEx.dwCaps3 & DDSCAPS3_MULTISAMPLE_MASK_) : 0;
    out->lwmip = s->lpSurfMore && (s->lpSurfMore->ddsCapsEx.dwCaps3 & DDSCAPS3_LIGHTWEIGHTMIPMAP_);
    out->autogen = s->lpSurfMore && (s->lpSurfMore->ddsCapsEx.dwCaps3 & DDSCAPS3_AUTOGENMIPMAP_);
    out->flags = s->dwFlags;
    out->w = s->lpGbl->wWidth;
    out->h = s->lpGbl->wHeight;
    out->pitch = (ULONG)s->lpGbl->lPitch;
    out->linear = s->lpGbl->dwLinearSize;
    out->vidmem = (ULONG)s->lpGbl->fpVidMem;
    /* no pixel format of its own means the primary's, which is the mode's */
    if (s->dwFlags & DDRAWISURF_HASPIXELFORMAT_) {
        out->fmt = pf_format(&s->lpGbl->ddpfSurface);
        out->pf_flags = s->lpGbl->ddpfSurface.dwFlags;
    } else {
        out->fmt = c->bpp == 32 ? D3DFMT_X8R8G8B8_ : D3DFMT_R5G6B5_;
        out->pf_flags = 0xffffffffu;
    }
    out->ck_lo = s->ddckCKSrcBlt.dwColorSpaceLowValue;
    out->ck_hi = s->ddckCKSrcBlt.dwColorSpaceHighValue;
    out->ck_dst_lo = s->ddckCKDestBlt.dwColorSpaceLowValue;
    return TRUE;
}

ULONG d3dpt_os_attached(void *os, void **out, ULONG max)
{
    PDD_SURFACE_LOCAL s = (PDD_SURFACE_LOCAL)os;
    PDD_ATTACHLIST a;
    ULONG n = 0;

    for (a = s->lpAttachList; a && n < max; a = a->lpLink) {
        PDD_SURFACE_LOCAL t = a->lpAttached;
        if (t && t->lpGbl && !(t->ddsCaps.dwCaps & DDSCAPS_MIPMAP)) {
            out[n++] = t;
        }
    }
    return n;
}

ULONG d3dpt_os_attached_all(void *os, void **out, ULONG max)
{
    PDD_SURFACE_LOCAL s = (PDD_SURFACE_LOCAL)os;
    PDD_ATTACHLIST a;
    ULONG n = 0;

    for (a = s->lpAttachList; a && n < max; a = a->lpLink) {
        if (a->lpAttached && a->lpAttached->lpGbl) {
            out[n++] = a->lpAttached;
        }
    }
    return n;
}

/* the next mip level attached to s (DDSCAPS_MIPMAP, no larger, the same
 * cube face), or NULL.
 * lpAttachList holds the surfaces attached to s, never its parent, so a
 * level of the same size is the next one: a system-memory DXT chain of
 * d3d9.dll's has no pixel format and is sized in block-row bytes by block
 * rows, so its 4x4, 2x2 and 1x1 levels are all one block (the callers stop
 * at 16 levels) */
void *d3dpt_os_next_mip(void *os)
{
    PDD_SURFACE_LOCAL s = (PDD_SURFACE_LOCAL)os;
    PDD_ATTACHLIST a;

    for (a = s->lpAttachList; a; a = a->lpLink) {
        PDD_SURFACE_LOCAL t = a->lpAttached;
        /* a cube's root lists its other faces too, of the same size: the
         * next level is the same face's */
        ULONG sf = s->lpSurfMore ? s->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_CUBEMAP_ALLFACES_ : 0;
        ULONG tf = (t && t->lpSurfMore) ? t->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_CUBEMAP_ALLFACES_ : 0;
        if (t && t->lpGbl && (t->ddsCaps.dwCaps & DDSCAPS_MIPMAP) && t != s && sf == tf &&
            t->lpGbl->wWidth <= s->lpGbl->wWidth && t->lpGbl->wHeight <= s->lpGbl->wHeight) {
            return t;
        }
    }
    return NULL;
}

/* the surface handle, for the callbacks that only need that */
static ULONG surf_handle(PDD_SURFACE_LOCAL s)
{
    return (s && s->lpSurfMore) ? s->lpSurfMore->dwSurfaceHandle : 0;
}

/* Registers a surface with the core, which wants a descriptor where the
 * callbacks have an lpDDSLcl. */
static void nt_register(PPDEV p, PDD_SURFACE_LOCAL s)
{
    d3dpt_surf_desc d;

    if (d3dpt_os_surf(&p->core, s, &d)) {
        d3d_register(&p->core, &d);
    }
}

/* dxg calls this once per surface it creates. With Direct3D on, every one
 * is mirrored later, in CreateSurfaceEx.
 *
 * dxg sizes a video-memory surface from its pixel format's bit count before
 * it takes it from the heap. A compressed FOURCC format has none, so the
 * request was for zero bytes and every DXT texture failed at CreateTexture
 * with D3DERR_OUTOFVIDEOMEMORY (DXTTEST, doc 15). As the DDK samples do,
 * hand dxg the block size (the whole compressed image as one block) and the
 * linear size, and let it allocate. Everything else is left to dxg. */
static DWORD APIENTRY DdCreateSurface(PDD_CREATESURFACEDATA d)
{
    DDSURFACEDESC *sd = d->lpDDSurfaceDesc;
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    ULONG i, f;

    d->ddRVal = DD_OK;
    if (p && p->core.reg_lines < 4096 && sd && (sd->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER)) {
        p->core.reg_lines++;
        dbg_hex(&p->core, "d3dptdisp: create buffer, caps ", sd->ddsCaps.dwCaps);
        dbg_hex(&p->core, " flags ", sd->dwFlags);
        dbg_hex(&p->core, " width ", sd->dwWidth);
        dbg_hex(&p->core, " linear ", sd->dwLinearSize);
        dbg_hex(&p->core, " count ", d->dwSCnt);
        if (d->dwSCnt && d->lplpSList[0] && d->lplpSList[0]->lpGbl) {
            dbg_hex(&p->core, " lcl caps ", d->lplpSList[0]->ddsCaps.dwCaps);
            dbg_hex(&p->core, " gbl linear ", d->lplpSList[0]->lpGbl->dwLinearSize);
            dbg_hex(&p->core, " vidmem ", (ULONG)d->lplpSList[0]->lpGbl->fpVidMem);
        }
        dbg_puts(&p->core, "\n");
    }
    if (sd && d->dwSCnt && d->lplpSList[0] && d->lplpSList[0]->lpSurfMore &&
        (d->lplpSList[0]->lpSurfMore->ddsCapsEx.dwCaps2 & DDSCAPS2_VOLUME_)) {
        /* A volume texture. dxg would size it as one slice, so the whole
         * box is requested as a block of depth x (row pitch x height) bytes
         * (the depth is in dwCaps4's low word, and every level is a surface
         * of its own). The block's height is the slice pitch on purpose.
         * dwBlockSizeY shares a union with lSlicePitch, and user mode takes
         * its copy of the surface when this call returns. A slice pitch set
         * any later (CreateSurfaceEx) reaches only the kernel's copy, and the
         * runtime then locks slice n at n bytes in (VOLTEST). */
        /* Row size and row count in the surface's own format: block rows
         * for DXT, dword-aligned texel rows otherwise. */
        ULONG fmt = pf_format(&sd->ddpfPixelFormat);
        for (i = 0; i < d->dwSCnt; i++) {
            PDD_SURFACE_LOCAL s = d->lplpSList[i];
            PDD_SURFACE_GLOBAL g = s ? s->lpGbl : NULL;
            ULONG depth = (s && s->lpSurfMore) ? (s->lpSurfMore->ddsCapsEx.dwCaps4 & 0xffff) : 0, pitch, rows, size;

            if (!g) {
                continue;
            }
            pitch = fmt_is_dxt(fmt) ? fmt_row_bytes(fmt, g->wWidth) : (fmt_row_bytes(fmt, g->wWidth) + 3) & ~3u;
            rows = surf_rows(fmt, g->wHeight);
            size = pitch * rows * depth;
            if (p && p->core.reg_lines < 4096) {
                p->core.reg_lines++;
                dbg_hex(&p->core, "d3dptdisp: create volume ", i);
                dbg_hex(&p->core, " of ", d->dwSCnt);
                dbg_hex(&p->core, " caps ", s->ddsCaps.dwCaps);
                dbg_hex(&p->core, " caps2 ", s->lpSurfMore ? s->lpSurfMore->ddsCapsEx.dwCaps2 : 0);
                dbg_hex(&p->core, " caps4 ", s->lpSurfMore ? s->lpSurfMore->ddsCapsEx.dwCaps4 : 0);
                dbg_hex(&p->core, " w ", g->wWidth);
                dbg_hex(&p->core, " h ", g->wHeight);
                dbg_hex(&p->core, " pitch ", (ULONG)g->lPitch);
                dbg_hex(&p->core, " pf ", sd->ddpfPixelFormat.dwFlags);
                dbg_hex(&p->core, " bits ", sd->ddpfPixelFormat.dwRGBBitCount);
                dbg_hex(&p->core, " -> ", size);
                dbg_puts(&p->core, "\n");
            }
            if (!size || (s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
                continue;
            }
            g->lPitch = pitch;
            g->dwBlockSizeX = depth;
            g->lSlicePitch = pitch * rows;
            g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
        }
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (sd && d->dwSCnt == 1 && d->lplpSList[0] && d->lplpSList[0]->lpGbl && d->lplpSList[0]->lpSurfMore &&
        (d->lplpSList[0]->lpSurfMore->ddsCapsEx.dwCaps3 & DDSCAPS3_LIGHTWEIGHTMIPMAP_) &&
        (d->lplpSList[0]->ddsCaps.dwCaps & DDSCAPS_TEXTURE) && !(d->lplpSList[0]->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        /* A lightweight mipmap (surf_lw_layout): one surface that holds its
         * whole mip chain. d3d9.dll leaves out the pixel format when it is
         * the desktop's. */
        PDD_SURFACE_LOCAL s = d->lplpSList[0];
        PDD_SURFACE_GLOBAL g = s->lpGbl;
        ULONG fmt = (sd->dwFlags & DDSD_PIXELFORMAT) ? pf_format(&sd->ddpfPixelFormat) : 0, n = 0, size, pitch[16];

        if (!fmt && p) {
            fmt = p->core.bpp == 32 ? D3DFMT_X8R8G8B8_ : D3DFMT_R5G6B5_;
        }
        size = surf_lw_layout(fmt, g->wWidth, g->wHeight, NULL, pitch, &n);
        if (p && p->core.reg_lines < 4096) {
            p->core.reg_lines++;
            dbg_hex(&p->core, "d3dptdisp: create lightweight mipmap, w ", g->wWidth);
            dbg_hex(&p->core, " h ", g->wHeight);
            dbg_hex(&p->core, " fmt ", fmt);
            dbg_hex(&p->core, " levels ", n);
            dbg_hex(&p->core, " -> ", size);
            dbg_puts(&p->core, "\n");
        }
        if (!size) {
            return DDHAL_DRIVER_NOTHANDLED;
        }
        if (fmt_is_dxt(fmt)) {
            g->dwLinearSize = surf_dxt_size(fmt, g->wWidth, g->wHeight);   /* the lPitch union, as for any DXT surface */
            sd->dwFlags |= DDSD_LINEARSIZE;
            sd->dwLinearSize = g->dwLinearSize;
        } else {
            g->lPitch = pitch[0];
            sd->dwFlags |= DDSD_PITCH;
            sd->lPitch = pitch[0];
        }
        g->dwBlockSizeX = size;
        g->dwBlockSizeY = 1;
        g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (!sd || !(sd->ddpfPixelFormat.dwFlags & DDPF_FOURCC)) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    f = sd->ddpfPixelFormat.dwFourCC;
    if (fmt_fourcc_rows(f)) {
        /* Q8W8V8U8 (a D3DFORMAT as the FOURCC). Dword-aligned texel rows,
         * which dxg cannot size without a bit count. */
        for (i = 0; i < d->dwSCnt; i++) {
            PDD_SURFACE_LOCAL s = d->lplpSList[i];
            PDD_SURFACE_GLOBAL g = s ? s->lpGbl : NULL;
            ULONG pitch;

            if (!g) {
                continue;
            }
            pitch = (fmt_row_bytes(f, g->wWidth) + 3) & ~3u;
            g->lPitch = pitch;
            if (!(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
                g->dwBlockSizeX = pitch * g->wHeight;
                g->dwBlockSizeY = 1;
                g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
            }
            if (i == 0) {
                sd->dwFlags |= DDSD_PITCH;
                sd->lPitch = pitch;
            }
        }
        return DDHAL_DRIVER_NOTHANDLED;
    }
    if (!fmt_is_dxt(f)) {
        return DDHAL_DRIVER_NOTHANDLED;
    }
    for (i = 0; i < d->dwSCnt; i++) {
        PDD_SURFACE_LOCAL s = d->lplpSList[i];
        PDD_SURFACE_GLOBAL g = s ? s->lpGbl : NULL;
        ULONG size;

        if (!g) {
            continue;
        }
        size = surf_dxt_size(f, g->wWidth, g->wHeight);
        g->dwLinearSize = size;             /* the lPitch union: the linear size, as surf_pitch expects */
        if (!(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
            g->dwBlockSizeX = size;
            g->dwBlockSizeY = 1;
            g->fpVidMem = DDHAL_PLEASEALLOC_BLOCKSIZE;
        }
        if (i == 0) {
            sd->dwFlags |= DDSD_LINEARSIZE;
            sd->dwLinearSize = size;
        }
    }
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY DdCreateSurfaceEx(PDD_CREATESURFACEEXDATA d)
{
    /* One PDEV has Direct3D (the primary display). The data's lpDDLcl
     * shares a union with the global in some DDK versions, so it is not
     * dereferenced. */
    PPDEV p = nt_d3d_pdev();
    PDD_SURFACE_LOCAL s = d->lpDDSLcl;

    if (p && s && s->lpGbl && p->core.d3d) {
        d3d_register_chain(&p->core, s);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* The runtime's device-info queries (D3DDEVINFOID_*: texture manager,
 * vertex stats). Nothing to report: a failure, which d3d8's GetInfo
 * returns as S_FALSE (Wine's test_get_info; a success claimed every id
 * with the buffer left as it was). */
static DWORD APIENTRY DdGetDriverState(PDD_GETDRIVERSTATEDATA d)
{
    d->ddRVal = DDERR_UNSUPPORTED;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY DdDestroySurface(PDD_DESTROYSURFACEDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (p->core.reg_lines < 4096 && d->lpDDSurface && (d->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_EXECUTEBUFFER)) {
        p->core.reg_lines++;
        dbg_hex(&p->core, "d3dptdisp: destroy buffer ", surf_handle(d->lpDDSurface));
        dbg_hex(&p->core, " caps ", d->lpDDSurface->ddsCaps.dwCaps);
        dbg_puts(&p->core, "\n");
    }
    surf_forget(surf_handle(d->lpDDSurface));
    if (d->lpDDSurface && !(d->lpDDSurface->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY)) {
        d3d_handle_op(&p->core, D3DPT_OP_VRAM_RELEASE, surf_handle(d->lpDDSurface));
    }
    d->ddRVal = DD_OK;
    /* dxg frees the heap block */
    return DDHAL_DRIVER_NOTHANDLED;
}

/* A Z buffer locked for writing (doc 19 §34). A title that resets its
 * depth by hand, or dxg's HEL doing a depth fill, writes VRAM the host's
 * depth buffer never sees. DdUnlock hands it to the core, which turns a
 * buffer written to one value into a host Z clear. A read-only lock is
 * left alone, so a title reading its depth does not wipe the frame's. */
static PDD_SURFACE_LOCAL zlock_surf;
static ULONG zlocks, zlocks_said;

static BOOL is_vram_z(PDD_SURFACE_LOCAL s)
{
    return s && s->lpGbl && (s->ddsCaps.dwCaps & DDSCAPS_ZBUFFER) && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY);
}

static DWORD APIENTRY DdLock(PDD_LOCKDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    PDD_SURFACE_LOCAL s = d->lpDDSurface;

    if (is_vram_z(s)) {
        zlocks++;
        if (zlocks_said < 16 || !(zlocks & 1023)) {
            zlocks_said++;
            dbg_hex(&p->core, "d3dptdisp: Z lock ", zlocks);
            dbg_hex(&p->core, " flags ", d->dwFlags);
            dbg_puts(&p->core, "\n");
        }
        zlock_surf = (d->dwFlags & DDLOCK_READONLY) ? NULL : s;
    }
    /* a render target the host drew into: read the frame back into VRAM first */
    if (d3d_ctx_live && s && surf_is_target(s->ddsCaps.dwCaps)) {
        d3d_register_moved(&p->core, s);
        d3d_readback(&p->core, surf_handle(s));
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* SetColorKey(DDCKEY_SRCBLT) on a video-memory texture. The host keys the
 * texels in [low, high] (alpha 0 + alpha test while COLORKEYENABLE is on).
 * dxg also records the key in the surface, and registration reads it from
 * there for a key set before the surface was mirrored (the core's
 * surf_key_snapshot; surf_colorkey_check tells the host at the first
 * bind). dwFlags also carries DDCKEY_COLORSPACE for a range. */
static DWORD APIENTRY DdSetColorKey(PDD_SETCOLORKEYDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    PDD_SURFACE_LOCAL s = d->lpDDSurface;

    if (p->core.reg_lines < 4096) {
        p->core.reg_lines++;
        dbg_hex(&p->core, "d3dptdisp: setcolorkey surface ", s ? surf_handle(s) : 0);
        dbg_hex(&p->core, " flags ", d->dwFlags);
        dbg_hex(&p->core, " key ", d->ckNew.dwColorSpaceLowValue);
        dbg_hex(&p->core, "..", d->ckNew.dwColorSpaceHighValue);
        dbg_puts(&p->core, "\n");
    }
    if (p->core.d3d && s && (d->dwFlags & DDCKEY_SRCBLT) && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) &&
        (s->ddsCaps.dwCaps & DDSCAPS_TEXTURE)) {
        surf_colorkey_set(&p->core, surf_handle(s), d->ckNew.dwColorSpaceLowValue, d->ckNew.dwColorSpaceHighValue);
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* Present so that dxg accepts the colour-key caps. Never called, since the
 * driver claims no DDCAPS_BLT (the HEL does every blit in user mode). It
 * declines anything that does arrive, which on XP the application sees as
 * E_NOTIMPL, not as a HEL fallback (doc 15 "Blit caps and the HEL"). So
 * claiming blit caps needs a real blitter here. The first few calls are
 * logged with their rectangles. */
static DWORD APIENTRY DdBlt(PDD_BLTDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;

    if (p->blt_lines < 8) {
        p->blt_lines++;
        dbg_hex(&p->core, "d3dptdisp: blt flags ", d->dwFlags);
        dbg_hex(&p->core, " rop ", d->bltFX.dwROP);
        dbg_hex(&p->core, " src ", (ULONG)(d->rSrc.right - d->rSrc.left));
        dbg_hex(&p->core, "x", (ULONG)(d->rSrc.bottom - d->rSrc.top));
        dbg_hex(&p->core, " dst ", (ULONG)(d->rDest.right - d->rDest.left));
        dbg_hex(&p->core, "x", (ULONG)(d->rDest.bottom - d->rDest.top));
        dbg_puts(&p->core, " declined to the HEL\n");
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

static DWORD APIENTRY DdUnlock(PDD_UNLOCKDATA d)
{
    PPDEV p = (PPDEV)d->lpDD->dhpdev;
    PDD_SURFACE_LOCAL s = d->lpDDSurface;

    /* the guest wrote texels (or a target): the host re-reads them before use */
    if (p->core.d3d && s && !(s->ddsCaps.dwCaps & DDSCAPS_SYSTEMMEMORY) &&
        ((s->ddsCaps.dwCaps & DDSCAPS_TEXTURE) || (d3d_ctx_live && surf_is_target(s->ddsCaps.dwCaps)))) {
        d3d_handle_op(&p->core, D3DPT_OP_VRAM_DIRTY, surf_handle(s));
    }
    if (s && s == zlock_surf) {
        zlock_surf = NULL;
        if (p->core.d3d) {
            d3d_z_written(&p->core, surf_handle(s), s->lpGbl->ddpfSurface.dwZBufferBitDepth,
                          s->lpGbl->ddpfSurface.dwZBitMask);
        }
    }
    d->ddRVal = DD_OK;
    return DDHAL_DRIVER_NOTHANDLED;
}

/* --- contexts: dxg's calls onto core_ctx.c --- */

static DWORD APIENTRY D3dContextCreate(LPD3DNTHAL_CONTEXTCREATEDATA d)
{
    PPDEV p = nt_d3d_pdev();
    PDD_SURFACE_LOCAL rt = d->lpDDSLcl, z = d->lpDDSZLcl;

    d->ddrval = DDERR_GENERIC;
    if (!p || !p->core.d3d || !rt || !rt->lpGbl) {
        return DDHAL_DRIVER_HANDLED;
    }
    /* Register the targets again, with the chain the target belongs to,
     * because a flip chain's surfaces may have moved. A DirectX 6 title's
     * back buffer is first seen here. */
    d3d_register_chain(&p->core, rt);
    if (z) {
        nt_register(p, z);
    }
    d->ddrval = ctx_create(&p->core, &d->dwhContext, d->dwPID, surf_handle(rt), z ? surf_handle(z) : 0);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dContextDestroy(LPD3DNTHAL_CONTEXTDESTROYDATA d)
{
    /* The data carries no PDEV. The context id is enough, because only one
     * PDEV has Direct3D on (the primary display). */
    PPDEV p = nt_d3d_pdev();

    d->ddrval = p ? ctx_destroy_one(&p->core, d->dwhContext) : DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dContextDestroyAll(LPD3DNTHAL_CONTEXTDESTROYALLDATA d)
{
    PPDEV p = nt_d3d_pdev();

    if (p) {
        ctx_destroy_all(&p->core, d->dwPID);
    }
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dSceneCapture(LPD3DNTHAL_SCENECAPTUREDATA d)
{
    PPDEV p = nt_d3d_pdev();

    d->ddrval = p ? ctx_scene_capture(&p->core, d->dwhContext, d->dwFlag == D3DNTHAL_SCENE_CAPTURE_END) : DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dSetRenderTarget(LPD3DNTHAL_SETRENDERTARGETDATA d)
{
    PPDEV p = nt_d3d_pdev();

    d->ddrval = DDERR_GENERIC;
    if (!p || !ctx_of(&p->core, d->dwhContext) || !d->lpDDS || !d->lpDDS->lpGbl) {
        return DDHAL_DRIVER_HANDLED;
    }
    d3d_register_chain(&p->core, d->lpDDS);
    if (d->lpDDSZ) {
        nt_register(p, d->lpDDSZ);
    }
    d->ddrval = ctx_set_render_target(&p->core, d->dwhContext, surf_handle(d->lpDDS),
                                      d->lpDDSZ ? surf_handle(d->lpDDSZ) : 0);
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dClear2(LPD3DNTHAL_CLEAR2DATA d)
{
    PPDEV p = nt_d3d_pdev();

    d->ddrval = p ? ctx_clear2(&p->core, d->dwhContext, d->dwFlags, d->dwFillColor, d->dvFillDepth,
                               d->dwFillStencil, d->lpRects, d->dwNumRects)
                  : DDERR_GENERIC;
    return DDHAL_DRIVER_HANDLED;
}

static DWORD APIENTRY D3dValidateTextureStageState(LPD3DNTHAL_VALIDATETEXTURESTAGESTATEDATA d)
{
    d->dwNumPasses = 1;
    d->ddrval = DD_OK;
    return DDHAL_DRIVER_HANDLED;
}

/* --- DrawPrimitives2: where the runtime's buffers are, and what the
 * core made of them --- */

static DWORD APIENTRY D3dDrawPrimitives2(LPD3DNTHAL_DRAWPRIMITIVES2DATA d)
{
    PPDEV p = nt_d3d_pdev();
    d3dpt_dp2_call call;
    d3dpt_dp2_result res;

    if (!p || !d->lpDDCommands || !d->lpDDCommands->lpGbl) {
        if (p && p->core.dp2_errors < 8) {
            p->core.dp2_errors++;
            dbg_hex(&p->core, "d3dptdisp: dp2 refused, context ", (ULONG)d->dwhContext);
            dbg_hex(&p->core, " commands ", (ULONG)(ULONG_PTR)d->lpDDCommands);
            dbg_puts(&p->core, "\n");
        }
        d->ddrval = DDERR_GENERIC;
        return DDHAL_DRIVER_HANDLED;
    }
    if (p->core.dp2_calls < 8) {
        dbg_hex(&p->core, "d3dptdisp: dp2 call flags ", d->dwFlags);
        dbg_hex(&p->core, " cmd caps ", d->lpDDCommands->ddsCaps.dwCaps);
        dbg_hex(&p->core, " at ", (ULONG)d->lpDDCommands->lpGbl->fpVidMem);
        dbg_hex(&p->core, " +", d->dwCommandOffset);
        dbg_hex(&p->core, " len ", d->dwCommandLength);
        /* Under USERMEMVERTICES lpDDVertex is not valid (the DDK says so).
         * With video-memory buffers in play it arrived as a dangling pointer
         * whose lpGbl was garbage, and this log block hit STOP 0x8E. */
        if (!(d->dwFlags & D3DNTHALDP2_USERMEMVERTICES) && d->lpDDVertex && d->lpDDVertex->lpGbl) {
            dbg_hex(&p->core, " vtx caps ", d->lpDDVertex->ddsCaps.dwCaps);
            dbg_hex(&p->core, " at ", (ULONG)d->lpDDVertex->lpGbl->fpVidMem);
            dbg_hex(&p->core, " linear ", d->lpDDVertex->lpGbl->dwLinearSize);
        } else {
            dbg_hex(&p->core, " user vtx ", (ULONG)(ULONG_PTR)d->lpVertices);
        }
        dbg_hex(&p->core, " +", d->dwVertexOffset);
        dbg_hex(&p->core, " n ", d->dwVertexLength);
        dbg_hex(&p->core, " type ", d->dwVertexType);
        dbg_puts(&p->core, "\n");
    }
    memset(&call, 0, sizeof(call));
    call.ctx = d->dwhContext;
    call.cmd = (const UCHAR *)d->lpDDCommands->lpGbl->fpVidMem + d->dwCommandOffset;
    call.clen = d->dwCommandLength;
    if (d->dwFlags & D3DNTHALDP2_USERMEMVERTICES) {
        call.vtx = (const UCHAR *)d->lpVertices + d->dwVertexOffset;
    } else if (d->lpDDVertex && d->lpDDVertex->lpGbl) {
        call.vtx = (const UCHAR *)d->lpDDVertex->lpGbl->fpVidMem + d->dwVertexOffset;
    }
    call.vsize = d->dwVertexSize;
    call.vcount = d->dwVertexLength;
    call.vlen = call.vtx ? d->dwVertexLength * d->dwVertexSize : 0;
    /* What the buffer really holds from vtx on: the declared vertices for
     * user memory, a dxg buffer's linear size otherwise. */
    call.vall = call.vlen;
    if (call.vtx && !(d->dwFlags & D3DNTHALDP2_USERMEMVERTICES) &&
        d->lpDDVertex->lpGbl->dwLinearSize > d->dwVertexOffset) {
        call.vall = d->lpDDVertex->lpGbl->dwLinearSize - d->dwVertexOffset;
        if (call.vall > (32u << 20)) call.vall = call.vlen;
    }
    call.flags = d->dwFlags;
    call.vertex_type = d->dwVertexType;
    call.eb = (d->dwFlags & D3DNTHALDP2_EXECUTEBUFFER) != 0;
    call.rstates = d->lpdwRStates;
    call.resp = (UCHAR *)d->lpDDCommands->lpGbl->fpVidMem;
    call.resp_max = d->lpDDCommands->lpGbl->dwLinearSize;

    dp2_run(&p->core, &call, &res);
    d->ddrval = res.hr;
    /* A bounce offset counts from the command buffer's start, like
     * dwCommandOffset. An error offset is the core's own. On success a DX9
     * runtime reads dwErrorOffset as the bytes of query responses at the
     * buffer's start (dp2_run) */
    d->dwErrorOffset = res.bounce ? d->dwCommandOffset + res.offset : res.hr == DD_OK ? res.resp_bytes : res.offset;
    return DDHAL_DRIVER_HANDLED;
}

/* -------------------------------------------------------------- entry */

static DRVFN drv_fn[] = {
    { INDEX_DrvEnablePDEV,     (PFN)DrvEnablePDEV },
    { INDEX_DrvCompletePDEV,   (PFN)DrvCompletePDEV },
    { INDEX_DrvDisablePDEV,    (PFN)DrvDisablePDEV },
    { INDEX_DrvEnableSurface,  (PFN)DrvEnableSurface },
    { INDEX_DrvDisableSurface, (PFN)DrvDisableSurface },
    { INDEX_DrvAssertMode,     (PFN)DrvAssertMode },
    { INDEX_DrvSetPalette,     (PFN)DrvSetPalette },
    { INDEX_DrvIcmSetDeviceGammaRamp, (PFN)DrvIcmSetDeviceGammaRamp },
    { INDEX_DrvGetModes,       (PFN)DrvGetModes },
    { INDEX_DrvSetPointerShape, (PFN)DrvSetPointerShape },
    { INDEX_DrvMovePointer,    (PFN)DrvMovePointer },
    { INDEX_DrvSynchronizeSurface, (PFN)DrvSynchronizeSurface },
    { INDEX_DrvGetDirectDrawInfo, (PFN)DrvGetDirectDrawInfo },
    { INDEX_DrvEnableDirectDraw,  (PFN)DrvEnableDirectDraw },
    { INDEX_DrvDisableDirectDraw, (PFN)DrvDisableDirectDraw },
};

BOOL APIENTRY DrvEnableDriver(ULONG iEngineVersion, ULONG cj, DRVENABLEDATA *pded)
{
    if (iEngineVersion < DDI_DRIVER_VERSION_NT5 || cj < sizeof(DRVENABLEDATA)) {
        return FALSE;
    }
    pded->iDriverVersion = DDI_DRIVER_VERSION_NT5_01;
    pded->c = sizeof(drv_fn) / sizeof(drv_fn[0]);
    pded->pdrvfn = drv_fn;
    return TRUE;
}

VOID APIENTRY DrvDisableDriver(VOID)
{
}
