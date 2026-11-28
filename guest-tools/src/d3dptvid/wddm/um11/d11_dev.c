/*
 * d11_dev.c: the Direct3D 11 driver's device (track M25): its creation,
 * the entries no other file has, and DXGI's functions (present, display
 * mode, a swap chain's identities and its copies).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3dpt11.h"

SIZE_T APIENTRY d11_calc_device_size(D3D10DDI_HADAPTER h, const D3D10DDIARG_CALCPRIVATEDEVICESIZE *a)
{ D11_TRACE("d11_calc_device_size");
    (void)h; (void)a;
    return sizeof(D11_DEV);
}

static void APIENTRY d11_flush(D3D10DDI_HDEVICE h)
{ D11_TRACE("d11_flush");
    cmd_flush(DEV(h));
}

static void APIENTRY d11_destroy_device(D3D10DDI_HDEVICE h)
{ D11_TRACE("d11_destroy_device");
    D11_DEV *d = DEV(h);

    if (g_d11_verbose) {
        umd_log("d3d11: DestroyDevice");
    }
    cmd_flush(d);
    d11_pipe_destroy(d);
    if (d->ctx) {
        D3DDDICB_DESTROYCONTEXT dc;

        /* the kernel driver destroys the host's context with it */
        dc.hContext = d->ctx;
        d->kt->pfnDestroyContextCb(d->rt.handle, &dc);
        d->ctx = NULL;
    }
}

static void APIENTRY d11_check_format(D3D10DDI_HDEVICE h, DXGI_FORMAT f, UINT *out)
{ D11_TRACE_FEW("d11_check_format");
    (void)h;
    *out = fmt_support((UINT)f);
    if (g_d11_verbose) {
        umd_log("d3d11: CheckFormatSupport %u -> 0x%x", f, *out);
    }
}

/* a format's family, as the member multisampling is decided by: the
 * runtime refuses every device unless a typeless format and each of its
 * typed ones give the same quality levels ("MSAA quality for parent !=
 * child" in its ETW journal, DRIVER_INTERNAL_ERROR) */
static UINT ms_family(UINT f)
{
    if (f >= 1 && f <= 4) return 2;               /* R32G32B32A32 */
    if (f >= 9 && f <= 14) return 10;             /* R16G16B16A16 */
    if (f >= 15 && f <= 18) return 16;            /* R32G32 */
    if (f >= 19 && f <= 22) return 20;            /* R32G8X24, D32_FLOAT_S8X24 */
    if (f >= 23 && f <= 25) return 24;            /* R10G10B10A2 */
    if (f >= 27 && f <= 32) return 28;            /* R8G8B8A8 */
    if (f >= 33 && f <= 38) return 34;            /* R16G16 */
    if (f >= 39 && f <= 43) return 41;            /* R32, D32_FLOAT */
    if (f >= 44 && f <= 47) return 45;            /* R24G8, D24S8 */
    if (f >= 48 && f <= 52) return 49;            /* R8G8 */
    if (f >= 53 && f <= 59) return 54;            /* R16, D16 */
    if (f >= 60 && f <= 64) return 61;            /* R8 */
    if (f == 90 || f == 91) return 87;            /* B8G8R8A8 */
    if (f == 92 || f == 93) return 88;            /* B8G8R8X8 */
    return f;
}

static void APIENTRY d11_check_ms(D3D10DDI_HDEVICE h, DXGI_FORMAT f, UINT count, UINT *levels)
{ D11_TRACE_FEW("d11_check_ms");
    /* One quality level at each count the host has for the format's size
     * class (fmt_msaa_counts: register set v16, the executor asked its own
     * device), in the pattern the host's own drivers answer in (WARP and
     * NVIDIA's, read on the PC with a probe of the same calls; 2026-11-27):
     * every format that can be a multisampled target (fmt_support), a
     * typeless one as its family does, nothing for the others (a depth
     * format's sampled and stencil views included, which still load
     * multisampled). The runtime refused every device for other patterns:
     * a typeless format at 0 ("MSAA quality for parent != child"), 8x short
     * of a format ("MSAA quality reported to be 0") */
    UINT fam = ms_family((UINT)f), of = (UINT)f, bit = count == 2 ? 1 : count == 4 ? 2 : count == 8 ? 4 : count == 16 ? 8 : 0;
    BOOL typeless = fam != of && !fmt_support(of);   /* no support of its own (a typed one keeps its sample / target bits) */
    BOOL ms;

    of = typeless ? fam : of;
    ms = (fmt_support(of) & D3D10_DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) != 0;
    (void)h;
    *levels = count == 1 ? 1 : ms && (fmt_msaa_counts(of) & bit) ? 1 : 0;
    if (g_d11_verbose && count <= 8) {
        umd_log("d3d11: CheckMultisampleQualityLevels %u x%u -> %u", f, count, *levels);
    }
}

