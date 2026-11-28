/*
 * d3dpt11.c: the WDDM user-mode display driver for Direct3D 10 and 11 on
 * the d3dpt-vga adapter (track M25, ADR-029). d3d11.dll loads it by the
 * name the INF writes after Direct3D 9's in UserModeDriverName and calls
 * OpenAdapter10_2; the device functions it hands out are the Direct3D 11
 * DDI, Windows 8's 11.1 interface (with its video functions, which a
 * device made with VIDEO_SUPPORT needs, DWM's among them; track M25 step
 * 4) or Windows 7's (D3D11_0_7), whose 3D pipeline level says which
 * feature levels the runtime offers (10_0 and 10_1 for now). d3d10.dll
 * and d3d10_1.dll get Direct3D 10's and 10.1's interfaces over the same
 * entries (d10_dev.c).
 *
 * The transport is the Direct3D 9 driver's (../um/d3dptumd.c): records in
 * the runtime's command buffer, allocations named through patch
 * locations, the kernel driver (../km/d3dptkmd.c) moving them into the
 * device's command window at submit. Everything this driver says to the
 * host is D3DPT_OP_D11 records (d3dpt_proto.h, v25 and v26): a run of tokens for
 * this device's context, executed on the host's d3d11 (DXVK) with this
 * device's own pipeline state. Shaders are not translated: their tokens
 * and signatures go to the host as the runtime gave them.
 *
 * This file: the adapter, the formats and the command buffer. The rest:
 * d11_res.c (resources), d11_pipe.c (views, states, shaders, bindings,
 * the work), d11_dev.c (the device and DXGI's functions), d10_dev.c
 * (Direct3D 10's and 10.1's tables), d11_video.c (the 11.1 interface's
 * video functions).
 *
 * Build: guest-tools/build-wddm.cmd (the EWDK's MSBuild, beside the
 * Direct3D 9 driver; docs/build-windows.md "The WDDM driver").
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3dpt11.h"

/* -------------------------------------------------------------- formats */

/* bytes per element (a block for BC formats), block width; 0: unknown */
UINT fmt_bytes(UINT f, UINT *block)
{
    *block = 1;
    switch (f) {
    case 1: case 2: case 3: case 4: return 16;                  /* R32G32B32A32 */
    case 5: case 6: case 7: case 8: return 12;                  /* R32G32B32 */
    case 9: case 10: case 11: case 12: case 13: case 14:        /* R16G16B16A16 */
    case 15: case 16: case 17: case 18:                         /* R32G32 */
    case 19: case 20: case 21: case 22: return 8;               /* R32G8X24, D32S8 */
    case 23: case 24: case 25: case 26:                         /* R10G10B10A2, R11G11B10 */
    case 27: case 28: case 29: case 30: case 31: case 32:       /* R8G8B8A8 */
    case 33: case 34: case 35: case 36: case 37: case 38:       /* R16G16 */
    case 39: case 40: case 41: case 42: case 43:                /* R32 */
    case 44: case 45: case 46: case 47:                         /* R24G8, D24S8 */
    case 67: case 68: case 69:                                  /* R9G9B9E5, R8G8_B8G8, G8R8_G8B8 */
    case 87: case 88: case 89: case 90: case 91: case 92: case 93:  /* B8G8R8A8 / X8, XR_BIAS */
        return 4;
    case 48: case 49: case 50: case 51: case 52:                /* R8G8 */
    case 53: case 54: case 55: case 56: case 57: case 58: case 59:  /* R16 */
    case 85: case 86: case 115:                                 /* B5G6R5, B5G5R5A1, B4G4R4A4 */
        return 2;
    case 60: case 61: case 62: case 63: case 64: case 65:       /* R8, A8 */
        return 1;
    case 70: case 71: case 72: case 79: case 80: case 81:       /* BC1, BC4 */
        *block = 4;
        return 8;
    case 73: case 74: case 75: case 76: case 77: case 78: case 82: case 83: case 84:  /* BC2, BC3, BC5 */
    case 94: case 95: case 96: case 97: case 98: case 99:       /* BC6H, BC7 */
        *block = 4;
        return 16;
    default:
        return 0;
    }
}

