/*
 * d3dptumd.c: the WDDM user-mode display driver for Direct3D 9 on the
 * d3dpt-vga adapter, Windows 7 (M18 step 2, plan step 6; ADR-022).
 * d3d9.dll loads it by the name the INF writes (UserModeDriverName) and
 * calls OpenAdapter; the D3DDDI functions it then hands out are the
 * Direct3D 9 API one level down.
 *
 * The host side does not change: this driver writes the d3dpt_proto.h
 * records the XP display driver writes (contexts, surfaces in VRAM, the
 * DrawPrimitives2 token stream), but into the runtime's command buffer
 * instead of the device's window. The kernel-mode driver (km/d3dptkmd.c)
 * copies each buffer into a DMA buffer at Render, puts in the host handle
 * and VRAM offset of every allocation it names (patch locations,
 * ../d3dpt_wddm.h) and moves the records into the window at submit time.
 * Every resource in video memory is one allocation, its levels inside it.
 *
 * The caps are the XP driver's DX9 face, from the same code
 * (core/core_caps.c, linked in): the host is the same executor.
 *
 * Its log goes to the kernel-mode driver through an escape and from there
 * to the device's DEBUG register, the QEMU log (doc 15's rule).
 *
 * Build: guest-tools/build-wddm.cmd (the EWDK's MSBuild, Win32, static
 * C runtime; docs/build-windows.md "The WDDM driver").
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
/* the DDI's structures at Windows 7's sizes (the kit defaults to WDDM 2.7) */
#define D3D_UMD_INTERFACE_VERSION D3D_UMD_INTERFACE_VERSION_WIN7
#define DXGKDDI_INTERFACE_VERSION DXGKDDI_INTERFACE_VERSION_WIN7
#include <windows.h>
#include <winternl.h>
#include <stdarg.h>
#include <stdio.h>
#include <d3d9types.h>
#include <d3dumddi.h>
#include <d3dkmthk.h>
#include "../../../../../d3dpt/d3dpt_proto.h"
#include "../d3dpt_wddm.h"
#include "umd_core.h"

/* ------------------------------------------------------------------ log */

static D3DKMT_HANDLE g_kmt_adapter;
static ULONG g_log_lines;

#define UMD_LOG_MAX 4000          /* lines per process; a frame loop must not drown the QEMU log */

void umd_log(const char *fmt, ...)
{
    D3DPT_ESC m;
    D3DKMT_ESCAPE e;
    va_list ap;

    if (g_log_lines >= UMD_LOG_MAX) {
        return;
    }
    if (!g_kmt_adapter) {
        D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME o;

        ZeroMemory(&o, sizeof(o));
        lstrcpyW(o.DeviceName, L"\\\\.\\DISPLAY1");
        if (D3DKMTOpenAdapterFromGdiDisplayName(&o) != 0) {
            return;
        }
        g_kmt_adapter = o.hAdapter;
    }
    g_log_lines++;
    ZeroMemory(&m, sizeof(m));
    m.magic = D3DPT_ESC_MAGIC;
    m.op = D3DPT_ESC_LOG;
    va_start(ap, fmt);
    _vsnprintf_s(m.text, sizeof(m.text), _TRUNCATE, fmt, ap);
    va_end(ap);
    ZeroMemory(&e, sizeof(e));
    e.hAdapter = g_kmt_adapter;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = &m;
    e.PrivateDriverDataSize = sizeof(m);
    D3DKMTEscape(&e);
}

/* -------------------------------------------------------------- adapter */

typedef struct UMD_ADAPTER {
    HANDLE rt;                                    /* the runtime's handle */
    D3DDDI_ADAPTERCALLBACKS cb;
    D3DPT_UMD_INFO info;
} UMD_ADAPTER;

/* A resource: one allocation in video memory (or the application's own
 * memory, the system-memory pool), every subresource at its offset. */
typedef struct UMD_RES {
    HANDLE rt;                                    /* the runtime's handle */
    D3DDDIARG_CREATERESOURCE cr;                  /* as created (pSurfList not kept) */
    D3DKMT_HANDLE kmt;                            /* the allocation, 0 for system memory */
    D3DPT_ALLOC_DESC d;
    UINT nsub;
    struct UMD_SUB { UINT off, pitch, slice; const void *sysmem; } *sub;
    BOOL rendered;                                /* the host drew into it: read back before the CPU sees it */
} UMD_RES;

/* The device: the runtime's callbacks, its one context, and the command
 * buffer being filled. */
typedef struct UMD_DEV {
    HANDLE rt;
    UMD_ADAPTER *ad;
    D3DDDI_DEVICECALLBACKS cb;
    HANDLE ctx;
    /* the command buffer, its allocation and patch lists */
    BYTE *buf;
    UINT buf_size, used, count;
    D3DDDI_ALLOCATIONLIST *al;
    UINT al_size, al_n;
    D3DDDI_PATCHLOCATIONLIST *pl;
    UINT pl_size, pl_n;
    UMD_RES *al_res[D3DPT_WDDM_MAX_LIST];         /* which resource each allocation-list slot names */
    /* the DP2 record being filled at the end of the buffer (0: none):
     * where it starts, where its tokens start */
    UINT dp2_off, dp2_tok;
    /* tokens from before the host's context could exist (no render target
     * yet), replayed into its first DP2 record */
    BYTE *pre;
    UINT pre_n, pre_size;
    /* the host's context */
    ULONG host_ctx;                               /* its handle; created at the first use */
    BOOL ctx_live;
    UMD_RES *tgt[4], *z;                          /* the render targets and the depth buffer */
    BOOL rt_dirty;                                /* target 0 / the depth buffer changed since the host was told */
    /* what the draws read (the host gets them inside each DRAW8) */
    struct UMD_STREAM { UMD_RES *res; const BYTE *um; UINT off, stride, freq; } st[D3DPT_DRAW8_MAX_STREAMS];
    struct { UMD_RES *res; const BYTE *um; UINT stride; } ib;
    ULONG fvf;                                    /* the vertex declaration's handle (bit 0), as DRAW8's fvf */
    ULONG next_decl, next_vs, next_ps;
    ULONG skipped;                                /* draws the host could not be given */
    struct UMD_RES *qres;                         /* the queries' result page (umd_result_res) */
    ULONG next_query;
} UMD_DEV;

static HRESULT APIENTRY umd_get_caps(HANDLE h, CONST D3DDDIARG_GETCAPS *g);
static HRESULT APIENTRY umd_create_device(HANDLE h, D3DDDIARG_CREATEDEVICE *c);
static HRESULT APIENTRY umd_close_adapter(HANDLE h);

/* d3d9.dll's first call: the adapter's facts from the kernel-mode driver,
 * then the caps tables of the XP driver's DX9 face built from them. */
HRESULT APIENTRY OpenAdapter(D3DDDIARG_OPENADAPTER *o)
{
    UMD_ADAPTER *a;
    D3DDDICB_QUERYADAPTERINFO q;
    HRESULT hr;

    umd_log("OpenAdapter interface 0x%x version 0x%x", o->Interface, o->Version);
    a = (UMD_ADAPTER *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*a));
    if (!a) {
        return E_OUTOFMEMORY;
    }
    a->rt = o->hAdapter;
    a->cb = *o->pAdapterCallbacks;
    q.pPrivateDriverData = &a->info;
    q.PrivateDriverDataSize = sizeof(a->info);
    hr = a->cb.pfnQueryAdapterInfoCb(a->rt, &q);
    if (FAILED(hr) || a->info.magic != D3DPT_UMD_MAGIC || a->info.version != D3DPT_UMD_VERSION) {
        umd_log("OpenAdapter: the kernel-mode driver's info: hr 0x%08x magic 0x%08x", hr, a->info.magic);
        HeapFree(GetProcessHeap(), 0, a);
        return E_FAIL;
    }
    umd_log("adapter: register set v%u, ddflags 0x%x, Direct3D %u, VRAM %u MiB, segment %u MiB",
            a->info.fb_version, a->info.ddflags, a->info.d3d, a->info.vram >> 20, a->info.seg_size >> 20);
    umd_caps_init(a->info.ddflags);
    o->hAdapter = a;
    o->pAdapterFuncs->pfnGetCaps = umd_get_caps;
    o->pAdapterFuncs->pfnCreateDevice = umd_create_device;
    o->pAdapterFuncs->pfnCloseAdapter = umd_close_adapter;
    o->DriverVersion = D3D_UMD_INTERFACE_VERSION;
    return S_OK;
}

static HRESULT APIENTRY umd_close_adapter(HANDLE h)
{
    umd_log("CloseAdapter");
    HeapFree(GetProcessHeap(), 0, h);
    return S_OK;
}

/* --------------------------------------------------------------- caps */

static HRESULT APIENTRY umd_get_caps(HANDLE h, CONST D3DDDIARG_GETCAPS *g)
{
    static ULONG logged;
    HRESULT hr = S_OK;
    UINT i;

    (void)h;
    switch (g->Type) {
    case D3DDDICAPS_GETD3D9CAPS:
        hr = umd_caps9(g->pData, g->DataSize);
        break;
    case D3DDDICAPS_GETD3D8CAPS:
        hr = umd_caps8(g->pData, g->DataSize);
        break;
    case D3DDDICAPS_GETD3D3CAPS:
        hr = umd_caps_hal(g->pData, g->DataSize);
        break;
    case D3DDDICAPS_GETD3D5CAPS:
    case D3DDDICAPS_GETD3D6CAPS:
    case D3DDDICAPS_GETD3D7CAPS:
        hr = umd_caps_ext(g->pData, g->DataSize);
        break;
    case D3DDDICAPS_GETFORMATCOUNT:
        *(UINT *)g->pData = umd_format_count();
        break;
    case D3DDDICAPS_GETFORMATDATA: {
        FORMATOP *f = (FORMATOP *)g->pData;
        UINT n = g->DataSize / sizeof(*f);

        for (i = 0; i < n; i++) {
            ULONG fmt, ops, ms;

            if (!umd_format(i, &fmt, &ops, &ms)) {
                break;
            }
            f[i].Format = (D3DDDIFORMAT)fmt;
            f[i].Operations = ops;
            f[i].FlipMsTypes = ms & 0xffff;
            f[i].BltMsTypes = ms >> 16;
            f[i].PrivateFormatBitCount = 0;
        }
        break;
    }
    case D3DDDICAPS_GETMULTISAMPLEQUALITYLEVELS: {
        DDIMULTISAMPLEQUALITYLEVELSDATA *m = (DDIMULTISAMPLEQUALITYLEVELSDATA *)g->pData;

        m->QualityLevels = umd_ms_levels((ULONG)m->Format, (ULONG)m->MsType);
        break;
    }
    case D3DDDICAPS_GETD3DQUERYCOUNT:
        *(UINT *)g->pData = 2;
        break;
    case D3DDDICAPS_GETD3DQUERYDATA: {
        /* the event and occlusion queries, as the XP driver's DX9 face */
        static const D3DDDIQUERYTYPE types[2] = { D3DDDIQUERYTYPE_EVENT, D3DDDIQUERYTYPE_OCCLUSION };
        UINT n = g->DataSize / sizeof(D3DDDIQUERYTYPE);

        for (i = 0; i < n && i < 2; i++) {
            ((D3DDDIQUERYTYPE *)g->pData)[i] = types[i];
        }
        break;
    }
    case D3DDDICAPS_GETGAMMARAMPCAPS:
    case D3DDDICAPS_DDRAW:
    case D3DDDICAPS_DDRAW_MODE_SPECIFIC:
    default:
        /* nothing claimed: no DirectDraw acceleration, no video, no
         * overlays, no content protection (yet) */
        if (g->pData && g->DataSize) {
            ZeroMemory(g->pData, g->DataSize);
        }
        break;
    }
    if (logged < 64) {
        logged++;
        umd_log("GetCaps type %u size %u -> 0x%08x", g->Type, g->DataSize, hr);
    }
    return hr;
}