static void APIENTRY d11_counter_info(D3D10DDI_HDEVICE h, D3D10DDI_COUNTER_INFO *c)
{ D11_TRACE("d11_counter_info");
    (void)h;
    ZeroMemory(c, sizeof(*c));                    /* no device-dependent counters */
}

static void APIENTRY d11_check_counter(D3D10DDI_HDEVICE h, D3D10DDI_QUERY q, D3D10DDI_COUNTER_TYPE *type, UINT *active, LPSTR name,
                                       UINT *name_len, LPSTR units, UINT *units_len, LPSTR desc, UINT *desc_len)
{ D11_TRACE("d11_check_counter");
    (void)q; (void)type; (void)active; (void)name; (void)name_len; (void)units; (void)units_len; (void)desc; (void)desc_len;
    set_error(DEV(h), E_INVALIDARG);
}

static void APIENTRY d11_text_filter(D3D10DDI_HDEVICE h, UINT w, UINT ht)
{ D11_TRACE("d11_text_filter");
    (void)h; (void)w; (void)ht;
}

static void APIENTRY d11_relocate(D3D10DDI_HDEVICE h, struct D3D11DDI_DEVICEFUNCS *f)
{ D11_TRACE("d11_relocate");
    (void)h; (void)f;                             /* nothing keeps the table's address */
}

/* the 11.1 interface's: a flush says whether it submitted anything */
static BOOL APIENTRY d11_flush1(D3D10DDI_HDEVICE h, UINT flags)
{ D11_TRACE("d11_flush1");
    (void)flags;
    cmd_flush(DEV(h));
    return TRUE;
}

static void APIENTRY d11_relocate1(D3D10DDI_HDEVICE h, struct D3D11_1DDI_DEVICEFUNCS *f)
{ D11_TRACE("d11_relocate1");
    (void)h; (void)f;
}

/* the 11.1 interface's sub-objects: the video functions (d11_video.c) */
static HRESULT APIENTRY d11_sub_object(D3D10DDI_HDEVICE h, UINT32 id, SIZE_T in_size, void *in, SIZE_T out_size, void *out)
{ D11_TRACE("d11_sub_object");
    (void)h; (void)out_size; (void)out;
    if (id == D3D11_1DDI_VIDEO_FUNCTIONS && in && in_size >= sizeof(D3D11_1DDI_VIDEO_INPUT)) {
        D3D11_1DDI_VIDEO_INPUT *v = (D3D11_1DDI_VIDEO_INPUT *)in;

        if (v->p11VideoDeviceFuncs) {
            d11_video_funcs(v->p11VideoDeviceFuncs);
        }
        return S_OK;
    }
    umd_log("d3d11: RetrieveSubObject %u (%u bytes in): refused", id, (UINT)in_size);
    return E_INVALIDARG;
}

/* deferred contexts and command lists: the runtime records them itself
 * (no D3D11DDICAPS_COMMANDLISTS), so none of these is called */