/* The Direct3D 9 format of a surface Direct3D 9 code may also see (a swap
 * chain's buffer: the kernel driver's presents, DWM's Direct3D 9 side);
 * 0: none */
UINT fmt_d3d9(UINT f)
{
    switch (f) {
    case 87: case 91: return 21;        /* B8G8R8A8 -> A8R8G8B8 */
    case 88: case 93: return 22;        /* B8G8R8X8 -> X8R8G8B8 */
    case 28: case 29: return 32;        /* R8G8B8A8 -> A8B8G8R8 */
    case 24: return 31;                 /* R10G10B10A2 -> A2B10G10R10 */
    case 10: return 113;                /* R16G16B16A16_FLOAT -> A16B16G16R16F */
    case 85: return 23;                 /* B5G6R5 -> R5G6B5 */
    case 86: return 25;                 /* B5G5R5A1 -> A1R5G5B5 */
    default: return 0;
    }
}

/* the DXGI format of a Direct3D 9 surface another process shared */
UINT fmt_from_d3d9(UINT f)
{
    switch (f) {
    case 21: return 87;
    case 22: return 88;
    case 32: return 28;
    case 31: return 24;
    case 113: return 10;
    case 23: return 85;
    case 25: return 86;
    default: return 0;
    }
}

/* the optional support bits the runtime asks for: what any DXVK host with
 * feature level 11_0 does (track M25). No multisampling yet: with 2x
 * claimed for R32G32B32A32_FLOAT (one quality level) the runtime stopped
 * asking at that answer and failed every device it was making
 * (DXGI_ERROR_DRIVER_INTERNAL_ERROR); which claim it refuses is open */
/* feature level 11_0's typed UAV stores (protocol v34): the formats the
 * level requires them for, which the runtime checks a driver claiming it
 * against (without them every device failed, DRIVER_INTERNAL_ERROR): the
 * float, UNORM, SNORM and integer colour formats from 128 bits down to 8,
 * no sRGB, no BGRA. The host's Vulkan stores all of them. */
static UINT fmt_uav(UINT f)
{
    switch (f) {
    case 2: case 3: case 4:                                     /* R32G32B32A32 FLOAT, UINT, SINT */
    case 10: case 11: case 12: case 13: case 14:                /* R16G16B16A16 */
    case 16: case 17: case 18:                                  /* R32G32 */
    case 24: case 25: case 26:                                  /* R10G10B10A2 UNORM, UINT; R11G11B10_FLOAT */
    case 28: case 30: case 31: case 32:                         /* R8G8B8A8 UNORM, UINT, SNORM, SINT */
    case 34: case 35: case 36: case 37: case 38:                /* R16G16 */
    case 41: case 42: case 43:                                  /* R32 */
    case 49: case 50: case 51: case 52:                         /* R8G8 */
    case 54: case 56: case 57: case 58: case 59:                /* R16 */
    case 61: case 62: case 63: case 64: case 65:                /* R8, A8 */
        return D3D11_1DDI_FORMAT_SUPPORT_UAV_WRITES;
    default:
        return 0;
    }
}

static UINT fmt_support_base(UINT f);

/* multisampling offered to this process (2026-11-27): to every one but
 * those D3D11NoMSAA lists (the A/B). DWM crashed in dwmcore.dll while the
 * claims were wrong, because its device was refused like every other; it
 * composes with them right. */
BOOL g_d11_msaa = TRUE;
/* the host's multisample counts (register set v16, CAPS bits 16-23: the
 * executor asked its own Direct3D 11 device): bits 0-3 2x 4x 8x 16x for
 * formats of up to 64 bits a pixel, 4-7 for 128-bit ones. None from an
 * older host: none offered */