/* ------------------------------------------------------ the command buffer
 *
 * Records as the XP driver writes them into the window, after a
 * D3DPT_UMD_CMD header. A record naming an allocation reserves its slot
 * in the allocation list and a patch location first, so that a flush in
 * between never splits the two.
 */
#define UMD_SLACK 4096        /* what the kernel-mode driver puts in front of the records */
#define UMD_FIX 16            /* and after them, per patch location (its fix-up packet) */

static void cmd_reset(UMD_DEV *d)
{
    d->used = sizeof(D3DPT_UMD_CMD);
    d->count = 0;
    d->al_n = 0;
    d->pl_n = 0;
    d->dp2_off = 0;
}

/* the open DP2 record is complete: its token bytes, its size padded */
static void dp2_close(UMD_DEV *d)
{
    d3dpt_cmd *c;
    UINT end;

    if (!d->dp2_off) {
        return;
    }
    c = (d3dpt_cmd *)(d->buf + d->dp2_off);
    ((d3dpt_dp2 *)(c + 1))->command_bytes = d->used - d->dp2_tok;
    end = d->dp2_off + D3DPT_ALIGN8(d->used - d->dp2_off);
    ZeroMemory(d->buf + d->used, end - d->used);
    c->size = end - d->dp2_off;
    d->used = end;
    d->dp2_off = 0;
}