static void APIENTRY d11_cl_execute(D3D10DDI_HDEVICE h, D3D11DDI_HCOMMANDLIST l) { D11_TRACE("d11_cl_execute"); (void)h; (void)l; D11_TODO("CommandListExecute"); }
static void APIENTRY d11_dc_handle_sizes(D3D10DDI_HDEVICE h, UINT *n, D3D11DDI_HANDLESIZE *s) { D11_TRACE("d11_dc_handle_sizes"); (void)h; (void)s; *n = 0; }
static SIZE_T APIENTRY d11_dc_handle_size(D3D10DDI_HDEVICE h, D3D11DDI_HANDLETYPE t, VOID *p) { D11_TRACE("d11_dc_handle_size"); (void)h; (void)t; (void)p; return 0; }
static SIZE_T APIENTRY d11_dc_size(D3D10DDI_HDEVICE h, const D3D11DDIARG_CALCPRIVATEDEFERREDCONTEXTSIZE *a) { D11_TRACE("d11_dc_size"); (void)h; (void)a; return 0; }
static void APIENTRY d11_dc_create(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEDEFERREDCONTEXT *a) { D11_TRACE("d11_dc_create"); (void)a; set_error(DEV(h), E_NOTIMPL); }
static HRESULT APIENTRY d11_dc_recycle_create(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATEDEFERREDCONTEXT *a) { D11_TRACE("d11_dc_recycle_create"); (void)h; (void)a; return E_NOTIMPL; }
static void APIENTRY d11_cl_abandon(D3D10DDI_HDEVICE h) { D11_TRACE("d11_cl_abandon"); (void)h; }
static SIZE_T APIENTRY d11_cl_size(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATECOMMANDLIST *a) { D11_TRACE("d11_cl_size"); (void)h; (void)a; return 0; }
static void APIENTRY d11_cl_create(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATECOMMANDLIST *a, D3D11DDI_HCOMMANDLIST l, D3D11DDI_HRTCOMMANDLIST rt) { D11_TRACE("d11_cl_create"); (void)a; (void)l; (void)rt; set_error(DEV(h), E_NOTIMPL); }
static void APIENTRY d11_cl_destroy(D3D10DDI_HDEVICE h, D3D11DDI_HCOMMANDLIST l) { D11_TRACE("d11_cl_destroy"); (void)h; (void)l; }
static void APIENTRY d11_cl_recycle(D3D10DDI_HDEVICE h, D3D11DDI_HCOMMANDLIST l) { D11_TRACE("d11_cl_recycle"); (void)h; (void)l; }
static HRESULT APIENTRY d11_cl_recycle_create(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATECOMMANDLIST *a, D3D11DDI_HCOMMANDLIST l, D3D11DDI_HRTCOMMANDLIST rt) { D11_TRACE("d11_cl_recycle_create"); (void)h; (void)a; (void)l; (void)rt; return E_NOTIMPL; }

/* ---------------------------------------------------------------- DXGI */

#define DXGI_DEV(p) ((D11_DEV *)(p)->hDevice)
#define DXGI_RES(x) ((D11_RES *)(x))

/* Present: the kernel driver's Present copies the buffer where it goes.
 * A blt (DWM's redirection bitmap) is a copy on the host, or the kernel
 * driver has the host put its pixels in VRAM first (d3d11_sync); a flip
 * may be an MMIO flip, which scans VRAM with no packet run, so its
 * buffer is read back here, as one the host may finish later, before the
 * display shows it (RES_SCANOUT: a readback waited for the host's GPU,
 * 2-4 ms at each of DWM's compositions; track M25) */
static HRESULT APIENTRY dxgi_present(DXGI_DDI_ARG_PRESENT *p)
{ D11_TRACE("dxgi_present");
    D11_DEV *d = DXGI_DEV(p);
    D11_RES *src = DXGI_RES(p->hSurfaceToPresent), *dst = DXGI_RES(p->hDstResource);
    DXGIDDICB_PRESENT pc;
    HRESULT hr;
    static ULONG logged;

    if (src && src->vram && !p->Flags.Blt) {
        res_ref(d, D3DPT_D11_RES_SCANOUT, src, p->SrcSubResourceIndex);
    }
    cmd_flush(d);
    ZeroMemory(&pc, sizeof(pc));
    pc.hSrcAllocation = src ? src->kmt : 0;
    pc.hDstAllocation = dst ? dst->kmt : 0;
    pc.pDXGIContext = p->pDXGIContext;
    pc.hContext = d->ctx;
    {
        ULONG64 t0 = D11_PROF_T0();

        hr = d->dxgi->pfnPresentCb(d->rt.handle, &pc);
        D11_PROF_ADD(D11P_PRESENT, t0, 0);
    }
    d11_prof_tick();
    if (FAILED(hr) || (g_d11_verbose && logged < 16)) {
        logged++;
        umd_log("d3d11: Present flags 0x%x src %p (%ux%u fmt %u) sub %u dst %p interval %u -> 0x%08x", p->Flags.Value, src,
                src ? src->r.width : 0, src ? src->r.height : 0, src ? src->r.format : 0, p->SrcSubResourceIndex, dst,
                p->FlipInterval, hr);
    }
    return hr;
}

static HRESULT APIENTRY dxgi_gamma_caps(DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS *g)
{ D11_TRACE("dxgi_gamma_caps");
    ZeroMemory(g->pGammaCapabilities, sizeof(*g->pGammaCapabilities));
    return S_OK;
}