UINT g_d11_host_msaa;

/* the counts a format may claim (bit 0 2x .. bit 3 16x): its size class's */
UINT fmt_msaa_counts(UINT f)
{
    UINT block = 0, bytes = fmt_bytes(f, &block);

    if (!g_d11_msaa) {
        return 0;
    }
    return bytes > 8 ? (g_d11_host_msaa >> 4) & 0xf : g_d11_host_msaa & 0xf;
}

UINT fmt_support(UINT f)
{
    UINT s = fmt_support_base(f);

    if (!fmt_msaa_counts(f)) {
        s &= ~(UINT)(D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET | D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD);
    }
    return s & D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED ? s : s | fmt_uav(f);
}

static UINT fmt_support_base(UINT f)
{
    const UINT S = D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE, R = D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET,
               B = D3D10_DDI_FORMAT_SUPPORT_BLENDABLE,
               /* multisampling (2026-11-27): 2x and 4x for every format
                * with these (d11_check_ms), the counts 10_1 and up ask of
                * a multisampled format and every host's Vulkan has */
               M = D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET, L = D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD;

    switch (f) {
    case 2: case 10: case 11: case 13: case 16: case 24: case 26: case 28: case 29: case 31: case 34: case 35:
    case 37: case 41: case 49: case 51: case 54: case 56: case 58: case 61: case 63: case 65:
    case 85: case 86: case 87: case 88: case 91: case 93:
        return S | R | B | M | L;
    case 115:                                                   /* B4G4R4A4: no multisampling on the host (DXVK, NVIDIA) */
        return S | R | B;
    case 3: case 4: case 12: case 14: case 17: case 18: case 25: case 30: case 32: case 36: case 38:
    case 42: case 43: case 50: case 52: case 57: case 59: case 62: case 64:
        return R | M | L;                                       /* integer: no filtering, no blending */
    case 6: return S;                                           /* R32G32B32_FLOAT */
    case 20: case 40: case 45: case 55:                         /* the depth formats */
        return M;
    case 21: case 46:                                           /* their sampled views */
        return S | L;
    case 22: case 47:                                           /* their stencil views (integer: loaded, not filtered) */
        return L;   /* with none, their family's multisample answers differed from theirs: every device refused */
    case 67: case 68: case 69:
    case 71: case 72: case 74: case 75: case 77: case 78:
    case 80: case 81: case 83: case 84: case 95: case 96: case 98: case 99:
        return S;
    case 89: return D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED;    /* R10G10B10_XR_BIAS_A2_UNORM */
    default:
        return 0;
    }
}

/* ------------------------------------------------------------- adapter */

BOOL g_d11_verbose;
BOOL g_d11_prof;
ULONG64 g_d11_prof_cyc[D11P_N], g_d11_prof_n[D11P_N], g_d11_prof_bytes[D11P_N], g_d11_calls;
static ULONG64 g_prof_tsc0, g_prof_ms0;
static LARGE_INTEGER g_prof_qpc0;