/* hand the buffer to the kernel-mode driver (Render) and take the next */
static HRESULT cmd_flush(UMD_DEV *d)
{
    D3DDDICB_RENDER r;
    D3DPT_UMD_CMD *hdr = (D3DPT_UMD_CMD *)d->buf;
    HRESULT hr;
    static ULONG logged;

    dp2_close(d);
    if (!d->count) {
        return S_OK;
    }
    hdr->magic = D3DPT_UMD_CMD_MAGIC;
    hdr->count = d->count;
    ZeroMemory(&r, sizeof(r));
    r.CommandLength = d->used;
    r.CommandOffset = 0;
    r.NumAllocations = d->al_n;
    r.NumPatchLocations = d->pl_n;
    r.hContext = d->ctx;
    hr = d->cb.pfnRenderCb(d->rt, &r);
    if (logged < 16) {
        logged++;
        umd_log("Render: %u records, %u bytes, %u allocations, %u patches -> 0x%08x; next buffer %u bytes",
                d->count, d->used, d->al_n, d->pl_n, hr, r.NewCommandBufferSize);
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

/* room for a record of `bytes` (whole) naming up to `nal` allocations */
static void *cmd_rec(UMD_DEV *d, UINT op, UINT body, UINT extra, UINT nal)
{
    UINT size = D3DPT_ALIGN8((UINT)sizeof(d3dpt_cmd) + body + extra);
    d3dpt_cmd *c;

    dp2_close(d);
    if (d->used + size + UMD_SLACK + (d->pl_n + nal) * UMD_FIX > d->buf_size || d->al_n + nal > d->al_size ||
        d->al_n + nal > D3DPT_WDDM_MAX_LIST || d->pl_n + nal > d->pl_size) {
        cmd_flush(d);
        if (d->used + size + UMD_SLACK > d->buf_size) {
            umd_log("a record of %u bytes never fits a command buffer of %u", size, d->buf_size);
            return NULL;
        }
    }
    c = (d3dpt_cmd *)(d->buf + d->used);
    ZeroMemory(c, size);
    c->op = op;
    c->size = size;
    d->used += size;
    d->count++;
    return c + 1;
}

/* the field at p (inside the current buffer) gets the resource's host
 * handle or VRAM offset (+ add) when the kernel-mode driver patches */
static void cmd_patch(UMD_DEV *d, void *p, UMD_RES *r, UINT kind, UINT add)
{
    D3DDDI_PATCHLOCATIONLIST *pl;
    UINT i;

    for (i = 0; i < d->al_n && d->al_res[i] != r; i++) {
    }
    if (i == d->al_n) {
        ZeroMemory(&d->al[i], sizeof(d->al[i]));
        d->al[i].hAllocation = r->kmt;
        d->al[i].WriteOperation = 1;
        d->al_res[i] = r;
        d->al_n++;
    }
    pl = &d->pl[d->pl_n++];
    ZeroMemory(pl, sizeof(*pl));
    pl->AllocationIndex = i;
    pl->DriverId = kind;
    pl->AllocationOffset = add;
    pl->PatchOffset = (UINT)((BYTE *)p - d->buf);
    *(ULONG *)p = 0;
}

/* ------------------------------------------------------------ resources */

static ULONG umd_res_caps(const D3DDDIARG_CREATERESOURCE *c)
{
    ULONG caps = 0;

    if (c->Flags.VertexBuffer || c->Flags.IndexBuffer) {
        return D3DPT_VS_BUFFER;
    }
    if (c->Flags.Texture || c->Flags.CubeMap || c->Flags.Volume) caps |= D3DPT_VS_TEXTURE;
    if (c->Flags.CubeMap) caps |= D3DPT_VS_CUBE;
    if (c->Flags.Volume) caps |= D3DPT_VS_VOLUME;
    if (c->Flags.RenderTarget) caps |= D3DPT_VS_RENDER_TARGET;
    if (c->Flags.ZBuffer) caps |= D3DPT_VS_ZBUFFER;
    if (c->Flags.Primary) caps |= D3DPT_VS_PRIMARY | D3DPT_VS_RENDER_TARGET;
    /* A plain surface in video memory (a DirectDraw flip chain's back
     * buffer, an offscreen plain one): DX7 renders into it without its
     * runtime saying so, so the host knows it as a possible target, as the
     * XP driver's DirectDraw surfaces are */
    if (!caps && c->SurfCount == 1 && c->Pool != D3DDDIPOOL_SYSTEMMEM && umd_row_bytes((ULONG)c->Format, 1) &&
        !umd_is_dxt((ULONG)c->Format)) {
        caps = D3DPT_VS_RENDER_TARGET;
    }
    if (c->Flags.AutogenMipmap && (caps & D3DPT_VS_TEXTURE)) caps |= D3DPT_VS_AUTOGEN;
    if (c->MultisampleType >= D3DDDIMULTISAMPLE_2_SAMPLES && c->MultisampleType <= D3DDDIMULTISAMPLE_16_SAMPLES) {
        caps |= (ULONG)c->MultisampleType << D3DPT_VS_SAMPLES_SHIFT;
    }
    return caps;
}

/* Every subresource one after the other, dword-aligned rows (block rows
 * for DXT), as the XP driver lays out a lightweight mip chain; a cube's
 * faces face-major, a volume's slices inside their level. Fills the
 * host's tail and returns the bytes. */
static UINT umd_layout(UMD_RES *r, const D3DDDIARG_CREATERESOURCE *c)
{
    D3DPT_ALLOC_DESC *d = &r->d;
    UINT total = 0, i, levels = c->MipLevels ? c->MipLevels : 1;
    BOOL buffer = (d->caps & D3DPT_VS_BUFFER) != 0;

    for (i = 0; i < r->nsub; i++) {
        const D3DDDI_SURFACEINFO *s = &c->pSurfList[i];
        UINT rb = buffer ? s->Width : umd_row_bytes((ULONG)c->Format, s->Width);
        UINT rows = buffer ? 1 : umd_rows((ULONG)c->Format, s->Height);
        UINT depth = s->Depth ? s->Depth : 1;

        if (!rb) {
            rb = s->Width * 4;        /* a format the host does not mirror: keep the memory sane */
        }
        if (!umd_is_dxt((ULONG)c->Format)) {
            rb = (rb + 3) & ~3u;
        }
        r->sub[i].off = total;
        r->sub[i].pitch = rb;
        r->sub[i].slice = rb * rows;
        total += rb * rows * depth;
        total = (total + 15) & ~15u;
    }
    d->w = c->pSurfList[0].Width;
    d->h = buffer ? 1 : c->pSurfList[0].Height;
    d->pitch = r->sub[0].pitch;
    d->format = buffer ? 0 : (ULONG)c->Format;
    d->levels = buffer ? 1 : levels;
    d->nlv = 0;
    if (!buffer) {
        for (i = 1; i < r->nsub && d->nlv < D3DPT_ALLOC_MAX_LV; i++) {
            d->lv[d->nlv].a = r->sub[i].off;
            d->lv[d->nlv].b = r->sub[i].pitch;
            d->nlv++;
        }
        if ((d->caps & D3DPT_VS_VOLUME) && d->nlv < D3DPT_ALLOC_MAX_LV) {
            d->lv[d->nlv].a = c->pSurfList[0].Depth;
            d->lv[d->nlv].b = r->sub[0].slice;
            d->nlv++;
        }
    }
    return total;
}

static HRESULT APIENTRY umd_create_resource(HANDLE h, D3DDDIARG_CREATERESOURCE *c)
{
    UMD_DEV *dev = (UMD_DEV *)h;
    UMD_RES *r;
    UINT i, bytes;
    HRESULT hr;

    r = (UMD_RES *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r));
    if (!r) {
        return E_OUTOFMEMORY;
    }
    r->sub = (struct UMD_SUB *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (c->SurfCount ? c->SurfCount : 1) * sizeof(*r->sub));
    if (!r->sub) {
        HeapFree(GetProcessHeap(), 0, r);
        return E_OUTOFMEMORY;
    }
    r->rt = c->hResource;
    r->cr = *c;
    r->cr.pSurfList = NULL;
    r->nsub = c->SurfCount;
    r->d.magic = D3DPT_ALLOC_MAGIC;
    r->d.kind = D3DPT_ALLOC_D3D;
    r->d.caps = umd_res_caps(c);
    r->d.refresh_num = c->RefreshRate.Numerator;
    r->d.refresh_den = c->RefreshRate.Denominator;
    r->d.source = c->VidPnSourceId;
    r->d.primary = c->Flags.Primary;
    bytes = umd_layout(r, c);
    r->d.size = bytes;
    r->d.bpp = r->d.h ? r->sub[0].pitch / (r->d.w ? r->d.w : 1) : 0;

    if (c->Pool == D3DDDIPOOL_SYSTEMMEM) {
        /* the application's memory: no allocation, the host never sees it
         * (a blit or an update copies it into video memory) */
        for (i = 0; i < r->nsub; i++) {
            r->sub[i].sysmem = c->pSurfList[i].pSysMem;
            r->sub[i].pitch = c->pSurfList[i].SysMemPitch;
            r->sub[i].slice = c->pSurfList[i].SysMemSlicePitch;
        }
        hr = S_OK;
    } else {
        D3DDDICB_ALLOCATE al;
        D3DDDI_ALLOCATIONINFO ai;

        ZeroMemory(&al, sizeof(al));
        ZeroMemory(&ai, sizeof(ai));
        ai.pPrivateDriverData = &r->d;
        ai.PrivateDriverDataSize = sizeof(r->d);
        ai.VidPnSourceId = c->VidPnSourceId;
        ai.Flags.Primary = c->Flags.Primary;
        al.NumAllocations = 1;
        al.pAllocationInfo = &ai;
        hr = dev->cb.pfnAllocateCb(dev->rt, &al);
        r->kmt = ai.hAllocation;
    }
    umd_log("CreateResource fmt %u pool %u flags 0x%08x %ux%ux%u surfaces %u levels %u ms %u: caps 0x%x bytes %u -> 0x%08x",
            c->Format, c->Pool, c->Flags.Value, c->pSurfList[0].Width, c->pSurfList[0].Height, c->pSurfList[0].Depth,
            c->SurfCount, c->MipLevels, c->MultisampleType, r->d.caps, bytes, hr);
    if (FAILED(hr)) {
        HeapFree(GetProcessHeap(), 0, r->sub);
        HeapFree(GetProcessHeap(), 0, r);
        return hr;
    }
    c->hResource = r;
    return S_OK;
}

static HRESULT APIENTRY umd_destroy_resource(HANDLE h, HANDLE hres)
{
    UMD_DEV *dev = (UMD_DEV *)h;
    UMD_RES *r = (UMD_RES *)hres;
    UINT i;

    cmd_flush(dev);                   /* nothing queued may name it once it is gone */
    for (i = 0; i < 4; i++) {
        if (dev->tgt[i] == r) dev->tgt[i] = NULL;
    }
    for (i = 0; i < D3DPT_DRAW8_MAX_STREAMS; i++) {
        if (dev->st[i].res == r) dev->st[i].res = NULL;
    }
    if (dev->ib.res == r) dev->ib.res = NULL;
    if (dev->z == r) dev->z = NULL;
    if (r->kmt) {
        D3DDDICB_DEALLOCATE da;

        ZeroMemory(&da, sizeof(da));
        da.NumAllocations = 1;
        da.HandleList = &r->kmt;
        dev->cb.pfnDeallocateCb(dev->rt, &da);
    }
    HeapFree(GetProcessHeap(), 0, r->sub);
    HeapFree(GetProcessHeap(), 0, r);
    return S_OK;
}

/* A render target the host drew into is read back into its VRAM before
 * the CPU or a present reads it. */
static void umd_readback(UMD_DEV *d, UMD_RES *r)
{
    d3dpt_sync *s;

    if (!r->rendered || !r->kmt) {
        return;
    }
    s = (d3dpt_sync *)cmd_rec(d, D3DPT_OP_READBACK, sizeof(*s), 0, 1);
    if (s) {
        cmd_patch(d, &s->handle, r, D3DPT_PATCH_HANDLE, 0);
        s->ret_off = 0;
    }
}

static HRESULT APIENTRY umd_lock(HANDLE h, D3DDDIARG_LOCK *l)
{
    UMD_DEV *dev = (UMD_DEV *)h;
    UMD_RES *r = (UMD_RES *)l->hResource;
    const struct UMD_SUB *s;
    D3DDDICB_LOCK lk;
    BYTE *base;
    HRESULT hr;

    if (l->SubResourceIndex >= r->nsub) {
        return E_INVALIDARG;
    }
    s = &r->sub[l->SubResourceIndex];
    if (s->sysmem) {
        base = (BYTE *)s->sysmem;
    } else {
        /* everything queued runs before the CPU touches the memory (the
         * host reads textures and buffers from VRAM at draw time) */
        if (!l->Flags.WriteOnly && !l->Flags.Discard) {
            umd_readback(dev, r);
        }
        if (r->d.caps & (D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY)) {
            static ULONG logged;

            if (logged++ < 16) {
                umd_log("Lock of a target %p: flags 0x%x, rendered %u", r, l->Flags.Value, r->rendered);
            }
        }
        cmd_flush(dev);
        ZeroMemory(&lk, sizeof(lk));
        lk.hAllocation = r->kmt;
        lk.Flags.ReadOnly = l->Flags.ReadOnly;
        lk.Flags.WriteOnly = l->Flags.WriteOnly;
        lk.Flags.DonotWait = l->Flags.DoNotWait;
        hr = dev->cb.pfnLockCb(dev->rt, &lk);
        if (FAILED(hr)) {
            return hr;
        }
        base = (BYTE *)lk.pData + s->off;
    }
    l->Pitch = s->pitch;
    l->SlicePitch = s->slice;
    if (l->Flags.RangeValid) {
        base += l->Range.Offset;
    } else if (l->Flags.AreaValid) {
        UINT rb = umd_row_bytes((ULONG)r->cr.Format, 1);

        base += (umd_is_dxt((ULONG)r->cr.Format) ? l->Area.top / 4 : l->Area.top) * s->pitch +
                (umd_is_dxt((ULONG)r->cr.Format) ? (l->Area.left / 4) * umd_row_bytes((ULONG)r->cr.Format, 4) : l->Area.left * rb);
    } else if (l->Flags.BoxValid) {
        base += l->Box.Front * s->slice + l->Box.Top * s->pitch + l->Box.Left * umd_row_bytes((ULONG)r->cr.Format, 1);
    }
    l->pSurfData = base;
    return S_OK;
}

static HRESULT APIENTRY umd_unlock(HANDLE h, CONST D3DDDIARG_UNLOCK *u)
{
    UMD_DEV *dev = (UMD_DEV *)h;
    UMD_RES *r = (UMD_RES *)u->hResource;
    D3DDDICB_UNLOCK ul;
    d3dpt_handle *dh;

    if (u->SubResourceIndex >= r->nsub || r->sub[u->SubResourceIndex].sysmem) {
        return S_OK;
    }
    ZeroMemory(&ul, sizeof(ul));
    ul.NumAllocations = 1;
    ul.phAllocations = &r->kmt;
    dev->cb.pfnUnlockCb(dev->rt, &ul);
    /* the host re-reads it before its next use */
    if (r->d.caps) {
        dh = (d3dpt_handle *)cmd_rec(dev, D3DPT_OP_VRAM_DIRTY, sizeof(*dh), 0, 1);
        if (dh) {
            cmd_patch(dev, &dh->handle, r, D3DPT_PATCH_HANDLE, 0);
        }
    }
    return S_OK;
}

/* ---------------------------------------------------- the host's context */

static BYTE *dp2_put(UMD_DEV *d, UINT op, UINT count, UINT bytes);

/* The host's context for this device: CTX_CREATE once a render target
 * exists, then the DP2 tokens that came before it. Its handle is this
 * process's and this device's: the host's table is the whole guest's. */
static BOOL umd_ctx(UMD_DEV *d)
{
    d3dpt_ctx_create *c;

    if (d->ctx_live) {
        return TRUE;
    }
    if (!d->tgt[0] || !d->tgt[0]->kmt) {
        return FALSE;
    }
    c = (d3dpt_ctx_create *)cmd_rec(d, D3DPT_OP_CTX_CREATE, sizeof(*c), 0, 2);
    if (!c) {
        return FALSE;
    }
    c->handle = d->host_ctx;
    c->ret_off = 0;
    cmd_patch(d, &c->rt, d->tgt[0], D3DPT_PATCH_HANDLE, 0);
    if (d->z && d->z->kmt) {
        cmd_patch(d, &c->z, d->z, D3DPT_PATCH_HANDLE, 0);
    }
    c->flags = 0;
    d->ctx_live = TRUE;
    d->rt_dirty = FALSE;
    umd_log("host context 0x%08x, %u bytes of earlier tokens", d->host_ctx, d->pre_n);
    if (d->pre_n) {
        UINT off = 0;

        /* each token whole, as dp2_tok wrote it */
        while (off + 4 <= d->pre_n) {
            UINT len = *(UINT *)(d->pre + off);
            BYTE *p = dp2_put(d, d->pre[off + 4], *(USHORT *)(d->pre + off + 6), len - 4);

            if (p) {
                CopyMemory(p, d->pre + off + 8, len - 4);
            }
            off += 4 + len;
        }
        d->pre_n = 0;
    }
    return TRUE;
}

/* Room for one token of `bytes` after its 4-byte header in the open DP2
 * record (opened here, after the buffer flushed when it is full), with
 * `nal` allocation slots and patch locations free. */
static BYTE *dp2_put_nal(UMD_DEV *d, UINT op, UINT count, UINT bytes, UINT nal)
{
    BYTE *p;

    if (d->dp2_off && (d->used + 4 + bytes + 8 + UMD_SLACK + (d->pl_n + nal) * UMD_FIX > d->buf_size || d->al_n + nal > d->al_size ||
                       d->al_n + nal > D3DPT_WDDM_MAX_LIST || d->pl_n + nal > d->pl_size)) {
        cmd_flush(d);
    }
    if (!d->dp2_off) {
        d3dpt_dp2 *a = (d3dpt_dp2 *)cmd_rec(d, D3DPT_OP_DP2, sizeof(*a), 0, nal);

        if (!a) {
            return NULL;
        }
        a->ctx = d->host_ctx;
        a->ret_off = 0;
        d->dp2_off = (UINT)((BYTE *)a - sizeof(d3dpt_cmd) - d->buf);
        d->dp2_tok = d->used;
    }
    if (d->used + 4 + bytes + 8 + UMD_SLACK > d->buf_size) {
        umd_log("a DP2 token of %u bytes never fits a command buffer of %u", bytes, d->buf_size);
        return NULL;
    }
    p = d->buf + d->used;
    p[0] = (BYTE)op;
    p[1] = 0;
    *(USHORT *)(p + 2) = (USHORT)count;
    d->used += 4 + bytes;
    return p + 4;
}

static BYTE *dp2_put(UMD_DEV *d, UINT op, UINT count, UINT bytes)
{
    return dp2_put_nal(d, op, count, bytes, 0);
}

/* A DP2 token (D3DHAL_DP2COMMAND, the DDI's numbering): into the context's
 * stream, or kept until the context exists. A token naming allocations
 * needs the context (a patch location belongs to a submission). */
static BYTE *dp2_tok(UMD_DEV *d, UINT op, UINT count, UINT bytes, UINT nal)
{
    BYTE *p;

    if (umd_ctx(d)) {
        if (d->rt_dirty) {
            UINT *pair;

            d->rt_dirty = FALSE;
            pair = (UINT *)dp2_put_nal(d, 41, 1, 8, 2);     /* SETRENDERTARGET: target 0, depth */
            if (pair) {
                if (d->tgt[0] && d->tgt[0]->kmt) cmd_patch(d, &pair[0], d->tgt[0], D3DPT_PATCH_HANDLE, 0);
                if (d->z && d->z->kmt) cmd_patch(d, &pair[1], d->z, D3DPT_PATCH_HANDLE, 0);
            }
        }
        return dp2_put_nal(d, op, count, bytes, nal);
    }
    if (nal) {
        umd_log("token %u names a resource before there is a render target: dropped", op);
        return NULL;
    }
    if (d->pre_n + 8 + bytes > d->pre_size) {
        UINT n = (d->pre_n + 8 + bytes) * 2 + 4096;
        BYTE *q = (BYTE *)(d->pre ? HeapReAlloc(GetProcessHeap(), 0, d->pre, n) : HeapAlloc(GetProcessHeap(), 0, n));

        if (!q) {
            return NULL;
        }
        d->pre = q;
        d->pre_size = n;
    }
    p = d->pre + d->pre_n;
    *(UINT *)p = 4 + bytes;
    p[4] = (BYTE)op;
    p[5] = 0;
    *(USHORT *)(p + 6) = (USHORT)count;
    d->pre_n += 8 + bytes;
    return p + 8;
}

/* the common case: one token whose body is a copy of the DDI argument */
static HRESULT dp2_copy(UMD_DEV *d, UINT op, const void *body, UINT bytes)
{
    BYTE *p = dp2_tok(d, op, 1, bytes, 0);

    if (p) {
        CopyMemory(p, body, bytes);
    }
    return S_OK;
}

/* the render targets the host draws into from here on */
static void umd_drawn(UMD_DEV *d)
{
    UINT i;

    for (i = 0; i < 4; i++) {
        if (d->tgt[i]) {
            d->tgt[i]->rendered = TRUE;
        }
    }
}

static HRESULT APIENTRY umd_set_render_target(HANDLE h, CONST D3DDDIARG_SETRENDERTARGET *s)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *r = (UMD_RES *)s->hRenderTarget;
    static ULONG logged;
    UINT *e;

    if (s->RenderTargetIndex >= 4) {
        return E_INVALIDARG;
    }
    if (logged < 32) {
        logged++;
        umd_log("SetRenderTarget %u: %p, allocation 0x%x, caps 0x%x, %ux%u", s->RenderTargetIndex, r, r ? r->kmt : 0,
                r ? r->d.caps : 0, r ? r->d.w : 0, r ? r->d.h : 0);
    }
    if (s->SubResourceIndex) {
        umd_log("todo: SetRenderTarget %u on subresource %u (a level or a face)", s->RenderTargetIndex, s->SubResourceIndex);
    }
    d->tgt[s->RenderTargetIndex] = r;
    if (s->RenderTargetIndex == 0) {
        d->rt_dirty = TRUE;
        return S_OK;
    }
    /* targets 1..3 (v17): the runtime's own SETRENDERTARGET2 */
    e = (UINT *)dp2_tok(d, 85, 1, 8, 1);
    if (e) {
        e[0] = s->RenderTargetIndex;
        if (r && r->kmt) {
            cmd_patch(d, &e[1], r, D3DPT_PATCH_HANDLE, 0);
        } else {
            e[1] = 0;
        }
    }
    return S_OK;
}