static HRESULT APIENTRY dxgi_set_display_mode(DXGI_DDI_ARG_SETDISPLAYMODE *m)
{ D11_TRACE("dxgi_set_display_mode");
    D11_DEV *d = DXGI_DEV(m);
    D11_RES *r = DXGI_RES(m->hResource);
    D3DDDICB_SETDISPLAYMODE s;
    HRESULT hr;

    if (r && r->vram) {
        res_ref(d, D3DPT_D11_RES_READBACK, r, m->SubResourceIndex);
    }
    cmd_flush(d);
    ZeroMemory(&s, sizeof(s));
    s.hPrimaryAllocation = r ? r->kmt : 0;
    hr = d->kt->pfnSetDisplayModeCb(d->rt.handle, &s);
    if (FAILED(hr) || g_d11_verbose) {
        umd_log("d3d11: SetDisplayMode %ux%u fmt %u -> 0x%08x", r ? r->r.width : 0, r ? r->r.height : 0, r ? r->r.format : 0, hr);
    }
    return hr;
}

static HRESULT APIENTRY dxgi_set_priority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *p)
{ D11_TRACE("dxgi_set_priority");
    (void)p;
    return S_OK;
}

static HRESULT APIENTRY dxgi_residency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *q)
{ D11_TRACE("dxgi_residency");
    SIZE_T i;

    for (i = 0; i < q->Resources; i++) {
        q->pStatus[i] = DXGI_DDI_RESIDENCY_FULLY_RESIDENT;
    }
    return S_OK;
}

/* A flip model's buffers trade places: each takes the next one's
 * allocation, host object and layout (they share one description) */
static HRESULT APIENTRY dxgi_rotate(DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES *a)
{ D11_TRACE("dxgi_rotate");
    D11_RES *first;
    SIZE_T bytes;
    BYTE *tmp;
    UINT i;

    if (a->Resources < 2) {
        return S_OK;
    }
    first = DXGI_RES(a->pResources[0]);
    bytes = FIELD_OFFSET(D11_RES, sub) + first->nsub * sizeof(D11_SUB) - sizeof(D3D10DDI_HRTRESOURCE);
    tmp = (BYTE *)HeapAlloc(GetProcessHeap(), 0, bytes);
    if (!tmp) {
        return E_OUTOFMEMORY;
    }
    /* everything after the runtime's handle */
    CopyMemory(tmp, (BYTE *)first + sizeof(D3D10DDI_HRTRESOURCE), bytes);
    for (i = 0; i + 1 < a->Resources; i++) {
        D11_RES *r = DXGI_RES(a->pResources[i]), *next = DXGI_RES(a->pResources[i + 1]);

        CopyMemory((BYTE *)r + sizeof(D3D10DDI_HRTRESOURCE), (BYTE *)next + sizeof(D3D10DDI_HRTRESOURCE), bytes);
    }
    CopyMemory((BYTE *)DXGI_RES(a->pResources[a->Resources - 1]) + sizeof(D3D10DDI_HRTRESOURCE), tmp, bytes);
    HeapFree(GetProcessHeap(), 0, tmp);
    d11_views_rotated(DXGI_DEV(a), (D11_RES *const *)a->pResources, a->Resources);
    return S_OK;
}

/* a copy between a swap chain's buffers (and DWM's), scaled and
 * converted on the host */
static HRESULT APIENTRY dxgi_blt(DXGI_DDI_ARG_BLT *b)
{ D11_TRACE("dxgi_blt");
    D11_DEV *d = DXGI_DEV(b);
    d3dpt_d11_blt *t = (d3dpt_d11_blt *)tok(d, D3DPT_D11_BLT, sizeof(*t), 2);
    static ULONG logged;

    if (g_d11_verbose && logged++ < 16) {
        umd_log("d3d11: Blt flags 0x%x %u,%u..%u,%u rotate %u", b->Flags.Value, b->DstLeft, b->DstTop, b->DstRight, b->DstBottom,
                b->Rotate);
    }
    if (!t) {
        return E_OUTOFMEMORY;
    }
    patch_dst(d, &t->dst, DXGI_RES(b->hDstResource));
    t->dst_sub = b->DstSubresource;
    t->left = b->DstLeft;
    t->top = b->DstTop;
    t->right = b->DstRight;
    t->bottom = b->DstBottom;
    patch_res(d, &t->src, DXGI_RES(b->hSrcResource));
    t->src_sub = b->SrcSubresource;
    return S_OK;
}