/* the 5 s line: each kind's calls, microseconds and KiB, and the DDI calls */
void d11_prof_tick(void)
{
    static const char *const names[D11P_N] = { "render", "upload", "map", "unmap", "query", "present" };
    ULONG64 now = GetTickCount64(), tsc = d11_ticks();
    LARGE_INTEGER q, f;
    double per_us;
    char line[512];
    int n = 0, k;

    if (!g_d11_prof || now - g_prof_ms0 < 5000) {
        return;
    }
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    per_us = (double)(tsc - g_prof_tsc0) / ((double)(q.QuadPart - g_prof_qpc0.QuadPart) * 1e6 / (double)f.QuadPart);
    n += wsprintfA(line + n, "d3d11: prof %u ms: calls %u", (UINT)(now - g_prof_ms0), (UINT)g_d11_calls);
    for (k = 0; k < D11P_N; k++) {
        n += wsprintfA(line + n, ", %s %u/%uus/%uK", names[k], (UINT)g_d11_prof_n[k],
                       (UINT)(per_us > 0 ? (double)g_d11_prof_cyc[k] / per_us : 0), (UINT)(g_d11_prof_bytes[k] >> 10));
        g_d11_prof_cyc[k] = g_d11_prof_n[k] = g_d11_prof_bytes[k] = 0;
        if (k == 2) {                             /* two lines: the debug register's lines are short */
            umd_log("%s", line);
            n = wsprintfA(line, "d3d11: prof ...");
        }
    }
    umd_log("%s", line);
    g_d11_calls = 0;
    g_prof_ms0 = now;
    g_prof_tsc0 = tsc;
    g_prof_qpc0 = q;
}
static BOOL g_d11_win7;                     /* D3D11Win7: only the Windows 7 interface */
/* feature level 11_0 (track M25: compute and unordered access, protocol
 * v34; tessellation, v35), claimed for every process but those D3D11FL10
 * lists (or "*"), which stay at 10_1 */
static BOOL g_d11_fl11;

SIZE_T APIENTRY d11_calc_device_size(D3D10DDI_HADAPTER h, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *a);
HRESULT APIENTRY d11_create_device(D3D10DDI_HADAPTER h, D3D10DDIARG_CREATEDEVICE *c);
static HRESULT APIENTRY d11_close_adapter(D3D10DDI_HADAPTER h);
static HRESULT APIENTRY d11_supported_versions(D3D10DDI_HADAPTER h, UINT32 *n, UINT64 *v);
static HRESULT APIENTRY d11_adapter_caps(D3D10DDI_HADAPTER h, const D3D10_2DDIARG_GETCAPS *g);

#ifdef __cplusplus
extern "C"
#endif
HRESULT APIENTRY OpenAdapter10_2(D3D10DDIARG_OPENADAPTER *o)
{
    D11_ADAPTER *a;
    D3DDDICB_QUERYADAPTERINFO q;
    HRESULT hr;
    char exe[MAX_PATH], *name;

    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) {
        lstrcpyA(exe, "?");
    }
    name = strrchr(exe, '\\');
    umd_log("d3d11: OpenAdapter10_2 interface 0x%x version 0x%x, process %lu %s", o->Interface, o->Version,
            GetCurrentProcessId(), name ? name + 1 : exe);
    if (umd_listed("D3D11Exclude")) {
        umd_log("d3d11: this process is in D3D11Exclude: refused");
        return E_FAIL;
    }
    g_d11_verbose = umd_listed("D3D11Trace");
    if (g_d11_verbose) {
        umd_log_traced();
    }
    g_d11_win7 = umd_listed("D3D11Win7");
    g_d11_fl11 = !umd_listed("D3D11FL10");
    g_d11_msaa = !umd_listed("D3D11NoMSAA");
    g_d11_prof = umd_listed("D3D11Prof");
    if (g_d11_prof) {
        g_prof_ms0 = GetTickCount64();
        g_prof_tsc0 = d11_ticks();
        QueryPerformanceCounter(&g_prof_qpc0);
    }
    a = (D11_ADAPTER *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*a));
    if (!a) {
        return E_OUTOFMEMORY;
    }
    a->rt = o->hRTAdapter;
    a->cb = o->pAdapterCallbacks;
    q.pPrivateDriverData = &a->info;
    q.PrivateDriverDataSize = sizeof(a->info);
    hr = a->cb->pfnQueryAdapterInfoCb(a->rt.handle, &q);
    if (FAILED(hr) || a->info.magic != D3DPT_UMD_MAGIC || a->info.version != D3DPT_UMD_VERSION || !a->info.d3d ||
        !(a->info.fb_caps & D3DPT_FB_CAP_D3D11)) {
        /* refused: Windows puts Direct3D 11 on the Direct3D 9 driver
         * (d3d10level9) instead, as on a host whose executor has no
         * Direct3D 11 (register set v10's CAP_D3D11) */
        umd_log("d3d11: the kernel driver's info: hr 0x%08x magic 0x%08x, Direct3D %u, caps 0x%x: no Direct3D 11 on this host", hr,
                a->info.magic, a->info.d3d, a->info.fb_caps);
        HeapFree(GetProcessHeap(), 0, a);
        return E_FAIL;
    }
    g_d11_host_msaa = (a->info.fb_caps & D3DPT_FB_CAP_MSAA_MASK) >> D3DPT_FB_CAP_MSAA_SHIFT;
    o->hAdapter.pDrvPrivate = a;
    o->pAdapterFuncs_2->pfnCalcPrivateDeviceSize = d11_calc_device_size;
    o->pAdapterFuncs_2->pfnCreateDevice = d11_create_device;
    o->pAdapterFuncs_2->pfnCloseAdapter = d11_close_adapter;
    o->pAdapterFuncs_2->pfnGetSupportedVersions = d11_supported_versions;
    o->pAdapterFuncs_2->pfnGetCaps = d11_adapter_caps;
    return S_OK;
}