static HRESULT APIENTRY umd_set_depth_stencil(HANDLE h, CONST D3DDDIARG_SETDEPTHSTENCIL *s)
{
    UMD_DEV *d = (UMD_DEV *)h;

    d->z = (UMD_RES *)s->hZBuffer;
    d->rt_dirty = TRUE;
    return S_OK;
}

/* CLEAR: D3DHAL_DP2CLEAR (flags, colour, depth, stencil) and the rects */
static HRESULT APIENTRY umd_clear(HANDLE h, CONST D3DDDIARG_CLEAR *c, UINT nrect, CONST RECT *rects)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UINT *k = (UINT *)dp2_tok(d, 42, nrect, 16 + nrect * sizeof(RECT), 0);

    if (!k) {
        return S_OK;
    }
    k[0] = c->Flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
    k[1] = c->FillColor;
    ((FLOAT *)k)[2] = c->FillDepth;
    k[3] = c->FillStencil;
    if (nrect) {
        CopyMemory(k + 4, rects, nrect * sizeof(RECT));
    }
    if (c->Flags & D3DCLEAR_TARGET) {
        umd_drawn(d);
    }
    return S_OK;
}

/* -------------------------------------------------------------- state */

static HRESULT APIENTRY umd_set_render_state(HANDLE h, CONST D3DDDIARG_RENDERSTATE *s)
{
    return dp2_copy((UMD_DEV *)h, 8, s, 8);
}

/* The host reads the filter stage states in the DDI's DX7 numbering, as
 * the XP driver hands them over (core_dp2.c, tss_dx8_filter): d3d9.dll
 * gives D3DTEXF_* here as it did there. */
static UINT tss_filter(UINT state, UINT v)
{
    if (state == 16) {                                  /* MAGFILTER */
        return v == 3 ? 5 : v == 4 ? 3 : v == 5 ? 4 : v;
    }
    if (state == 18) {                                  /* MIPFILTER */
        return v <= 2 ? v + 1 : v;
    }
    return v;
}

static HRESULT APIENTRY umd_set_texture_stage_state(HANDLE h, CONST D3DDDIARG_TEXTURESTAGESTATE *s)
{
    USHORT *e = (USHORT *)dp2_tok((UMD_DEV *)h, 25, 1, 8, 0);

    if (e) {
        e[0] = (USHORT)s->Stage;
        e[1] = (USHORT)s->State;
        *(UINT *)(e + 2) = tss_filter((UINT)s->State, s->Value);
    }
    return S_OK;
}

/* a texture is stage state 0 (TEXTUREMAP), its value the host's handle */
static HRESULT APIENTRY umd_set_texture(HANDLE h, UINT stage, HANDLE tex)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *r = (UMD_RES *)tex;
    USHORT *e = (USHORT *)dp2_tok(d, 25, 1, 8, r && r->kmt ? 1 : 0);

    if (e) {
        e[0] = (USHORT)stage;
        e[1] = 0;
        if (r && r->kmt) {
            cmd_patch(d, e + 2, r, D3DPT_PATCH_HANDLE, 0);
        } else {
            *(UINT *)(e + 2) = 0;
        }
    }
    return S_OK;
}

static HRESULT APIENTRY umd_update_winfo(HANDLE h, CONST D3DDDIARG_WINFO *a)
{
    return dp2_copy((UMD_DEV *)h, 29, a, 8);
}

static HRESULT APIENTRY umd_set_viewport(HANDLE h, CONST D3DDDIARG_VIEWPORTINFO *a)
{
    return dp2_copy((UMD_DEV *)h, 28, a, 16);
}

static HRESULT APIENTRY umd_set_zrange(HANDLE h, CONST D3DDDIARG_ZRANGE *a)
{
    return dp2_copy((UMD_DEV *)h, 32, a, 8);
}

static HRESULT APIENTRY umd_set_transform(HANDLE h, CONST D3DDDIARG_SETTRANSFORM *a)
{
    return dp2_copy((UMD_DEV *)h, 36, a, 68);
}

static HRESULT APIENTRY umd_multiply_transform(HANDLE h, CONST D3DDDIARG_MULTIPLYTRANSFORM *a)
{
    return dp2_copy((UMD_DEV *)h, 65, a, 68);
}

static HRESULT APIENTRY umd_set_material(HANDLE h, CONST D3DDDIARG_SETMATERIAL *a)
{
    return dp2_copy((UMD_DEV *)h, 33, a, 68);
}

static HRESULT APIENTRY umd_create_light(HANDLE h, CONST D3DDDIARG_CREATELIGHT *a)
{
    return dp2_copy((UMD_DEV *)h, 35, a, 4);
}

static HRESULT APIENTRY umd_destroy_light(HANDLE h, CONST D3DDDIARG_DESTROYLIGHT *a)
{
    (void)h; (void)a;           /* the DDI has no token: a light the runtime no longer uses stays disabled */
    return S_OK;
}

/* SETLIGHT: index, kind (enable, disable, data: the same numbers), and a
 * D3DLIGHT9 after a DATA one */
static HRESULT APIENTRY umd_set_light(HANDLE h, CONST D3DDDIARG_SETLIGHT *a, CONST D3DDDI_LIGHT *l)
{
    BOOL data = a->DataType == D3DDDI_SETLIGHT_DATA;
    UINT *e = (UINT *)dp2_tok((UMD_DEV *)h, 34, 1, 8 + (data ? 104 : 0), 0);

    if (e) {
        e[0] = a->Index;
        e[1] = (UINT)a->DataType;
        if (data) {
            CopyMemory(e + 2, l, 104);
        }
    }
    return S_OK;
}

static HRESULT APIENTRY umd_set_clip_plane(HANDLE h, CONST D3DDDIARG_SETCLIPPLANE *a)
{
    return dp2_copy((UMD_DEV *)h, 44, a, 20);
}

static HRESULT APIENTRY umd_set_scissor_rect(HANDLE h, CONST RECT *r)
{
    return dp2_copy((UMD_DEV *)h, 79, r, 16);
}

/* ------------------------------------------------------------- shaders
 *
 * Created, set and deleted by the DDI's own tokens, which the host keeps
 * per context. A declaration's handle is odd: DRAW8's fvf field takes it
 * where it would take an FVF (core_dp2.c). */

static HRESULT APIENTRY umd_create_vertex_shader_decl(HANDLE h, D3DDDIARG_CREATEVERTEXSHADERDECL *a,
                                                      CONST D3DDDIVERTEXELEMENT *el)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UINT *e = (UINT *)dp2_tok(d, 71, 1, 8 + a->NumVertexElements * 8, 0);
    ULONG hd = (++d->next_decl << 1) | 1;

    if (e) {
        e[0] = hd;
        e[1] = a->NumVertexElements;
        CopyMemory(e + 2, el, a->NumVertexElements * 8);
    }
    a->ShaderHandle = (HANDLE)(ULONG_PTR)hd;
    return S_OK;
}

static HRESULT APIENTRY umd_set_vertex_shader_decl(HANDLE h, HANDLE decl)
{
    ((UMD_DEV *)h)->fvf = (ULONG)(ULONG_PTR)decl;
    return S_OK;
}

static HRESULT APIENTRY umd_delete_vertex_shader_decl(HANDLE h, HANDLE decl)
{
    UMD_DEV *d = (UMD_DEV *)h;
    ULONG v = (ULONG)(ULONG_PTR)decl;

    if (d->fvf == v) {
        d->fvf = 0;
    }
    return dp2_copy(d, 72, &v, 4);
}

static HRESULT APIENTRY umd_create_vertex_shader_func(HANDLE h, D3DDDIARG_CREATEVERTEXSHADERFUNC *a, CONST UINT *code)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UINT *e = (UINT *)dp2_tok(d, 74, 1, 8 + a->Size, 0);
    ULONG hs = 0x10000u + (++d->next_vs << 1);

    if (e) {
        e[0] = hs;
        e[1] = a->Size;
        CopyMemory(e + 2, code, a->Size);
    }
    a->ShaderHandle = (HANDLE)(ULONG_PTR)hs;
    return S_OK;
}

static HRESULT APIENTRY umd_set_vertex_shader_func(HANDLE h, HANDLE vs)
{
    ULONG v = (ULONG)(ULONG_PTR)vs;

    return dp2_copy((UMD_DEV *)h, 76, &v, 4);
}

static HRESULT APIENTRY umd_delete_vertex_shader_func(HANDLE h, HANDLE vs)
{
    ULONG v = (ULONG)(ULONG_PTR)vs;

    return dp2_copy((UMD_DEV *)h, 75, &v, 4);
}

static HRESULT APIENTRY umd_create_pixel_shader(HANDLE h, D3DDDIARG_CREATEPIXELSHADER *a, CONST UINT *code)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UINT *e = (UINT *)dp2_tok(d, 54, 1, 8 + a->CodeSize, 0);
    ULONG hs = ++d->next_ps;

    if (e) {
        e[0] = hs;
        e[1] = a->CodeSize;
        CopyMemory(e + 2, code, a->CodeSize);
    }
    a->ShaderHandle = (HANDLE)(ULONG_PTR)hs;
    return S_OK;
}

static HRESULT APIENTRY umd_set_pixel_shader(HANDLE h, HANDLE ps)
{
    ULONG v = (ULONG)(ULONG_PTR)ps;

    return dp2_copy((UMD_DEV *)h, 56, &v, 4);
}

static HRESULT APIENTRY umd_delete_pixel_shader(HANDLE h, HANDLE ps)
{
    ULONG v = (ULONG)(ULONG_PTR)ps;

    return dp2_copy((UMD_DEV *)h, 55, &v, 4);
}

/* the constants: register, count, then count vectors (16 bytes each) or
 * BOOLs (4) */
static HRESULT dp2_const(UMD_DEV *d, UINT op, UINT reg, UINT count, const void *data, UINT each)
{
    UINT *e = (UINT *)dp2_tok(d, op, 1, 8 + count * each, 0);

    if (e) {
        e[0] = reg;
        e[1] = count;
        CopyMemory(e + 2, data, count * each);
    }
    return S_OK;
}

static HRESULT APIENTRY umd_set_vertex_shader_const(HANDLE h, CONST D3DDDIARG_SETVERTEXSHADERCONST *a, CONST VOID *v)
{
    return dp2_const((UMD_DEV *)h, 48, a->Register, a->Count, v, 16);
}

static HRESULT APIENTRY umd_set_pixel_shader_const(HANDLE h, CONST D3DDDIARG_SETPIXELSHADERCONST *a, CONST FLOAT *v)
{
    return dp2_const((UMD_DEV *)h, 57, a->Register, a->Count, v, 16);
}

static HRESULT APIENTRY umd_set_vertex_shader_const_i(HANDLE h, CONST D3DDDIARG_SETVERTEXSHADERCONSTI *a, CONST INT *v)
{
    return dp2_const((UMD_DEV *)h, 77, a->Register, a->Count, v, 16);
}

static HRESULT APIENTRY umd_set_pixel_shader_const_i(HANDLE h, CONST D3DDDIARG_SETPIXELSHADERCONSTI *a, CONST INT *v)
{
    return dp2_const((UMD_DEV *)h, 93, a->Register, a->Count, v, 16);
}