static HRESULT APIENTRY dxgi_resolve_shared(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *r)
{ D11_TRACE("dxgi_resolve_shared");
    (void)r;
    return S_OK;
}

/* DXGI 1.2's Blt1: a source rectangle too; the whole source is the
 * DXGI 1.1 Blt, part of one is not done yet */
static HRESULT APIENTRY dxgi_blt1(DXGI_DDI_ARG_BLT1 *b)
{ D11_TRACE("dxgi_blt1");
    D11_RES *src = DXGI_RES(b->hSrcResource);
    DXGI_DDI_ARG_BLT o;

    if (src && (b->SrcLeft || b->SrcTop || (b->SrcRight && b->SrcRight != src->r.width) ||
                (b->SrcBottom && b->SrcBottom != src->r.height))) {
        D11_TODO("a DXGI Blt1 of part of its source");
    }
    ZeroMemory(&o, sizeof(o));
    o.hDevice = b->hDevice;
    o.hDstResource = b->hDstResource;
    o.DstSubresource = b->DstSubresource;
    o.DstLeft = b->DstLeft;
    o.DstTop = b->DstTop;
    o.DstRight = b->DstRight;
    o.DstBottom = b->DstBottom;
    o.hSrcResource = b->hSrcResource;
    o.SrcSubresource = b->SrcSubresource;
    o.Flags = b->Flags;
    o.Rotate = b->Rotate;
    return dxgi_blt(&o);
}

/* offered resources keep their contents: nothing is ever discarded */
static HRESULT APIENTRY dxgi_offer(DXGI_DDI_ARG_OFFERRESOURCES *o)
{ D11_TRACE("dxgi_offer");
    (void)o;
    return S_OK;
}

static HRESULT APIENTRY dxgi_reclaim(DXGI_DDI_ARG_RECLAIMRESOURCES *r)
{ D11_TRACE("dxgi_reclaim");
    UINT i;

    for (i = 0; r->pDiscarded && i < r->Resources; i++) {
        r->pDiscarded[i] = FALSE;
    }
    return S_OK;
}

/* ---------------------------------------------------------------- device */

/* the Windows 7 interface's table, which the others are made from (the
 * 11.1 one on top of it below, Direct3D 10's and 10.1's in d10_dev.c) */
void d11_device_funcs(D3D11DDI_DEVICEFUNCS *f)
{
    d11_res_funcs(f);
    d11_pipe_funcs(f);
    f->pfnFlush = d11_flush;
    f->pfnDestroyDevice = d11_destroy_device;
    f->pfnCheckFormatSupport = d11_check_format;
    f->pfnCheckMultisampleQualityLevels = d11_check_ms;
    f->pfnCheckCounterInfo = d11_counter_info;
    f->pfnCheckCounter = d11_check_counter;
    f->pfnSetTextFilterSize = d11_text_filter;
    f->pfnRelocateDeviceFuncs = d11_relocate;
    f->pfnCommandListExecute = d11_cl_execute;
    f->pfnCheckDeferredContextHandleSizes = d11_dc_handle_sizes;
    f->pfnCalcDeferredContextHandleSize = d11_dc_handle_size;
    f->pfnCalcPrivateDeferredContextSize = d11_dc_size;
    f->pfnCreateDeferredContext = d11_dc_create;
    f->pfnAbandonCommandList = d11_cl_abandon;
    f->pfnCalcPrivateCommandListSize = d11_cl_size;
    f->pfnCreateCommandList = d11_cl_create;
    f->pfnDestroyCommandList = d11_cl_destroy;
    f->pfnRecycleCommandList = d11_cl_recycle;
    f->pfnRecycleCreateCommandList = d11_cl_recycle_create;
    f->pfnRecycleCreateDeferredContext = d11_dc_recycle_create;
    f->pfnRecycleDestroyCommandList = d11_cl_destroy;
}