static HRESULT APIENTRY d11_close_adapter(D3D10DDI_HADAPTER h)
{ D11_TRACE("d11_close_adapter");
    HeapFree(GetProcessHeap(), 0, h.pDrvPrivate);
    return S_OK;
}

/* The interfaces of Direct3D 11's DDI this driver has: Windows 8's 11.1
 * and Windows 7's (ADR-029); and Direct3D 10.1's and 10's, which d3d10.dll
 * and d3d10_1.dll ask for (d10_dev.c) */
static HRESULT APIENTRY d11_supported_versions(D3D10DDI_HADAPTER h, UINT32 *n, UINT64 *v)
{ D11_TRACE("d11_supported_versions");
    /* Windows 8's 11.1 interface first: its video functions are what a
     * device made with VIDEO_SUPPORT needs, DWM's among them (the runtime
     * takes it over this WDDM 1.1 kernel driver too); Windows 7's runtime
     * knows only the second. Direct3D 10's: Windows 7's, and those of
     * Vista's DXGI 1.1 update */
    static const UINT64 ours[] = { D3D11_1_DDI_SUPPORTED, D3D11_0_7_DDI_SUPPORTED, D3D10_1_7_DDI_SUPPORTED, D3D10_1_x_DDI_SUPPORTED,
                                   D3D10_0_7_DDI_SUPPORTED, D3D10_0_x_DDI_SUPPORTED };
    const UINT64 *list = g_d11_win7 ? ours + 1 : ours;
    UINT32 count = g_d11_win7 ? ARRAYSIZE(ours) - 1 : ARRAYSIZE(ours);

    (void)h;
    if (v) {
        UINT32 i;

        for (i = 0; i < *n && i < count; i++) {
            v[i] = list[i];
        }
    }
    *n = count;
    return S_OK;
}