static HRESULT APIENTRY umd_set_vertex_shader_const_b(HANDLE h, CONST D3DDDIARG_SETVERTEXSHADERCONSTB *a, CONST BOOL *v)
{
    return dp2_const((UMD_DEV *)h, 83, a->Register, a->Count, v, 4);
}

static HRESULT APIENTRY umd_set_pixel_shader_const_b(HANDLE h, CONST D3DDDIARG_SETPIXELSHADERCONSTB *a, CONST BOOL *v)
{
    return dp2_const((UMD_DEV *)h, 94, a->Register, a->Count, v, 4);
}

/* --------------------------------------------------------------- draws
 *
 * Every draw is a self-contained DRAW8 token, as the XP driver's walker
 * makes them (core_dp2.c, walk_draw; d3dpt_proto.h): the vertex range and
 * the indices, inline when they are in the application's memory, a
 * buffer's handle and offset when they are in video memory (the host
 * reads those from VRAM). */

static HRESULT APIENTRY umd_set_stream_source(HANDLE h, CONST D3DDDIARG_SETSTREAMSOURCE *a)
{
    UMD_DEV *d = (UMD_DEV *)h;

    if (a->Stream < D3DPT_DRAW8_MAX_STREAMS) {
        d->st[a->Stream].res = (UMD_RES *)a->hVertexBuffer;
        d->st[a->Stream].um = NULL;
        d->st[a->Stream].off = a->Offset;
        d->st[a->Stream].stride = a->Stride;
        /* a system-memory buffer is the application's memory: read as a user pointer */
        if (d->st[a->Stream].res && !d->st[a->Stream].res->kmt) {
            d->st[a->Stream].um = (const BYTE *)d->st[a->Stream].res->sub[0].sysmem + a->Offset;
            d->st[a->Stream].res = NULL;
            d->st[a->Stream].off = 0;
        }
    }
    return S_OK;
}

static HRESULT APIENTRY umd_set_stream_source_um(HANDLE h, CONST D3DDDIARG_SETSTREAMSOURCEUM *a, CONST VOID *um)
{
    UMD_DEV *d = (UMD_DEV *)h;

    if (a->Stream < D3DPT_DRAW8_MAX_STREAMS) {
        d->st[a->Stream].res = NULL;
        d->st[a->Stream].um = (const BYTE *)um;
        d->st[a->Stream].off = 0;
        d->st[a->Stream].stride = a->Stride;
    }
    return S_OK;
}

static HRESULT APIENTRY umd_set_stream_source_freq(HANDLE h, CONST D3DDDIARG_SETSTREAMSOURCEFREQ *a)
{
    UMD_DEV *d = (UMD_DEV *)h;

    if (a->Stream < D3DPT_DRAW8_MAX_STREAMS) {
        d->st[a->Stream].freq = a->Divider;
    }
    return S_OK;
}

static HRESULT APIENTRY umd_set_indices(HANDLE h, CONST D3DDDIARG_SETINDICES *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *r = (UMD_RES *)a->hIndexBuffer;

    d->ib.res = r && r->kmt ? r : NULL;
    d->ib.um = r && !r->kmt ? (const BYTE *)r->sub[0].sysmem : NULL;
    d->ib.stride = a->Stride;
    return S_OK;
}

static HRESULT APIENTRY umd_set_indices_um(HANDLE h, UINT size, CONST VOID *um)
{
    UMD_DEV *d = (UMD_DEV *)h;

    d->ib.res = NULL;
    d->ib.um = (const BYTE *)um;
    d->ib.stride = size;
    return S_OK;
}

static UINT prim_verts(UINT prim, UINT n)
{
    switch (prim) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return n * 2;
    case D3DPT_LINESTRIP: return n + 1;
    case D3DPT_TRIANGLELIST: return n * 3;
    case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return n + 2;
    default: return 0;
    }
}

/* a stream's vertices from `first` on: {handle, offset} or the bytes */
static BYTE *put_stream(UMD_DEV *d, BYTE *p, const struct UMD_STREAM *s, UINT off, UINT bytes)
{
    if (s->res) {
        cmd_patch(d, p, s->res, D3DPT_PATCH_HANDLE, 0);
        ((UINT *)p)[1] = s->off + off;
        return p + 8;
    }
    CopyMemory(p, s->um + off, bytes);
    return p + ((bytes + 3) & ~3u);
}

static void skip_draw(UMD_DEV *d, const char *why, UINT prim, UINT count)
{
    if (d->skipped++ < 16) {
        umd_log("draw skipped (%s): prim %u count %u fvf 0x%x stride %u", why, prim, count, d->fvf, d->st[0].stride);
    }
}

/* one draw: stream 0 from byte voff of its binding, nverts vertices (the
 * primitives' own for a plain draw); nindices indices from byte ioff of
 * the index buffer, relative to min_index */
static void umd_draw(UMD_DEV *d, UINT prim, UINT count, UINT voff, UINT nverts, UINT ioff, UINT nindices, UINT min_index)
{
    const struct UMD_STREAM *s0 = &d->st[0];
    UINT stride = s0->stride, vbytes, ibytes = 0, bytes, first = 0, ext[D3DPT_DRAW8_MAX_STREAMS], next = 0, inst = 0, nal, i;
    BOOL decl = (d->fvf & 1) != 0;
    d3dpt_dp2_draw8 *t;
    BYTE *p;

    if (!d->fvf) { skip_draw(d, "no declaration", prim, count); return; }
    if (!stride || (!s0->res && !s0->um)) { skip_draw(d, "no stream 0", prim, count); return; }
    if (!prim_verts(prim, count)) { skip_draw(d, "primitive", prim, count); return; }
    if (!nindices) {
        nverts = prim_verts(prim, count);
    }
    if (nverts > 0x10000 || stride > 1024) { skip_draw(d, "too many vertices", prim, count); return; }
    if (nindices && ((!d->ib.res && !d->ib.um) || d->ib.stride != 2)) {
        skip_draw(d, d->ib.stride == 4 ? "32-bit indices" : "no index buffer", prim, count);
        return;
    }
    vbytes = nverts * stride;
    if (nindices && !d->ib.res) {
        ibytes = (nindices * 2 + 3) & ~3u;
    }
    /* DX9 instancing (v16) and the other streams a declaration may read:
     * every bound one, from the same vertex as stream 0 (the host takes
     * the ones the declaration names) */
    if (nindices && decl && (d->st[0].freq & D3DSTREAMSOURCE_INDEXEDDATA) && (d->st[0].freq & 0x3fffffff)) {
        inst = d->st[0].freq & 0x3fffffff;
    }
    if (decl && voff % stride == 0) {
        first = voff / stride;
        for (i = 1; i < D3DPT_DRAW8_MAX_STREAMS; i++) {
            if (d->st[i].res && d->st[i].stride && d->st[i].stride <= 1024) {
                ext[next++] = i;
            }
        }
    }
    bytes = sizeof(*t) + (s0->res ? 8 : (vbytes + 3) & ~3u) + (nindices ? (d->ib.res ? 8 : ibytes) : 0);
    if (next || inst) {
        bytes += 8 + next * (sizeof(d3dpt_dp2_draw8_stream) + 8);
    }
    nal = 2 + next;
    p = dp2_tok(d, D3DPT_DP2_DRAW8, 0, bytes, nal);
    if (!p) {
        skip_draw(d, "no room", prim, count);
        return;
    }
    t = (d3dpt_dp2_draw8 *)p;
    t->prim_type = prim;
    t->prim_count = count;
    t->fvf = d->fvf;
    t->stride = stride;
    t->nverts = nverts;
    t->nindices = nindices;
    t->min_index = min_index;
    t->flags = (s0->res ? D3DPT_DRAW8_VRAM_VB : 0) | (nindices && d->ib.res ? D3DPT_DRAW8_VRAM_IB : 0) |
               (next || inst ? D3DPT_DRAW8_STREAMS : 0);
    p = put_stream(d, (BYTE *)(t + 1), s0, voff, vbytes);
    if (nindices) {
        if (d->ib.res) {
            cmd_patch(d, p, d->ib.res, D3DPT_PATCH_HANDLE, 0);
            ((UINT *)p)[1] = ioff;
            p += 8;
        } else {
            CopyMemory(p, d->ib.um + ioff, nindices * 2);
            p += ibytes;
        }
    }
    if (next || inst) {
        ((UINT *)p)[0] = next;
        ((UINT *)p)[1] = inst ? d->st[0].freq : 0;
        p += 8;
        for (i = 0; i < next; i++) {
            const struct UMD_STREAM *s = &d->st[ext[i]];
            d3dpt_dp2_draw8_stream *sh = (d3dpt_dp2_draw8_stream *)p;
            BOOL per_inst = inst && (s->freq & D3DSTREAMSOURCE_INSTANCEDATA);

            sh->stream = ext[i];
            sh->stride = s->stride;
            sh->flags = D3DPT_DRAW8_VRAM_VB;
            sh->freq = per_inst ? s->freq : 0;
            p = put_stream(d, (BYTE *)(sh + 1), s, per_inst ? 0 : first * s->stride, 0);
        }
    }
    umd_drawn(d);
}

static HRESULT APIENTRY umd_draw_primitive(HANDLE h, CONST D3DDDIARG_DRAWPRIMITIVE *a, CONST UINT *flags)
{
    UMD_DEV *d = (UMD_DEV *)h;

    (void)flags;
    umd_draw(d, a->PrimitiveType, a->PrimitiveCount, a->VStart * d->st[0].stride, 0, 0, 0, 0);
    return S_OK;
}

static HRESULT APIENTRY umd_draw_primitive2(HANDLE h, CONST D3DDDIARG_DRAWPRIMITIVE2 *a)
{
    umd_draw((UMD_DEV *)h, a->PrimitiveType, a->PrimitiveCount, a->FirstVertexOffset, 0, 0, 0, 0);
    return S_OK;
}

static HRESULT APIENTRY umd_draw_indexed_primitive(HANDLE h, CONST D3DDDIARG_DRAWINDEXEDPRIMITIVE *a)
{
    UMD_DEV *d = (UMD_DEV *)h;

    umd_draw(d, a->PrimitiveType, a->PrimitiveCount, (UINT)(a->BaseVertexIndex + (INT)a->MinIndex) * d->st[0].stride,
             a->NumVertices, a->StartIndex * d->ib.stride, prim_verts(a->PrimitiveType, a->PrimitiveCount), a->MinIndex);
    return S_OK;
}

/* the application's own indices (DrawIndexedPrimitiveUP) */
static HRESULT APIENTRY umd_draw_indexed_primitive2(HANDLE h, CONST D3DDDIARG_DRAWINDEXEDPRIMITIVE2 *a, UINT isize,
                                                    CONST VOID *indices, CONST UINT *flags)
{
    UMD_DEV *d = (UMD_DEV *)h;

    (void)flags;
    d->ib.res = NULL;
    d->ib.um = (const BYTE *)indices;
    d->ib.stride = isize;
    umd_draw(d, a->PrimitiveType, a->PrimitiveCount, (UINT)(a->BaseVertexOffset + (INT)(a->MinIndex * d->st[0].stride)),
             a->NumVertices, a->StartIndexOffset, prim_verts(a->PrimitiveType, a->PrimitiveCount), a->MinIndex);
    return S_OK;
}

static HRESULT APIENTRY umd_generate_mip_sub_levels(HANDLE h, CONST D3DDDIARG_GENERATEMIPSUBLEVELS *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *r = (UMD_RES *)a->hResource;
    UINT *e;

    if (!r || !r->kmt) {
        return S_OK;
    }
    e = (UINT *)dp2_tok(d, 89, 1, 8, 1);
    if (e) {
        cmd_patch(d, &e[0], r, D3DPT_PATCH_HANDLE, 0);
        e[1] = (UINT)a->Filter;
    }
    return S_OK;
}

/* ---------------------------------------------------------------- blits
 *
 * The runtime's copies into video memory (a managed texture's update from
 * its system-memory copy, a buffer's): done here on the CPU through a
 * lock of both, as the XP driver does them, and the host told to re-read
 * the texture. */