HRESULT APIENTRY d11_create_device(D3D10DDI_HADAPTER h, D3D10DDIARG_CREATEDEVICE *c)
{ D11_TRACE("d11_create_device");
    static LONG devices;
    D11_ADAPTER *a = (D11_ADAPTER *)h.pDrvPrivate;
    D11_DEV *d = DEV(c->hDrvDevice);
    D3DDDICB_CREATECONTEXT cc;
    D3DPT_CTX_PRIV priv;
    HRESULT hr;
    UINT ten = d10_interface(c->Interface);

    if (g_d11_verbose) {
        umd_log("d3d11: CreateDevice interface 0x%x version 0x%x flags 0x%x", c->Interface, c->Version, c->Flags);
    }
    if (c->Interface != D3D11_0_7_DDI_INTERFACE_VERSION && c->Interface != D3D11_1_DDI_INTERFACE_VERSION && !ten) {
        return E_INVALIDARG;
    }
    ZeroMemory(d, sizeof(*d));
    d->v11_1 = c->Interface == D3D11_1_DDI_INTERFACE_VERSION;
    d->rt = c->hRTDevice;
    d->core = c->hRTCoreLayer;
    d->ad = a;
    d->kt = c->pKTCallbacks;
    /* Direct3D 10's callbacks are the start of 11's, and the driver calls
     * only the first (SetErrorCb) */
    d->um = c->p11UMCallbacks;
    d->dxgi = c->DXGIBaseDDI.pDXGIBaseCallbacks;
    /* the host's context: a Direct3D 11 one, apart from the Direct3D 9
     * driver's (which are this process's id << 8 and a count) */
    d->host_ctx = 0x80000000u | ((GetCurrentProcessId() & 0xffffffu) << 7) | ((ULONG)InterlockedIncrement(&devices) & 0x7f);

    ZeroMemory(&cc, sizeof(cc));
    priv.magic = D3DPT_CTX_MAGIC;
    priv.host_ctx = d->host_ctx;
    cc.pPrivateDriverData = &priv;
    cc.PrivateDriverDataSize = sizeof(priv);
    hr = d->kt->pfnCreateContextCb(d->rt.handle, &cc);
    if (FAILED(hr) || g_d11_verbose) {
        umd_log("d3d11: context 0x%08x -> 0x%08x, buffer %u bytes, lists %u / %u", d->host_ctx, hr, cc.CommandBufferSize,
                cc.AllocationListSize, cc.PatchLocationListSize);
    }
    if (FAILED(hr)) {
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

    if (ten) {
        /* Direct3D 10 and 10.1 (d3d10.dll, d3d10_1.dll): their own tables
         * over the same entries */
        d10_device_funcs(ten, c);
    } else {
        d11_device_funcs(c->p11DeviceFuncs);
    }
    if (d->v11_1) {
        /* the 11.1 table is the 11.0 one entry for entry, then five more:
         * the entries whose arguments changed, and the new ones */
        D3D11_1DDI_DEVICEFUNCS *f1 = c->p11_1DeviceFuncs;

        d11_res_funcs11_1(f1);
        d11_pipe_funcs11_1(f1);
        f1->pfnFlush = d11_flush1;
        f1->pfnRelocateDeviceFuncs = d11_relocate1;
        if (c->ppfnRetrieveSubObject) {
            *c->ppfnRetrieveSubObject = d11_sub_object;
        }
    }

    /* DXGI 1.1's functions with the Windows 7 interface (Version's low
     * word: the runtime's DXGI DDI revision, 9 = DXGI_RESOLVE_SHARED_RESOURCE) */
    c->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnPresent = dxgi_present;
    c->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnGetGammaCaps = dxgi_gamma_caps;
    c->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetDisplayMode = dxgi_set_display_mode;
    c->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnSetResourcePriority = dxgi_set_priority;
    c->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnQueryResourceResidency = dxgi_residency;
    c->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnRotateResourceIdentities = dxgi_rotate;
    c->DXGIBaseDDI.pDXGIDDIBaseFunctions->pfnBlt = dxgi_blt;
    if ((c->Version & 0xffff) >= DXGI_RESOLVE_SHARED_RESOURCE || d->v11_1) {
        c->DXGIBaseDDI.pDXGIDDIBaseFunctions2->pfnResolveSharedResource = dxgi_resolve_shared;
    }
    if (IS_DXGI1_2_BASE_FUNCTIONS(c->Interface, c->Version)) {
        /* DXGI 1.2 with the 11.1 interface (no multiplane overlays) */
        c->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnBlt1 = dxgi_blt1;
        c->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnOfferResources = dxgi_offer;
        c->DXGIBaseDDI.pDXGIDDIBaseFunctions3->pfnReclaimResources = dxgi_reclaim;
    }
    return S_OK;
}