static HRESULT APIENTRY d11_adapter_caps(D3D10DDI_HADAPTER h, const D3D10_2DDIARG_GETCAPS *g)
{ D11_TRACE("d11_adapter_caps");
    static ULONG logged;

    (void)h;
    if (g_d11_verbose && logged++ < 16) {
        umd_log("d3d11: GetCaps type %u size %u", g->Type, g->DataSize);
    }
    if (g->pData && g->DataSize) {
        ZeroMemory(g->pData, g->DataSize);
    }
    switch ((UINT)g->Type) {
    case D3D11DDICAPS_THREADING:                  /* not free threaded: the runtime records command lists */
        return S_OK;
    case D3D11DDICAPS_SHADER:
        /* no doubles. With 11_0 claimed, compute shader 4.x with raw and
         * structured buffers on 10_x devices too, which the runtime asks of
         * an 11_0 driver ("Driver doesn't support compute on FL11",
         * DRIVER_INTERNAL_ERROR for every device without it) */
        if (g_d11_fl11 && g->DataSize >= sizeof(D3D11DDI_SHADER_CAPS)) {
            ((D3D11DDI_SHADER_CAPS *)g->pData)->Caps = D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X;
        }
        return S_OK;
    case D3D11DDICAPS_3DPIPELINESUPPORT:
        if (g->DataSize < sizeof(D3D11DDI_3DPIPELINESUPPORT_CAPS)) {
            return E_INVALIDARG;
        }
        /* 10_1: shader model 4.1, cube arrays, a blend per target */
        ((D3D11DDI_3DPIPELINESUPPORT_CAPS *)g->pData)->Caps = D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_0) |
                                                               D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_10_1);
        if (g_d11_fl11) {
            ((D3D11DDI_3DPIPELINESUPPORT_CAPS *)g->pData)->Caps |= D3D11DDI_ENCODE_3DPIPELINESUPPORT_CAP(D3D11DDI_3DPIPELINELEVEL_11_0);
        }
        return S_OK;
    default:
        return S_OK;
    }
}

/* ------------------------------------------------------ the command buffer
 *
 * Records as the Direct3D 9 driver writes them, after a D3DPT_UMD_CMD
 * header; this driver's are D3D11 records. A token naming resources
 * reserves their allocation-list slots and patch locations with its room,
 * so a flush never splits the two. */
#define D11_SLACK D3DPT_WDDM_SLACK  /* what the kernel driver puts in front of the records */
/* and after them, per patch location: a fix-up entry is three ULONGs and
 * a pointer, 24 bytes on x64, whose kernel driver a 32-bit (WoW64)
 * process talks to too (16 lost DWM a full buffer: "Render does not fit
 * one DMA buffer", and every object it created) */
#define D11_FIX 24

void rec_close(D11_DEV *d)
{
    d3dpt_cmd *c;
    d3dpt_d11 *r;
    UINT end;

    if (!d->rec_off) {
        return;
    }
    c = (d3dpt_cmd *)(d->buf + d->rec_off);
    r = (d3dpt_d11 *)(c + 1);
    r->bytes = d->used - (d->rec_off + (UINT)sizeof(*c) + (UINT)sizeof(*r));
    end = d->rec_off + D3DPT_ALIGN8(d->used - d->rec_off);
    ZeroMemory(d->buf + d->used, end - d->used);
    c->size = end - d->rec_off;
    d->used = end;
    d->rec_off = 0;
}

void cmd_reset(D11_DEV *d)
{
    d->used = sizeof(D3DPT_UMD_CMD);
    d->count = 0;
    d->al_n = 0;
    d->pl_n = 0;
    d->rec_off = 0;
    if (++d->al_gen == 0) {                       /* wrapped: no stale entry may match */
        ZeroMemory(d->al_hash, sizeof(d->al_hash));
        d->al_gen = 1;
    }
}

HRESULT cmd_flush(D11_DEV *d)
{
    D3DDDICB_RENDER r;
    D3DPT_UMD_CMD *hdr = (D3DPT_UMD_CMD *)d->buf;
    HRESULT hr;
    static ULONG logged;

    rec_close(d);
    if (!d->count) {
        return S_OK;
    }
    hdr->magic = D3DPT_UMD_CMD_MAGIC;
    hdr->count = d->count;
    ZeroMemory(&r, sizeof(r));
    r.CommandLength = d->used;
    r.NumAllocations = d->al_n;
    r.NumPatchLocations = d->pl_n;
    r.hContext = d->ctx;
    {
        ULONG64 t0 = D11_PROF_T0();

        hr = d->kt->pfnRenderCb(d->rt.handle, &r);
        D11_PROF_ADD(D11P_RENDER, t0, d->used);
    }
    if (FAILED(hr) || (g_d11_verbose && logged < 8)) {
        logged++;
        umd_log("d3d11: Render %u records, %u bytes, %u allocations, %u patches -> 0x%08x", d->count, d->used, d->al_n,
                d->pl_n, hr);
    }
    if (r.pNewCommandBuffer) {
        d->buf = (BYTE *)r.pNewCommandBuffer;
        d->buf_size = r.NewCommandBufferSize;
        d->al = r.pNewAllocationList;
        d->al_size = r.NewAllocationListSize;
        d->pl = r.pNewPatchLocationList;
        d->pl_size = r.NewPatchLocationListSize;
    }
    cmd_reset(d);
    return hr;
}