static BYTE *lock_res(UMD_DEV *d, UMD_RES *r, BOOL ro)
{
    D3DDDICB_LOCK lk;

    if (!r->kmt) {
        return (BYTE *)r->sub[0].sysmem;
    }
    ZeroMemory(&lk, sizeof(lk));
    lk.hAllocation = r->kmt;
    lk.Flags.ReadOnly = ro;
    lk.Flags.WriteOnly = !ro;
    return SUCCEEDED(d->cb.pfnLockCb(d->rt, &lk)) ? (BYTE *)lk.pData : NULL;
}

static void unlock_res(UMD_DEV *d, UMD_RES *r)
{
    D3DDDICB_UNLOCK ul;

    if (r->kmt) {
        ZeroMemory(&ul, sizeof(ul));
        ul.NumAllocations = 1;
        ul.phAllocations = &r->kmt;
        d->cb.pfnUnlockCb(d->rt, &ul);
    }
}

/* a subresource's memory and pitch, through the lock's base */
static BYTE *sub_mem(UMD_RES *r, BYTE *base, UINT i)
{
    return r->sub[i].sysmem ? (BYTE *)r->sub[i].sysmem : base + r->sub[i].off;
}

static void vram_dirty(UMD_DEV *d, UMD_RES *r)
{
    d3dpt_handle *dh;

    if (!r->kmt || !r->d.caps) {
        return;
    }
    dh = (d3dpt_handle *)cmd_rec(d, D3DPT_OP_VRAM_DIRTY, sizeof(*dh), 0, 1);
    if (dh) {
        cmd_patch(d, &dh->handle, r, D3DPT_PATCH_HANDLE, 0);
    }
}

/* TEXBLT: SrcRect of the source's level 0 (and the same part of every
 * level both have) to DstPoint, one face of a cube; DXT in whole blocks */
static HRESULT APIENTRY umd_tex_blt(HANDLE h, CONST D3DDDIARG_TEXBLT *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *dst = (UMD_RES *)a->hDstResource, *src = (UMD_RES *)a->hSrcResource;
    UINT slev = src->cr.MipLevels ? src->cr.MipLevels : 1, dlev = dst->cr.MipLevels ? dst->cr.MipLevels : 1;
    UINT fmt = (UINT)dst->cr.Format, lv, levels, sface = 0, dface = 0;
    BOOL dxt = umd_is_dxt(fmt);
    BYTE *sb, *db;

    if (dst->cr.Flags.CubeMap) {
        dface = a->CubeMapFace * dlev;
        sface = src->cr.Flags.CubeMap ? a->CubeMapFace * slev : 0;
    }
    /* the source's levels from the one that matches the destination's level 0 */
    levels = slev < dlev ? slev : dlev;
    cmd_flush(d);
    sb = lock_res(d, src, TRUE);
    db = lock_res(d, dst, FALSE);
    if (sb && db) {
        for (lv = 0; lv < levels; lv++) {
            UINT si = sface + lv, di = dface + lv;
            UINT x0 = (UINT)a->SrcRect.left >> lv, y0 = (UINT)a->SrcRect.top >> lv;
            UINT x1 = (UINT)a->DstPoint.x >> lv, y1 = (UINT)a->DstPoint.y >> lv;
            UINT cw = (((UINT)a->SrcRect.right + (1u << lv) - 1) >> lv) - x0;
            UINT ch = (((UINT)a->SrcRect.bottom + (1u << lv) - 1) >> lv) - y0;
            UINT rows, rowbytes, y, bpp = umd_row_bytes(fmt, 1);
            BYTE *s, *t;

            if (si >= src->nsub || di >= dst->nsub) {
                break;
            }
            if (!cw) cw = 1;
            if (!ch) ch = 1;
            if (dxt) {
                UINT block = umd_row_bytes(fmt, 4);

                rows = (y0 + ch + 3) / 4 - y0 / 4;
                rowbytes = ((x0 + cw + 3) / 4 - x0 / 4) * block;
                s = sub_mem(src, sb, si) + (y0 / 4) * src->sub[si].pitch + (x0 / 4) * block;
                t = sub_mem(dst, db, di) + (y1 / 4) * dst->sub[di].pitch + (x1 / 4) * block;
            } else {
                rows = ch;
                rowbytes = cw * bpp;
                s = sub_mem(src, sb, si) + y0 * src->sub[si].pitch + x0 * bpp;
                t = sub_mem(dst, db, di) + y1 * dst->sub[di].pitch + x1 * bpp;
            }
            if (rowbytes > dst->sub[di].pitch || rowbytes > src->sub[si].pitch) {
                continue;
            }
            for (y = 0; y < rows; y++) {
                CopyMemory(t + y * dst->sub[di].pitch, s + y * src->sub[si].pitch, rowbytes);
            }
        }
    }
    if (db) unlock_res(d, dst);
    if (sb) unlock_res(d, src);
    vram_dirty(d, dst);
    return S_OK;
}

static HRESULT APIENTRY umd_buf_blt(HANDLE h, CONST D3DDDIARG_BUFFERBLT *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *dst = (UMD_RES *)a->hDstResource, *src = (UMD_RES *)a->hSrcResource;
    BYTE *sb, *db;

    cmd_flush(d);
    sb = lock_res(d, src, TRUE);
    db = lock_res(d, dst, FALSE);
    if (sb && db && a->Offset + a->SrcRange.Size <= dst->d.size) {
        CopyMemory(db + a->Offset, sb + a->SrcRange.Offset, a->SrcRange.Size);
    }
    if (db) unlock_res(d, dst);
    if (sb) unlock_res(d, src);
    return S_OK;
}

/* a subresource's size: its level of the resource's level-0 size */
static void sub_size(const UMD_RES *r, UINT i, UINT *w, UINT *h)
{
    UINT levels = r->cr.MipLevels ? r->cr.MipLevels : 1, lv = i % levels;

    *w = r->d.w >> lv ? r->d.w >> lv : 1;
    *h = r->d.h >> lv ? r->d.h >> lv : 1;
}

static BOOL rect_ok(const RECT *r, UINT w, UINT h)
{
    return r->left >= 0 && r->top >= 0 && r->left < r->right && r->top < r->bottom &&
           (UINT)r->right <= w && (UINT)r->bottom <= h;
}

/* ColorFill: the rectangle in the colour packed for the surface's format,
 * on the CPU after what the host drew came back (core_dp2.c,
 * walk_colorfill) */
static HRESULT APIENTRY umd_color_fill(HANDLE h, CONST D3DDDIARG_COLORFILL *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *r = (UMD_RES *)a->hResource;
    UINT w, ht, bpp, x, y;
    ULONG v[4];
    BYTE *base, *mem;

    sub_size(r, a->SubResourceIndex, &w, &ht);
    bpp = umd_fill_pack((ULONG)r->cr.Format, a->Color, (UCHAR *)v);
    if (a->SubResourceIndex >= r->nsub || !rect_ok(&a->DstRect, w, ht) || !bpp) {
        umd_log("ColorFill refused: fmt %u sub %u", r->cr.Format, a->SubResourceIndex);
        return S_OK;
    }
    umd_readback(d, r);
    cmd_flush(d);
    base = lock_res(d, r, FALSE);
    if (!base) {
        return E_FAIL;
    }
    mem = sub_mem(r, base, a->SubResourceIndex);
    for (y = (UINT)a->DstRect.top; y < (UINT)a->DstRect.bottom; y++) {
        BYTE *row = mem + y * r->sub[a->SubResourceIndex].pitch + a->DstRect.left * bpp;

        for (x = 0; x < (UINT)(a->DstRect.right - a->DstRect.left); x++) {
            if (bpp == 4) ((ULONG *)row)[x] = v[0];
            else if (bpp == 2) ((USHORT *)row)[x] = (USHORT)v[0];
            else if (bpp == 1) row[x] = (BYTE)v[0];
            else CopyMemory(row + x * bpp, v, bpp);
        }
    }
    unlock_res(d, r);
    vram_dirty(d, r);
    return S_OK;
}

/* Blt (StretchRect, GetRenderTargetData, UpdateSurface): on the CPU, as
 * the XP driver's walk_blt9 does it. Same-size rectangles copy (DXT in
 * whole blocks), scaled ones take the nearest texel; one texel size
 * copies as it is, two formats of the ARGB group convert through a
 * D3DCOLOR. */
static HRESULT APIENTRY umd_blt(HANDLE h, CONST D3DDDIARG_BLT *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *src = (UMD_RES *)a->hSrcResource, *dst = (UMD_RES *)a->hDstResource;
    const RECT *sr = &a->SrcRect, *dr = &a->DstRect;
    ULONG sfmt = (ULONG)src->cr.Format, dfmt = (ULONG)dst->cr.Format, c;
    UINT sw, sh, dw, dh, spitch, dpitch, bpp, dbpp, cw, ch, x, y;
    BYTE *sb, *db, *smem, *dmem;
    UCHAR zero[16];

    if (a->SrcSubResourceIndex >= src->nsub || a->DstSubResourceIndex >= dst->nsub) {
        return E_INVALIDARG;
    }
    sub_size(src, a->SrcSubResourceIndex, &sw, &sh);
    sub_size(dst, a->DstSubResourceIndex, &dw, &dh);
    if (umd_row_bytes(sfmt, 1) != umd_row_bytes(dfmt, 1) || (sfmt != dfmt && (umd_is_dxt(sfmt) || umd_is_dxt(dfmt)))) {
        ZeroMemory(zero, sizeof(zero));
        if (!umd_px_unpack(sfmt, zero, &c) || !umd_fill_pack(dfmt, 0, zero)) {
            sfmt = 0;
        }
    } else if (sfmt != dfmt) {
        dfmt = sfmt;                                /* one texel size: copied as it is */
    }
    if (!sfmt || !rect_ok(sr, sw, sh) || !rect_ok(dr, dw, dh) || !umd_row_bytes(sfmt, 1)) {
        umd_log("Blt refused: fmt %u -> %u, %ux%u -> %ux%u", src->cr.Format, dst->cr.Format, sw, sh, dw, dh);
        return S_OK;
    }
    umd_readback(d, src);
    if (dst != src) {
        umd_readback(d, dst);                       /* the rest of the destination stays what the host drew */
    }
    cmd_flush(d);
    sb = lock_res(d, src, src != dst);
    db = dst == src ? sb : lock_res(d, dst, FALSE);
    if (!sb || !db) {
        if (sb) unlock_res(d, src);
        return E_FAIL;
    }
    smem = sub_mem(src, sb, a->SrcSubResourceIndex);
    dmem = sub_mem(dst, db, a->DstSubResourceIndex);
    spitch = src->sub[a->SrcSubResourceIndex].pitch;
    dpitch = dst->sub[a->DstSubResourceIndex].pitch;
    cw = (UINT)(dr->right - dr->left);
    ch = (UINT)(dr->bottom - dr->top);
    if (umd_is_dxt(sfmt)) {
        UINT block = umd_row_bytes(sfmt, 4), rows = (sr->top + ch + 3) / 4 - sr->top / 4;
        UINT rowbytes = ((sr->left + cw + 3) / 4 - sr->left / 4) * block;

        if (cw == (UINT)(sr->right - sr->left) && ch == (UINT)(sr->bottom - sr->top)) {
            for (y = 0; y < rows; y++) {
                CopyMemory(dmem + (dr->top / 4 + y) * dpitch + (dr->left / 4) * block,
                           smem + (sr->top / 4 + y) * spitch + (sr->left / 4) * block, rowbytes);
            }
        }
    } else if (cw == (UINT)(sr->right - sr->left) && ch == (UINT)(sr->bottom - sr->top) && sfmt == dfmt) {
        bpp = umd_row_bytes(sfmt, 1);
        for (y = 0; y < ch; y++) {
            MoveMemory(dmem + (dr->top + y) * dpitch + dr->left * bpp, smem + (sr->top + y) * spitch + sr->left * bpp, cw * bpp);
        }
    } else {
        UINT scw = (UINT)(sr->right - sr->left), sch = (UINT)(sr->bottom - sr->top);

        bpp = umd_row_bytes(sfmt, 1);
        dbpp = umd_row_bytes(dfmt, 1);
        for (y = 0; y < ch; y++) {
            const BYTE *srow = smem + (sr->top + ((2 * y + 1) * sch) / (2 * ch)) * spitch;
            BYTE *drow = dmem + (dr->top + y) * dpitch + dr->left * dbpp;

            for (x = 0; x < cw; x++) {
                umd_px_copy(sfmt, dfmt, drow + x * dbpp, srow + (sr->left + ((2 * x + 1) * scw) / (2 * cw)) * bpp, bpp);
            }
        }
    }
    if (db != sb) unlock_res(d, dst);
    unlock_res(d, src);
    vram_dirty(d, dst);
    return S_OK;
}

/* -------------------------------------------------------------- present */

static HRESULT APIENTRY umd_present(HANDLE h, CONST D3DDDIARG_PRESENT *p)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *src = (UMD_RES *)p->hSrcResource, *dst = (UMD_RES *)p->hDstResource;
    D3DDDICB_PRESENT pc;
    HRESULT hr;
    static ULONG logged;

    if (src) {
        umd_readback(d, src);
    }
    cmd_flush(d);
    ZeroMemory(&pc, sizeof(pc));
    pc.hSrcAllocation = src ? src->kmt : 0;
    pc.hDstAllocation = dst ? dst->kmt : 0;
    pc.hContext = d->ctx;
    hr = d->cb.pfnPresentCb(d->rt, &pc);
    if (logged < 16) {
        logged++;
        umd_log("Present flags 0x%x src %p sub %u dst %p -> 0x%08x", p->Flags.Value, p->hSrcResource,
                p->SrcSubResourceIndex, p->hDstResource, hr);
    }
    return hr;
}

/* A full-screen swap chain's primary becomes what the display scans out
 * (dxgkrnl then flips through SetVidPnSourceAddress / Present). */
static HRESULT APIENTRY umd_set_display_mode(HANDLE h, CONST D3DDDIARG_SETDISPLAYMODE *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_RES *r = (UMD_RES *)a->hResource;
    D3DDDICB_SETDISPLAYMODE m;
    HRESULT hr;

    umd_readback(d, r);
    cmd_flush(d);
    ZeroMemory(&m, sizeof(m));
    m.hPrimaryAllocation = r->kmt;
    hr = d->cb.pfnSetDisplayModeCb(d->rt, &m);
    umd_log("SetDisplayMode %ux%u fmt %u -> 0x%08x", r->d.w, r->d.h, r->d.format, hr);
    return hr;
}

static HRESULT APIENTRY umd_flush(HANDLE h)
{
    return cmd_flush((UMD_DEV *)h);
}

/* ---------------------------------------------- what is not done yet:
 * each one names itself in the log (the first few times) and succeeds,
 * so a run shows what the runtime asks for next. Each is declared with
 * the kit's own function type, which checks its arguments: a stdcall
 * function with the wrong argument count corrupts the caller's stack. */
#define UMD_TODO(fn) do { static ULONG n_; if (n_ < 4) { n_++; umd_log("todo: %s", #fn); } } while (0)
#define STUB1(type, fn, t1) \
    static HRESULT APIENTRY fn(HANDLE h, t1 a1) { (void)h; (void)a1; UMD_TODO(fn); return S_OK; } \
    static const type fn##_type_ = fn;
#define STUB2(type, fn, t1, t2) \
    static HRESULT APIENTRY fn(HANDLE h, t1 a1, t2 a2) { (void)h; (void)a1; (void)a2; UMD_TODO(fn); return S_OK; } \
    static const type fn##_type_ = fn;
#define STUB3(type, fn, t1, t2, t3) \
    static HRESULT APIENTRY fn(HANDLE h, t1 a1, t2 a2, t3 a3) { (void)h; (void)a1; (void)a2; (void)a3; UMD_TODO(fn); return S_OK; } \
    static const type fn##_type_ = fn;
#define STUB4(type, fn, t1, t2, t3, t4) \
    static HRESULT APIENTRY fn(HANDLE h, t1 a1, t2 a2, t3 a3, t4 a4) { (void)h; (void)a1; (void)a2; (void)a3; (void)a4; UMD_TODO(fn); return S_OK; } \
    static const type fn##_type_ = fn;

STUB3(PFND3DDDI_DRAWRECTPATCH, umd_draw_rect_patch, CONST D3DDDIARG_DRAWRECTPATCH *, CONST D3DDDIRECTPATCH_INFO *, CONST FLOAT *)
STUB3(PFND3DDDI_DRAWTRIPATCH, umd_draw_tri_patch, CONST D3DDDIARG_DRAWTRIPATCH *, CONST D3DDDITRIPATCH_INFO *, CONST FLOAT *)
STUB1(PFND3DDDI_VOLBLT, umd_vol_blt, CONST D3DDDIARG_VOLUMEBLT *)
STUB1(PFND3DDDI_STATESET, umd_state_set, D3DDDIARG_STATESET *)
STUB1(PFND3DDDI_SETPRIORITY, umd_set_priority, CONST D3DDDIARG_SETPRIORITY *)
STUB2(PFND3DDDI_UPDATEPALETTE, umd_update_palette, CONST D3DDDIARG_UPDATEPALETTE *, CONST PALETTEENTRY *)
STUB1(PFND3DDDI_SETPALETTE, umd_set_palette, CONST D3DDDIARG_SETPALETTE *)
STUB1(PFND3DDDI_SETCONVOLUTIONKERNELMONO, umd_set_convolution_kernel_mono, CONST D3DDDIARG_SETCONVOLUTIONKERNELMONO *)
STUB1(PFND3DDDI_COMPOSERECTS, umd_compose_rects, CONST D3DDDIARG_COMPOSERECTS *)
STUB1(PFND3DDDI_DEPTHFILL, umd_depth_fill, CONST D3DDDIARG_DEPTHFILL *)
STUB1(PFND3DDDI_QUERYRESOURCERESIDENCY, umd_query_resource_residency, CONST D3DDDIARG_QUERYRESOURCERESIDENCY *)
STUB1(PFND3DDDI_RENAME, umd_rename, CONST D3DDDIARG_RENAME *)

/* the ones that hand back a handle or data: a unique dummy until each is real */
static HRESULT APIENTRY umd_validate_device(HANDLE h, D3DDDIARG_VALIDATETEXTURESTAGESTATE *v)
{
    (void)h;
    v->NumPasses = 1;
    return S_OK;
}

static HRESULT APIENTRY umd_get_info(HANDLE h, UINT id, VOID *p, UINT size)
{
    (void)h;
    umd_log("GetInfo id %u size %u", id, size);
    if (p && size) {
        ZeroMemory(p, size);
    }
    return E_FAIL;
}

/* --------------------------------------------------------------- queries
 *
 * The event and occlusion queries, as the XP driver's DX9 face has them
 * (core_dp2.c): an event is done once the commands before it ran, an
 * occlusion query is the host's. Its count comes back through the
 * kernel-mode driver: a D3DPT_UMD_OP_RETURN before the QUERY_GET_DATA
 * record has the result copied into this device's result allocation, a
 * page of VRAM the host never sees, which a lock then reads. */

typedef struct UMD_QUERY {
    D3DDDIQUERYTYPE type;
    ULONG host;                                   /* the host's handle (occlusion) */
    BOOL ended;                                   /* issued with End since the last answer */
    DWORD value;                                  /* the last answer */
} UMD_QUERY;

#define UMD_RESULT_BYTES 4096

/* the result allocation, made at the first occlusion query */
static UMD_RES *umd_result_res(UMD_DEV *d)
{
    D3DDDICB_ALLOCATE al;
    D3DDDI_ALLOCATIONINFO ai;
    UMD_RES *r;

    if (d->qres) {
        return d->qres;
    }
    r = (UMD_RES *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r));
    if (!r) {
        return NULL;
    }
    r->sub = (struct UMD_SUB *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r->sub));
    if (!r->sub) {
        HeapFree(GetProcessHeap(), 0, r);
        return NULL;
    }
    r->nsub = 1;
    r->d.magic = D3DPT_ALLOC_MAGIC;
    r->d.kind = D3DPT_ALLOC_D3D;
    r->d.size = UMD_RESULT_BYTES;
    r->d.w = UMD_RESULT_BYTES;
    r->d.h = 1;
    r->d.pitch = UMD_RESULT_BYTES;
    r->d.caps = 0;                                /* no host handle: the kernel-mode driver writes it */
    ZeroMemory(&al, sizeof(al));
    ZeroMemory(&ai, sizeof(ai));
    ai.pPrivateDriverData = &r->d;
    ai.PrivateDriverDataSize = sizeof(r->d);
    al.NumAllocations = 1;
    al.pAllocationInfo = &ai;
    if (FAILED(d->cb.pfnAllocateCb(d->rt, &al))) {
        HeapFree(GetProcessHeap(), 0, r->sub);
        HeapFree(GetProcessHeap(), 0, r);
        return NULL;
    }
    r->kmt = ai.hAllocation;
    d->qres = r;
    return r;
}

static HRESULT APIENTRY umd_create_query(HANDLE h, D3DDDIARG_CREATEQUERY *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_QUERY *q;

    if (a->QueryType != D3DDDIQUERYTYPE_EVENT && a->QueryType != D3DDDIQUERYTYPE_OCCLUSION) {
        umd_log("CreateQuery: type %u refused", a->QueryType);
        return D3DDDIERR_NOTAVAILABLE;
    }
    q = (UMD_QUERY *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*q));
    if (!q) {
        return E_OUTOFMEMORY;
    }
    q->type = a->QueryType;
    if (q->type == D3DDDIQUERYTYPE_OCCLUSION) {
        d3dpt_create_query *c;

        /* the host's handle space is the whole guest's: this process's id in it */
        q->host = 0x51000000u | ((GetCurrentProcessId() & 0xfffu) << 12) | (++d->next_query & 0xfffu);
        c = (d3dpt_create_query *)cmd_rec(d, D3DPT_OP_CREATE_QUERY, sizeof(*c), 0, 0);
        if (c) {
            c->handle = q->host;
            c->type = 9;
        }
    }
    a->hQuery = q;
    return S_OK;
}

static HRESULT APIENTRY umd_destroy_query(HANDLE h, CONST HANDLE hq)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_QUERY *q = (UMD_QUERY *)hq;

    if (q->host) {
        d3dpt_handle *k = (d3dpt_handle *)cmd_rec(d, D3DPT_OP_RELEASE, sizeof(*k), 0, 0);

        if (k) {
            k->handle = q->host;
        }
    }
    HeapFree(GetProcessHeap(), 0, q);
    return S_OK;
}

/* D3DISSUE_END 1, D3DISSUE_BEGIN 2, as the host takes them */
static HRESULT APIENTRY umd_issue_query(HANDLE h, CONST D3DDDIARG_ISSUEQUERY *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_QUERY *q = (UMD_QUERY *)a->hQuery;

    if (a->Flags.End) {
        q->ended = TRUE;
    }
    if (q->host) {
        d3dpt_u32x2 *k = (d3dpt_u32x2 *)cmd_rec(d, D3DPT_OP_QUERY_ISSUE, sizeof(*k), 0, 0);

        if (k) {
            k->a = q->host;
            k->b = (a->Flags.End ? 1u : 0u) | (a->Flags.Begin ? 2u : 0u);
        }
    }
    return S_OK;
}