static BOOL room(D11_DEV *d, UINT bytes, UINT nal)
{
    return d->used + bytes + D11_SLACK + (d->pl_n + nal) * D11_FIX <= d->buf_size && d->al_n + nal <= d->al_size &&
           d->al_n + nal <= D3DPT_WDDM_MAX_LIST && d->pl_n + nal <= d->pl_size;
}

/* room for `bytes` more (records whole) naming up to `nal` allocations,
 * flushing first if the buffer has none: what must stay in one buffer
 * (a RETURN and its record) asks for it together */
void cmd_reserve(D11_DEV *d, UINT bytes, UINT nal)
{
    rec_close(d);
    if (!room(d, bytes, nal)) {
        cmd_flush(d);
    }
}

/* a record outside the D11 stream (the kernel driver's RETURN) of `body` bytes */
void *cmd_rec(D11_DEV *d, UINT op, UINT body, UINT nal)
{
    UINT size = D3DPT_ALIGN8((UINT)sizeof(d3dpt_cmd) + body);
    d3dpt_cmd *c;

    rec_close(d);
    if (!room(d, size, nal)) {
        cmd_flush(d);
    }
    c = (d3dpt_cmd *)(d->buf + d->used);
    ZeroMemory(c, size);
    c->op = op;
    c->size = size;
    d->used += size;
    d->count++;
    return c + 1;
}

/* Room for one token with `body` bytes after its header, naming up to
 * `nal` resources, in the open D11 record (opened here), its first `zero`
 * bytes of body zeroed (an update's payload is written over at once: a
 * game's frame is ~4 MB of them). NULL only when the token can never fit
 * a command buffer. */
void *tok_z(D11_DEV *d, UINT op, UINT body, UINT zero, UINT nal)
{
    UINT size = (UINT)sizeof(d3dpt_d11_tok) + ((body + 3) & ~3u);
    UINT hdr = (UINT)sizeof(d3dpt_cmd) + (UINT)sizeof(d3dpt_d11);
    d3dpt_d11_tok *t;

    if (d->rec_off && !room(d, size + 8, nal)) {
        cmd_flush(d);
    }
    if (!d->rec_off) {
        d3dpt_cmd *c;
        d3dpt_d11 *r;

        if (!room(d, hdr + size + 8, nal)) {
            cmd_flush(d);
            if (!room(d, hdr + size + 8, nal)) {
                umd_log("d3d11: a token of %u bytes never fits a command buffer of %u", size, d->buf_size);
                return NULL;
            }
        }
        d->rec_off = d->used;
        c = (d3dpt_cmd *)(d->buf + d->used);
        c->op = D3DPT_OP_D11;
        c->size = 0;
        r = (d3dpt_d11 *)(c + 1);
        r->ctx = d->host_ctx;
        r->ret_off = 0;
        r->bytes = 0;
        r->flags = 0;
        d->used += hdr;
        d->count++;
    }
    t = (d3dpt_d11_tok *)(d->buf + d->used);
    ZeroMemory(t, sizeof(*t) + (zero < body ? zero : body));
    if (size > sizeof(*t) + body) {
        ZeroMemory((BYTE *)(t + 1) + body, size - sizeof(*t) - body);    /* the padding */
    }
    t->op = (USHORT)op;
    t->size = size;
    d->used += size;
    return t + 1;
}