static HRESULT APIENTRY umd_get_query_data(HANDLE h, CONST D3DDDIARG_GETQUERYDATA *a)
{
    UMD_DEV *d = (UMD_DEV *)h;
    UMD_QUERY *q = (UMD_QUERY *)a->hQuery;
    UMD_RES *res;
    UINT n;

    if (q->type == D3DDDIQUERYTYPE_EVENT) {
        cmd_flush(d);                             /* every command before it on its way */
        if (a->pData) {
            *(BOOL *)a->pData = TRUE;
        }
        return S_OK;
    }
    if (!q->ended) {
        if (a->pData) {
            *(DWORD *)a->pData = q->value;
        }
        return S_OK;
    }
    res = umd_result_res(d);
    if (!res) {
        return E_OUTOFMEMORY;
    }
    /* the host has the count once the draws before it ran: it says "not
     * yet" (S_FALSE) only while it waits for its own GPU */
    for (n = 0; n < 1000; n++) {
        d3dpt_u32x2 *ret = (d3dpt_u32x2 *)cmd_rec(d, D3DPT_UMD_OP_RETURN, sizeof(*ret), 0, 1);
        d3dpt_query_get *g;
        const d3dpt_ret *r;
        BYTE *mem;
        HRESULT hr = E_FAIL;
        DWORD v = 0;

        if (!ret) {
            return E_FAIL;
        }
        cmd_patch(d, &ret->a, res, D3DPT_PATCH_OFFSET, 0);
        ret->b = 4;
        g = (d3dpt_query_get *)cmd_rec(d, D3DPT_OP_QUERY_GET_DATA, sizeof(*g), 0, 0);
        if (!g) {
            return E_FAIL;
        }
        g->handle = q->host;
        g->flags = 1;                             /* D3DGETDATA_FLUSH */
        g->size = 4;
        cmd_flush(d);
        mem = lock_res(d, res, TRUE);
        if (mem) {
            r = (const d3dpt_ret *)mem;
            hr = (HRESULT)r->hr;
            v = *(const DWORD *)(r + 1);
            unlock_res(d, res);
        }
        if (hr == S_OK) {
            q->value = v;
            q->ended = FALSE;
            if (a->pData) {
                *(DWORD *)a->pData = v;
            }
            return S_OK;
        }
        if (hr != S_FALSE) {
            umd_log("occlusion query 0x%08x: the host answered 0x%08x", q->host, hr);
            break;
        }
    }
    q->ended = FALSE;
    if (a->pData) {
        *(DWORD *)a->pData = 0;
    }
    return S_OK;
}

/* Another process's allocation, by the private data its creator gave it
 * (the kernel-mode driver's for dxgkrnl's own surfaces: the desktop's
 * shared primary, which d3d9.dll opens at CreateDevice as the target of
 * its windowed presents). One subresource, as it was described. */
static HRESULT APIENTRY umd_open_resource(HANDLE h, D3DDDIARG_OPENRESOURCE *o)
{
    const D3DDDI_OPENALLOCATIONINFO *ai = o->pOpenAllocationInfo;
    const D3DPT_ALLOC_DESC *d;
    UMD_RES *r;

    (void)h;
    if (o->NumAllocations != 1 || !ai[0].pPrivateDriverData || ai[0].PrivateDriverDataSize < sizeof(*d) ||
        ((const D3DPT_ALLOC_DESC *)ai[0].pPrivateDriverData)->magic != D3DPT_ALLOC_MAGIC) {
        umd_log("OpenResource: %u allocations, private data %u bytes: not ours", o->NumAllocations,
                o->NumAllocations ? ai[0].PrivateDriverDataSize : 0);
        return E_INVALIDARG;
    }
    d = (const D3DPT_ALLOC_DESC *)ai[0].pPrivateDriverData;
    r = (UMD_RES *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r));
    if (!r) {
        return E_OUTOFMEMORY;
    }
    r->sub = (struct UMD_SUB *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*r->sub));
    if (!r->sub) {
        HeapFree(GetProcessHeap(), 0, r);
        return E_OUTOFMEMORY;
    }
    r->rt = o->hResource;
    r->kmt = ai[0].hAllocation;
    r->d = *d;
    if (d->kind == D3DPT_ALLOC_PRIMARY) {
        r->d.caps = D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY;     /* as the kernel-mode driver registers it */
    }
    r->nsub = 1;
    r->sub[0].pitch = d->pitch;
    r->sub[0].slice = d->pitch * d->h;
    r->cr.Format = (D3DDDIFORMAT)d->format;
    r->cr.Pool = D3DDDIPOOL_VIDEOMEMORY;
    r->cr.MipLevels = 1;
    r->cr.SurfCount = 1;
    umd_log("OpenResource: kind %u %ux%u fmt %u caps 0x%x", d->kind, d->w, d->h, d->format, d->caps);
    o->hResource = r;
    return S_OK;
}

/* ---------------------------------------------------------------- device */

static HRESULT APIENTRY umd_destroy_device(HANDLE h)
{
    UMD_DEV *d = (UMD_DEV *)h;

    umd_log("DestroyDevice");
    cmd_flush(d);
    if (d->ctx_live) {
        d3dpt_handle *k = (d3dpt_handle *)cmd_rec(d, D3DPT_OP_CTX_DESTROY, sizeof(*k), 0, 0);

        if (k) {
            k->handle = d->host_ctx;
        }
        cmd_flush(d);
    }
    if (d->qres) {
        umd_destroy_resource(d, d->qres);
        d->qres = NULL;
    }
    if (d->pre) {
        HeapFree(GetProcessHeap(), 0, d->pre);
    }
    if (d->ctx) {
        D3DDDICB_DESTROYCONTEXT dc;

        dc.hContext = d->ctx;
        d->cb.pfnDestroyContextCb(d->rt, &dc);
    }
    HeapFree(GetProcessHeap(), 0, d);
    return S_OK;
}

static HRESULT APIENTRY umd_create_device(HANDLE h, D3DDDIARG_CREATEDEVICE *c)
{
    static LONG devices;
    UMD_ADAPTER *a = (UMD_ADAPTER *)h;
    D3DDDI_DEVICEFUNCS *f = c->pDeviceFuncs;
    D3DDDICB_CREATECONTEXT cc;
    UMD_DEV *d;
    HRESULT hr;

    d = (UMD_DEV *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*d));
    if (!d) {
        return E_OUTOFMEMORY;
    }
    d->rt = c->hDevice;
    d->ad = a;
    d->cb = *c->pCallbacks;
    d->host_ctx = (GetCurrentProcessId() << 8) | ((ULONG)InterlockedIncrement(&devices) & 0xff);

    ZeroMemory(&cc, sizeof(cc));
    cc.NodeOrdinal = 0;
    cc.EngineAffinity = 0;
    hr = d->cb.pfnCreateContextCb(d->rt, &cc);
    umd_log("CreateDevice interface 0x%x version 0x%x flags 0x%x: context -> 0x%08x, buffer %u bytes, lists %u / %u",
            c->Interface, c->Version, c->Flags.Value, hr, cc.CommandBufferSize, cc.AllocationListSize,
            cc.PatchLocationListSize);
    if (FAILED(hr)) {
        HeapFree(GetProcessHeap(), 0, d);
        return hr;
    }
    d->ctx = cc.hContext;
    d->buf = (BYTE *)cc.pCommandBuffer;
    d->buf_size = cc.CommandBufferSize;
    d->al = cc.pAllocationList;
    d->al_size = cc.AllocationListSize;
    d->pl = cc.pPatchLocationList;
    d->pl_size = cc.PatchLocationListSize;
    cmd_reset(d);

    f->pfnSetRenderState = umd_set_render_state;
    f->pfnUpdateWInfo = umd_update_winfo;
    f->pfnValidateDevice = umd_validate_device;
    f->pfnSetTextureStageState = umd_set_texture_stage_state;
    f->pfnSetTexture = umd_set_texture;
    f->pfnSetPixelShader = umd_set_pixel_shader;
    f->pfnSetPixelShaderConst = umd_set_pixel_shader_const;
    f->pfnSetStreamSourceUm = umd_set_stream_source_um;
    f->pfnSetIndices = umd_set_indices;
    f->pfnSetIndicesUm = umd_set_indices_um;
    f->pfnDrawPrimitive = umd_draw_primitive;
    f->pfnDrawIndexedPrimitive = umd_draw_indexed_primitive;
    f->pfnDrawRectPatch = umd_draw_rect_patch;
    f->pfnDrawTriPatch = umd_draw_tri_patch;
    f->pfnDrawPrimitive2 = umd_draw_primitive2;
    f->pfnDrawIndexedPrimitive2 = umd_draw_indexed_primitive2;
    f->pfnVolBlt = umd_vol_blt;
    f->pfnBufBlt = umd_buf_blt;
    f->pfnTexBlt = umd_tex_blt;
    f->pfnStateSet = umd_state_set;
    f->pfnSetPriority = umd_set_priority;
    f->pfnClear = umd_clear;
    f->pfnUpdatePalette = umd_update_palette;
    f->pfnSetPalette = umd_set_palette;
    f->pfnSetVertexShaderConst = umd_set_vertex_shader_const;
    f->pfnMultiplyTransform = umd_multiply_transform;
    f->pfnSetTransform = umd_set_transform;
    f->pfnSetViewport = umd_set_viewport;
    f->pfnSetZRange = umd_set_zrange;
    f->pfnSetMaterial = umd_set_material;
    f->pfnSetLight = umd_set_light;
    f->pfnCreateLight = umd_create_light;
    f->pfnDestroyLight = umd_destroy_light;
    f->pfnSetClipPlane = umd_set_clip_plane;
    f->pfnGetInfo = umd_get_info;
    f->pfnLock = umd_lock;
    f->pfnUnlock = umd_unlock;
    f->pfnCreateResource = umd_create_resource;
    f->pfnDestroyResource = umd_destroy_resource;
    f->pfnSetDisplayMode = umd_set_display_mode;
    f->pfnPresent = umd_present;
    f->pfnFlush = umd_flush;
    f->pfnCreateVertexShaderFunc = umd_create_vertex_shader_func;
    f->pfnDeleteVertexShaderFunc = umd_delete_vertex_shader_func;
    f->pfnSetVertexShaderFunc = umd_set_vertex_shader_func;
    f->pfnCreateVertexShaderDecl = umd_create_vertex_shader_decl;
    f->pfnDeleteVertexShaderDecl = umd_delete_vertex_shader_decl;
    f->pfnSetVertexShaderDecl = umd_set_vertex_shader_decl;
    f->pfnSetVertexShaderConstI = umd_set_vertex_shader_const_i;
    f->pfnSetVertexShaderConstB = umd_set_vertex_shader_const_b;
    f->pfnSetScissorRect = umd_set_scissor_rect;
    f->pfnSetStreamSource = umd_set_stream_source;
    f->pfnSetStreamSourceFreq = umd_set_stream_source_freq;
    f->pfnSetConvolutionKernelMono = umd_set_convolution_kernel_mono;
    f->pfnComposeRects = umd_compose_rects;
    f->pfnBlt = umd_blt;
    f->pfnColorFill = umd_color_fill;
    f->pfnDepthFill = umd_depth_fill;
    f->pfnCreateQuery = umd_create_query;
    f->pfnDestroyQuery = umd_destroy_query;
    f->pfnIssueQuery = umd_issue_query;
    f->pfnGetQueryData = umd_get_query_data;
    f->pfnSetRenderTarget = umd_set_render_target;
    f->pfnSetDepthStencil = umd_set_depth_stencil;
    f->pfnGenerateMipSubLevels = umd_generate_mip_sub_levels;
    f->pfnSetPixelShaderConstI = umd_set_pixel_shader_const_i;
    f->pfnSetPixelShaderConstB = umd_set_pixel_shader_const_b;
    f->pfnCreatePixelShader = umd_create_pixel_shader;
    f->pfnDeletePixelShader = umd_delete_pixel_shader;
    f->pfnDestroyDevice = umd_destroy_device;
    f->pfnQueryResourceResidency = umd_query_resource_residency;
    f->pfnOpenResource = umd_open_resource;
    f->pfnRename = umd_rename;
    c->hDevice = d;
    return S_OK;
}