void *tok(D11_DEV *d, UINT op, UINT body, UINT nal)
{
    return tok_z(d, op, body, body, nal);
}

/* the most body an update token carries (they are split to it) */
UINT tok_max(D11_DEV *d)
{
    UINT n = d->buf_size > D11_SLACK + 4096 ? d->buf_size - D11_SLACK - 4096 : 256;

    return n > 64 * 1024 ? 64 * 1024 : n;
}

/* the field at p (in the current buffer) becomes the allocation's host
 * handle when the kernel driver patches the submission; write says the
 * buffer writes the allocation. A WDDM 1.2 kernel refuses a submission
 * that writes an allocation the device opened read-only (DWM's view of a
 * flip-model swap chain's buffer): STATUS_ACCESS_DENIED, which the runtime
 * turns into a removed device (DXGI_ERROR_ACCESS_DENIED, track M25) */
#define AL_HASH(k) (((UINT)(k) * 2654435761u) >> 22)   /* 1024 buckets */

/* the allocation's slot in this buffer's list, or al_n when it has none */
UINT al_find(D11_DEV *d, D3DKMT_HANDLE kmt)
{
    UINT h = AL_HASH(kmt), i;

    if (d->al_hash[h].gen == d->al_gen && d->al_hash[h].idx < d->al_n && d->al[d->al_hash[h].idx].hAllocation == kmt) {
        return d->al_hash[h].idx;
    }
    for (i = 0; i < d->al_n && d->al[i].hAllocation != kmt; i++) {
    }
    if (i < d->al_n) {
        d->al_hash[h].gen = d->al_gen;
        d->al_hash[h].idx = i;
    }
    return i;
}

/* ... added at the end when it has none (the caller made room) */
UINT al_slot(D11_DEV *d, D3DKMT_HANDLE kmt)
{
    UINT i = al_find(d, kmt), h;

    if (i == d->al_n) {
        ZeroMemory(&d->al[i], sizeof(d->al[i]));
        d->al[i].hAllocation = kmt;
        d->al_n++;
        h = AL_HASH(kmt);
        d->al_hash[h].gen = d->al_gen;
        d->al_hash[h].idx = i;
    }
    return i;
}

void patch_rw(D11_DEV *d, void *p, D3DKMT_HANDLE kmt, BOOL write)
{
    D3DDDI_PATCHLOCATIONLIST *pl;
    UINT i;

    *(UINT *)p = 0;
    if (!kmt) {
        return;
    }
    i = al_slot(d, kmt);
    if (write) {
        d->al[i].WriteOperation = 1;
    }
    pl = &d->pl[d->pl_n++];
    ZeroMemory(pl, sizeof(*pl));
    pl->AllocationIndex = i;
    pl->DriverId = D3DPT_PATCH_HANDLE;
    pl->PatchOffset = (UINT)((BYTE *)p - d->buf);
}

void patch(D11_DEV *d, void *p, D3DKMT_HANDLE kmt)
{
    patch_rw(d, p, kmt, TRUE);
}

/* a resource named by a token: written, as far as the kernel knows, when
 * this device made it (views and draws write through ids no buffer names
 * again); read when another process's, which this device writes only by
 * patch_dst */
void patch_res(D11_DEV *d, void *p, const D11_RES *r)
{
    patch_rw(d, p, r ? r->kmt : 0, r && !r->opened);
}

/* a copy's, an update's or a readback's destination */
void patch_dst(D11_DEV *d, void *p, const D11_RES *r)
{
    patch_rw(d, p, r ? r->kmt : 0, TRUE);
}

void set_error(D11_DEV *d, HRESULT hr)
{
    if (d->errors++ < 16) {
        umd_log("d3d11: SetError 0x%08x", hr);
    }
    d->um->pfnSetErrorCb(d->core, hr);
}
