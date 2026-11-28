/*
 * d11_res.c: the Direct3D 11 driver's resources (track M25).
 *
 * Every resource is one allocation, whose host handle names it on the
 * host. Its memory holds the subresources only where the guest's CPU or
 * another device must see them (vram): a staging resource (Map reads the
 * host's copy back first), a swap chain's buffer and a shared resource
 * (read back before a present). A dynamic resource is a copy in this
 * process's memory that Unmap sends to the host as an update; a default
 * one only names itself and lives on the host, its initial data and
 * UpdateSubresource sent inline.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3dpt11.h"

/* the DDI's bind flags to the API's (the low seven agree; PRESENT has no
 * API flag; UNORDERED_ACCESS moved) */
static UINT bind_api(UINT b)
{
    return (b & 0x7f) | (b & D3D11_DDI_BIND_UNORDERED_ACCESS ? API_BIND_UNORDERED_ACCESS : 0);
}

/* the misc flags both know: autogen mips, shared, draw indirect, raw and
 * structured buffers, resource clamp */
static UINT misc_api(UINT m)
{
    return m & (0x1u | 0x2u | 0x10u | 0x20u | 0x40u | 0x80u);
}

static UINT res_nsub(const D11_RES *r)
{
    return r->r.dim == 1 ? 1 : r->r.mips * r->r.array;
}

/* Every subresource in D3D11's order (mip-major within a slice), rows
 * dword-aligned (block rows for BC formats), each subresource 16-byte
 * aligned; a buffer is its bytes. The total bytes. */
static UINT layout(D11_RES *r, const D3D10DDI_MIPINFO *mi)
{
    UINT total = 0, s, m, block, bpe = fmt_bytes(r->r.format, &block);

    if (r->r.dim == 1) {
        r->sub[0].offset = 0;
        r->sub[0].row_pitch = r->sub[0].depth_pitch = r->r.width;
        r->sub[0].w = r->r.width;
        r->sub[0].h = r->sub[0].d = 1;
        return r->r.width;
    }
    if (!bpe) {
        bpe = 4;                                  /* a format we do not know: keep the memory sane */
    }
    for (s = 0; s < r->r.array; s++) {
        for (m = 0; m < r->r.mips; m++) {
            D11_SUB *u = &r->sub[m + s * r->r.mips];

            u->w = mi[m].TexelWidth;
            u->h = r->r.dim >= 3 ? mi[m].TexelHeight : 1;
            u->d = r->r.dim == 4 ? mi[m].TexelDepth : 1;
            u->row_pitch = ((u->w + block - 1) / block * bpe + 3) & ~3u;
            u->depth_pitch = u->row_pitch * ((u->h + block - 1) / block);
            u->offset = total;
            total += u->depth_pitch * u->d;
            total = (total + 15) & ~15u;
        }
    }
    return total;
}

/* 2D slices: a cube's six faces each (the count may be of cubes or of
 * faces; feature level 10_0 has no cube arrays, so 1 is a cube and 6 its
 * faces either way) */
static UINT slices(const D3D11DDIARG_CREATERESOURCE *c)
{
    if (c->ResourceDimension == D3D10DDIRESOURCE_BUFFER) {
        return 1;
    }
    if (c->ResourceDimension == D3D10DDIRESOURCE_TEXTURECUBE && c->ArraySize % 6) {
        return c->ArraySize * 6;
    }
    return c->ArraySize;
}

static SIZE_T APIENTRY d11_calc_res_size(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATERESOURCE *c)
{ D11_TRACE("d11_calc_res_size");
    UINT n = c->ResourceDimension == D3D10DDIRESOURCE_BUFFER ? 1 : c->MipLevels * slices(c);

    (void)h;
    return FIELD_OFFSET(D11_RES, sub) + (n ? n : 1) * sizeof(D11_SUB);
}

/* the subresource's region (texels; a buffer's in bytes) from a box or
 * the whole of it */
static void sub_box(const D11_RES *r, UINT sub, const D3D10_DDI_BOX *b, UINT box[6])
{
    const D11_SUB *u = &r->sub[sub];

    if (b) {
        box[0] = b->left; box[1] = b->top; box[2] = b->front;
        box[3] = b->right; box[4] = b->bottom; box[5] = b->back;
    } else {
        box[0] = box[1] = box[2] = 0;
        box[3] = u->w; box[4] = u->h; box[5] = u->d;
    }
}

/* UpdateSubresource on the host, in tokens of at most tok_max bytes: rows
 * (block rows) of one slice at a time, a buffer's bytes in pieces. src is
 * the region's first byte at the given pitches. */
static void upload_(D11_DEV *d, D11_RES *r, UINT sub, const BYTE *src, UINT row_pitch, UINT depth_pitch, const D3D10_DDI_BOX *b);

static void upload(D11_DEV *d, D11_RES *r, UINT sub, const BYTE *src, UINT row_pitch, UINT depth_pitch, const D3D10_DDI_BOX *b)
{
    ULONG64 t0 = D11_PROF_T0();
    UINT before = d->used;

    upload_(d, r, sub, src, row_pitch, depth_pitch, b);
    D11_PROF_ADD(D11P_UPLOAD, t0, d->used > before ? d->used - before : 0);
}

static void upload_(D11_DEV *d, D11_RES *r, UINT sub, const BYTE *src, UINT row_pitch, UINT depth_pitch, const D3D10_DDI_BOX *b)
{
    UINT box[6], block, bpe = fmt_bytes(r->r.format, &block), max = tok_max(d), z;

    sub_box(r, sub, b, box);
    if (box[3] <= box[0] || box[4] <= box[1] || box[5] <= box[2]) {
        return;
    }
    if (r->r.dim == 1) {
        UINT x;

        for (x = box[0]; x < box[3];) {
            UINT n = box[3] - x < max ? box[3] - x : max;
            d3dpt_d11_update *u = (d3dpt_d11_update *)tok_z(d, D3DPT_D11_RES_UPDATE, sizeof(*u) + n, sizeof(*u), 1);

            if (!u) {
                return;
            }
            patch_dst(d, &u->res, r);
            u->sub = 0;
            u->has_box = 1;
            u->box[0] = x; u->box[1] = 0; u->box[2] = 0;
            u->box[3] = x + n; u->box[4] = 1; u->box[5] = 1;
            u->row_pitch = u->depth_pitch = u->bytes = n;
            CopyMemory(u + 1, src + (x - box[0]), n);
            x += n;
        }
        return;
    }
    if (!bpe) {
        umd_log("d3d11: an update of format %u, which this driver does not size: dropped", r->r.format);
        return;
    }
    {
        UINT row_bytes = (box[3] - box[0] + block - 1) / block * bpe;
        UINT rows = (box[4] - box[1] + block - 1) / block;
        UINT per = max / (row_bytes ? row_bytes : 1);

        if (!per) {
            per = 1;
        }
        for (z = 0; z < box[5] - box[2]; z++) {
            UINT y;

            for (y = 0; y < rows; y += per) {
                UINT n = rows - y < per ? rows - y : per, i;
                d3dpt_d11_update *u = (d3dpt_d11_update *)tok_z(d, D3DPT_D11_RES_UPDATE, sizeof(*u) + n * row_bytes, sizeof(*u), 1);
                BYTE *out;

                if (!u) {
                    return;
                }
                patch_dst(d, &u->res, r);
                u->sub = sub;
                u->has_box = 1;
                u->box[0] = box[0];
                u->box[1] = box[1] + y * block;
                u->box[2] = box[2] + z;
                u->box[3] = box[3];
                u->box[4] = box[1] + (y + n) * block < box[4] ? box[1] + (y + n) * block : box[4];
                u->box[5] = box[2] + z + 1;
                u->row_pitch = row_bytes;
                u->depth_pitch = row_bytes * n;
                u->bytes = row_bytes * n;
                out = (BYTE *)(u + 1);
                for (i = 0; i < n; i++) {
                    CopyMemory(out + i * row_bytes, src + z * depth_pitch + (y + i) * row_pitch, row_bytes);
                }
            }
        }
    }
}

void res_ref(D11_DEV *d, UINT op, D11_RES *r, UINT sub)
{
    d3dpt_d11_ref *t = (d3dpt_d11_ref *)tok(d, op, sizeof(*t), 1);

    if (t) {
        /* a readback writes the allocation's VRAM, RES_DIRTY reads it */
        patch_rw(d, &t->a, r ? r->kmt : 0, op == D3DPT_D11_RES_READBACK || op == D3DPT_D11_RES_SCANOUT || (r && !r->opened));
        t->b = sub;
    }
}

/* RES_CREATE: the description and, for a resource in VRAM, its layout */
static void res_create_tok(D11_DEV *d, D11_RES *r)
{
    UINT n = r->vram ? r->nsub : 0, i;
    d3dpt_d11_res *t = (d3dpt_d11_res *)tok(d, D3DPT_D11_RES_CREATE, sizeof(*t) + n * sizeof(d3dpt_d11_sub), 1);
    d3dpt_d11_sub *s;

    if (!t) {
        return;
    }
    *t = r->r;
    patch_res(d, &t->res, r);
    t->nsub = n;
    t->opened = r->opened;
    s = (d3dpt_d11_sub *)(t + 1);
    for (i = 0; i < n; i++) {
        s[i].offset = r->sub[i].offset;
        s[i].row_pitch = r->sub[i].row_pitch;
        s[i].depth_pitch = r->sub[i].depth_pitch;
    }
}

/* a dynamic resource's CPU copy gets the initial data too, at its own pitches */
static void shadow_init(D11_RES *r, UINT i, const D3D10_DDIARG_SUBRESOURCE_UP *s)
{
    const D11_SUB *u = &r->sub[i];
    UINT block, rows, y, z, row;

    if (r->r.dim == 1) {
        CopyMemory(r->shadow, s->pSysMem, u->row_pitch);
        return;
    }
    fmt_bytes(r->r.format, &block);
    rows = (u->h + block - 1) / block;
    row = s->SysMemPitch && s->SysMemPitch < u->row_pitch ? s->SysMemPitch : u->row_pitch;
    for (z = 0; z < u->d; z++) {
        for (y = 0; y < rows; y++) {
            CopyMemory(r->shadow + u->offset + z * u->depth_pitch + y * u->row_pitch,
                       (const BYTE *)s->pSysMem + z * s->SysMemSlicePitch + y * s->SysMemPitch, row);
        }
    }
}

static void APIENTRY d11_create_resource(D3D10DDI_HDEVICE h, const D3D11DDIARG_CREATERESOURCE *c, D3D10DDI_HRESOURCE hr_,
                                         D3D10DDI_HRTRESOURCE rt)
{ D11_TRACE("d11_create_resource");
    D11_DEV *d = DEV(h);
    D11_RES *r = RES(hr_);
    D3DDDICB_ALLOCATE al;
    D3DDDI_ALLOCATIONINFO ai;
    UINT bytes, i;
    HRESULT hr;
    BOOL present = (c->BindFlags & D3D10_DDI_BIND_PRESENT) || c->pPrimaryDesc;

    ZeroMemory(r, FIELD_OFFSET(D11_RES, sub));
    r->rt = rt;
    r->r.dim = c->ResourceDimension == D3D10DDIRESOURCE_TEXTURECUBE ? 3 : (UINT)c->ResourceDimension;
    r->r.format = (UINT)c->Format;
    r->r.usage = c->Usage;
    r->r.bind = bind_api(c->BindFlags);
    r->r.cpu = (c->MapFlags & D3D10_DDI_CPU_ACCESS_WRITE ? API_CPU_WRITE : 0) | (c->MapFlags & D3D10_DDI_CPU_ACCESS_READ ? API_CPU_READ : 0);
    r->r.misc = misc_api(c->MiscFlags) | (c->ResourceDimension == D3D10DDIRESOURCE_TEXTURECUBE ? API_MISC_TEXTURECUBE : 0);
    r->r.width = c->pMipInfoList[0].TexelWidth;
    r->r.height = c->pMipInfoList[0].TexelHeight;
    r->r.depth = c->pMipInfoList[0].TexelDepth;
    r->r.mips = c->ResourceDimension == D3D10DDIRESOURCE_BUFFER ? 1 : c->MipLevels;
    r->r.array = slices(c);
    r->r.samples = c->SampleDesc.Count;
    r->r.quality = c->SampleDesc.Quality;
    r->r.stride = c->ByteStride;
    r->nsub = res_nsub(r);
    r->shared = (c->MiscFlags & D3D10_DDI_RESOURCE_MISC_SHARED) != 0;
    /* where the guest's CPU or another device reads it: in VRAM */
    r->vram = c->Usage == D3D10_DDI_USAGE_STAGING || r->shared || present;
    bytes = layout(r, c->pMipInfoList);

    r->a.magic = D3DPT_ALLOC_MAGIC;
    r->a.kind = D3DPT_ALLOC_D3D;
    r->a.caps = D3DPT_VS_D3D11;
    r->a.levels = 1;
    r->a.shared = r->shared;
    if (r->vram && r->r.dim == 3 && r->nsub == 1 && r->r.samples <= 1 && fmt_d3d9(r->r.format) && (present || r->shared)) {
        /* a surface Direct3D 9 code may also see: the kernel driver's
         * presents copy it, DWM opens it and samples it (a flip-model swap
         * chain's buffers, a composition surface), so a render-target
         * texture there */
        r->d3d9 = TRUE;
        r->a.caps |= D3DPT_VS_RENDER_TARGET | D3DPT_VS_TEXTURE;
        r->a.w = r->r.width;
        r->a.h = r->r.height;
        r->a.pitch = r->sub[0].row_pitch;
        r->a.bpp = r->sub[0].row_pitch / (r->r.width ? r->r.width : 1);
        r->a.format = fmt_d3d9(r->r.format);
        r->a.size = bytes;
    } else {
        /* a VRAM range (or a page naming a resource the host keeps alone) */
        r->a.size = r->vram && bytes ? bytes : 16;
        r->a.w = r->a.pitch = r->a.size;
        r->a.h = 1;
    }
    if (c->pPrimaryDesc) {
        r->a.source = c->pPrimaryDesc->VidPnSourceId;
        r->a.refresh_num = c->pPrimaryDesc->ModeDesc.RefreshRate.Numerator;
        r->a.refresh_den = c->pPrimaryDesc->ModeDesc.RefreshRate.Denominator;
        /* scanned out as it is, as the Direct3D 9 driver's: with NO_SCANOUT
         * DXGI never sets the display mode on it, and DWM's frames went to
         * a primary the screen never showed (track M25 step 4) */
        r->a.caps |= D3DPT_VS_PRIMARY;
    }

    ZeroMemory(&al, sizeof(al));
    ZeroMemory(&ai, sizeof(ai));
    ai.pPrivateDriverData = &r->a;
    ai.PrivateDriverDataSize = sizeof(r->a);
    ai.VidPnSourceId = c->pPrimaryDesc ? c->pPrimaryDesc->VidPnSourceId : 0;
    ai.Flags.Primary = c->pPrimaryDesc != NULL;
    /* a shared resource is a kernel resource other processes open, which
     * keeps the description for them */
    al.hResource = r->shared ? rt.handle : NULL;
    al.pPrivateDriverData = &r->r;
    al.PrivateDriverDataSize = sizeof(r->r);
    al.NumAllocations = 1;
    al.pAllocationInfo = &ai;
    hr = d->kt->pfnAllocateCb(d->rt.handle, &al);
    r->kmt = ai.hAllocation;
    {
        static ULONG logged;

        if (FAILED(hr) || (g_d11_verbose && logged < 64)) {
            logged++;
            umd_log("d3d11: CreateResource 0x%08x: dim %u fmt %u %ux%ux%u mips %u array %u usage %u bind 0x%x cpu 0x%x misc 0x%x%s%s",
                    hr, c->ResourceDimension, c->Format, r->r.width, r->r.height, r->r.depth, r->r.mips, r->r.array, c->Usage,
                    c->BindFlags, c->MapFlags, c->MiscFlags, r->vram ? " vram" : "", r->d3d9 ? " d3d9" : "");
        }
    }
    if (FAILED(hr)) {
        set_error(d, hr);
        return;
    }
    if (c->Usage == D3D10_DDI_USAGE_DYNAMIC) {
        /* the CPU's copy, and the host's as last sent (Unmap sends what
         * changed). Map hands the copy out, so it is aligned as a GPU
         * mapping is: Unigine Heaven fills its buffers with movaps, which
         * faults on the 8-byte alignment a 32-bit process's heap gives
         * (an access violation every other run in Direct3D 11) */
        UINT padded;

        r->shadow_bytes = bytes ? bytes : 16;
        padded = (r->shadow_bytes + 63) & ~63u;
        r->shadow_mem = (BYTE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, padded * 2 + 63);
        if (!r->shadow_mem) {
            set_error(d, E_OUTOFMEMORY);
            return;
        }
        r->shadow = (BYTE *)(((ULONG_PTR)r->shadow_mem + 63) & ~(ULONG_PTR)63);
        r->sent = r->shadow + padded;
    }
    res_create_tok(d, r);
    if (c->pInitialDataUP) {
        for (i = 0; i < r->nsub; i++) {
            const D3D10_DDIARG_SUBRESOURCE_UP *s = &c->pInitialDataUP[i];

            if (!s->pSysMem) {
                continue;
            }
            upload(d, r, i, (const BYTE *)s->pSysMem, s->SysMemPitch, s->SysMemSlicePitch, NULL);
            if (r->shadow) {
                shadow_init(r, i, s);
            }
        }
        if (r->shadow) {
            CopyMemory(r->sent, r->shadow, r->shadow_bytes);
        }
        r->host_newer = r->vram;                  /* the data went to the host, not to VRAM */
    }
}

static SIZE_T APIENTRY d11_calc_open_size(D3D10DDI_HDEVICE h, const D3D10DDIARG_OPENRESOURCE *o)
{ D11_TRACE("d11_calc_open_size");
    UINT n = 1;

    (void)h;
    if (o->pPrivateDriverData && o->PrivateDriverDataSize == sizeof(d3dpt_d11_res)) {
        const d3dpt_d11_res *r = (const d3dpt_d11_res *)o->pPrivateDriverData;

        n = r->dim == 1 ? 1 : r->mips * r->array;
    }
    return FIELD_OFFSET(D11_RES, sub) + (n ? n : 1) * sizeof(D11_SUB);
}

/* Another process's resource: one this driver made (its description in
 * the kernel resource's private data) or a Direct3D 9 driver's surface
 * (DWM's on Windows 11, through d3d10level9), described from its
 * allocation. Either way the host learns it by RES_CREATE, which it
 * ignores for a resource it has. */
static void APIENTRY d11_open_resource(D3D10DDI_HDEVICE h, const D3D10DDIARG_OPENRESOURCE *o, D3D10DDI_HRESOURCE hr_,
                                       D3D10DDI_HRTRESOURCE rt)
{ D11_TRACE("d11_open_resource");
    D11_DEV *d = DEV(h);
    D11_RES *r = RES(hr_);
    const D3DPT_ALLOC_DESC *a;
    D3D10DDI_MIPINFO mi[16];
    UINT i;

    ZeroMemory(r, FIELD_OFFSET(D11_RES, sub));
    r->rt = rt;
    if (o->NumAllocations != 1 || !o->pOpenAllocationInfo[0].pPrivateDriverData ||
        o->pOpenAllocationInfo[0].PrivateDriverDataSize < sizeof(*a) ||
        ((const D3DPT_ALLOC_DESC *)o->pOpenAllocationInfo[0].pPrivateDriverData)->magic != D3DPT_ALLOC_MAGIC) {
        umd_log("d3d11: OpenResource: %u allocations, not ours", o->NumAllocations);
        set_error(d, E_INVALIDARG);
        return;
    }
    a = (const D3DPT_ALLOC_DESC *)o->pOpenAllocationInfo[0].pPrivateDriverData;
    r->kmt = o->pOpenAllocationInfo[0].hAllocation;
    r->a = *a;
    r->opened = r->shared = TRUE;
    r->vram = TRUE;
    if (o->pPrivateDriverData && o->PrivateDriverDataSize == sizeof(d3dpt_d11_res)) {
        r->r = *(const d3dpt_d11_res *)o->pPrivateDriverData;
        r->d3d9 = (a->caps & D3DPT_VS_RENDER_TARGET) != 0;
    } else {
        /* a Direct3D 9 surface: a 2D texture of one level */
        r->r.dim = 3;
        r->r.format = fmt_from_d3d9(a->format);
        r->r.bind = 0x8 | 0x20;                   /* shader resource, render target */
        r->r.width = a->w;
        r->r.height = a->h;
        r->r.depth = 1;
        r->r.mips = r->r.array = r->r.samples = 1;
        r->d3d9 = TRUE;
    }
    r->r.res = 0;
    r->nsub = res_nsub(r);
    for (i = 0; i < 16; i++) {
        mi[i].TexelWidth = mi[i].PhysicalWidth = r->r.width >> i ? r->r.width >> i : 1;
        mi[i].TexelHeight = mi[i].PhysicalHeight = r->r.height >> i ? r->r.height >> i : 1;
        mi[i].TexelDepth = mi[i].PhysicalDepth = r->r.depth >> i ? r->r.depth >> i : 1;
    }
    layout(r, mi);
    if (r->d3d9 && r->nsub == 1) {
        r->sub[0].row_pitch = a->pitch;
        r->sub[0].depth_pitch = a->pitch * a->h;
    }
    if (g_d11_verbose) {
        umd_log("d3d11: OpenResource %ux%u fmt %u (Direct3D 9 format %u, caps 0x%x)", r->r.width, r->r.height, r->r.format, a->format,
                a->caps);
    }
    res_create_tok(d, r);
}

static void APIENTRY d11_destroy_resource(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_)
{ D11_TRACE("d11_destroy_resource");
    D11_DEV *d = DEV(h);
    D11_RES *r = RES(hr_);

    cmd_flush(d);                                 /* nothing queued may name it once it is gone */
    if (r->kmt && !r->opened) {
        D3DDDICB_DEALLOCATE da;

        ZeroMemory(&da, sizeof(da));
        da.hResource = r->shared ? r->rt.handle : NULL;
        da.NumAllocations = 1;
        da.HandleList = &r->kmt;
        d->kt->pfnDeallocateCb(d->rt.handle, &da);
    }
    if (r->shadow_mem) {
        HeapFree(GetProcessHeap(), 0, r->shadow_mem);
    }
    r->shadow = r->sent = r->shadow_mem = NULL;
    r->kmt = 0;
}

BYTE *res_lock(D11_DEV *d, D11_RES *r, BOOL donotwait, HRESULT *hr)
{
    D3DDDICB_LOCK lk;

    ZeroMemory(&lk, sizeof(lk));
    lk.hAllocation = r->kmt;
    lk.Flags.DonotWait = donotwait;
    *hr = d->kt->pfnLockCb(d->rt.handle, &lk);
    return SUCCEEDED(*hr) ? (BYTE *)lk.pData : NULL;
}

void res_unlock(D11_DEV *d, D11_RES *r)
{
    D3DDDICB_UNLOCK ul;

    ZeroMemory(&ul, sizeof(ul));
    ul.NumAllocations = 1;
    ul.phAllocations = &r->kmt;
    d->kt->pfnUnlockCb(d->rt.handle, &ul);
}

/* Map. A dynamic resource maps its CPU copy; one in VRAM (staging) is
 * read back from the host first and then locked, which waits until the
 * submission that read it back has run. */
static void APIENTRY d11_map(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_, UINT sub, D3D10_DDI_MAP type, UINT flags,
                             D3D10DDI_MAPPED_SUBRESOURCE *out)
{ D11_TRACE("d11_map");
    D11_DEV *d = DEV(h);
    D11_RES *r = RES(hr_);
    const D11_SUB *u;
    BYTE *base;
    HRESULT hr;
    ULONG64 t0 = D11_PROF_T0();

    if (sub >= r->nsub) {
        set_error(d, E_INVALIDARG);
        return;
    }
    u = &r->sub[sub];
    if (r->shadow) {
        base = r->shadow;
    } else if (r->vram && r->kmt) {
        UINT i;

        /* read back only what the host wrote since (a copy into it): a
         * DONOTWAIT map polled until it succeeds must not queue new work
         * each time, which kept Explorer's icon uploads "still drawing"
         * for good */
        if (r->host_newer) {
            res_ref(d, D3DPT_D11_RES_READBACK, r, ~0u);
            r->host_newer = FALSE;
        }
        /* the lock waits for what is submitted: what names it goes first */
        i = al_find(d, r->kmt);
        if (i < d->al_n) {
            cmd_flush(d);
        }
        base = res_lock(d, r, (flags & D3D10_DDI_MAP_FLAG_DONOTWAIT) != 0, &hr);
        if (!base) {
            if (hr == D3DDDIERR_WASSTILLDRAWING) {
                set_error(d, DXGI_DDI_ERR_WASSTILLDRAWING_);
            } else {
                umd_log("d3d11: Map of a %u-byte staging resource: lock 0x%08x", r->a.size, hr);
                set_error(d, hr);
            }
            return;
        }
    } else {
        umd_log("d3d11: Map of a resource the CPU never sees (usage %u)", r->r.usage);
        set_error(d, E_INVALIDARG);
        return;
    }
    r->mapped = sub + 1;
    r->map_write = type != D3D10_DDI_MAP_READ;
    out->pData = base + u->offset;
    out->RowPitch = u->row_pitch;
    out->DepthPitch = u->depth_pitch;
    D11_PROF_ADD(D11P_MAP, t0, 0);
    if (g_d11_verbose) {
        umd_log("d3d11: Map dim %u fmt %u %ux%ux%u usage %u sub %u type %u flags 0x%x -> %p row %u depth %u (%s %u bytes at %p)",
                r->r.dim, r->r.format, r->r.width, r->r.height, r->r.depth, r->r.usage, sub, type, flags, out->pData,
                u->row_pitch, u->depth_pitch, r->shadow ? "shadow" : "vram", r->shadow ? r->shadow_bytes : r->a.size, base);
    }
}

static void d11_unmap_(D11_DEV *d, D11_RES *r, UINT sub);

/* 16 bytes alike? (SSE2 on x86 / x64; words elsewhere). a and b are
 * 16-byte aligned at every use */
#if (defined(_M_IX86) || defined(_M_X64)) && !defined(_M_ARM64EC)
#include <emmintrin.h>
static __inline BOOL same16(const BYTE *a, const BYTE *b)
{
    return _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_load_si128((const __m128i *)a), _mm_load_si128((const __m128i *)b))) == 0xffff;
}
#else
static __inline BOOL same16(const BYTE *a, const BYTE *b)
{
    return ((const UINT64 *)a)[0] == ((const UINT64 *)b)[0] && ((const UINT64 *)a)[1] == ((const UINT64 *)b)[1];
}
#endif

/* where a and b (64-byte aligned, n bytes) first differ (n: nowhere):
 * 64 bytes a step while alike (an unrolled SSE2 compare: the byte-wise
 * memcmp MSVC inlines here was slower than words) */
static UINT diff_first(const BYTE *a, const BYTE *b, UINT n)
{
    UINT i = 0;

    while (n - i >= 64 && same16(a + i, b + i) && same16(a + i + 16, b + i + 16) && same16(a + i + 32, b + i + 32) &&
           same16(a + i + 48, b + i + 48)) {
        i += 64;
    }
    while (i < n && a[i] == b[i]) {
        i++;
    }
    return i;
}

/* the end of their last difference, which is after `first` (one) */
static UINT diff_last(const BYTE *a, const BYTE *b, UINT first, UINT n)
{
    UINT e = n;

    while (e > first && (e & 63)) {
        if (a[e - 1] != b[e - 1]) {
            return e;
        }
        e--;
    }
    while (e - first >= 64 && same16(a + e - 64, b + e - 64) && same16(a + e - 48, b + e - 48) &&
           same16(a + e - 32, b + e - 32) && same16(a + e - 16, b + e - 16)) {
        e -= 64;
    }
    while (e > first && a[e - 1] == b[e - 1]) {
        e--;
    }
    return e;
}

/* Unmap: a dynamic resource sends what changed since the host last had it
 * (a buffer the range of bytes that differ, a texture the subresource); a
 * staging one written through the map is uploaded from VRAM. */
static void APIENTRY d11_unmap(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_, UINT sub)
{ D11_TRACE("d11_unmap");
    D11_DEV *d = DEV(h);
    D11_RES *r = RES(hr_);
    ULONG64 t0 = D11_PROF_T0();

    if (!r->mapped) {
        return;
    }
    r->mapped = 0;
    d11_unmap_(d, r, sub);
    D11_PROF_ADD(D11P_UNMAP, t0, 0);
}

static void d11_unmap_(D11_DEV *d, D11_RES *r, UINT sub)
{
    if (r->shadow) {
        const D11_SUB *u = &r->sub[sub];

        if (r->r.dim == 1) {
            UINT n = r->shadow_bytes, first = 0, last;
            D3D10_DDI_BOX b;

            /* the changed range (diff_first / diff_last: 4 KB blocks by
             * memcmp, then words, then bytes; this was a third of the
             * driver's time in Heaven, ~25 maps of 128 KB buffers a frame) */
            first = diff_first(r->shadow, r->sent, n);
            if (first == n) {
                return;
            }
            last = diff_last(r->shadow, r->sent, first, n);
            b.left = first; b.right = last;
            b.top = b.front = 0; b.bottom = b.back = 1;
            upload(d, r, 0, r->shadow + first, last - first, last - first, &b);
            CopyMemory(r->sent + first, r->shadow + first, last - first);
        } else {
            upload(d, r, sub, r->shadow + u->offset, u->row_pitch, u->depth_pitch, NULL);
        }
        return;
    }
    if (r->vram && r->kmt) {
        res_unlock(d, r);
        if (r->map_write) {
            res_ref(d, D3DPT_D11_RES_DIRTY, r, sub);
        }
    }
}

static BOOL APIENTRY d11_staging_busy(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_)
{ D11_TRACE("d11_staging_busy");
    (void)h; (void)hr_;
    return FALSE;
}

static void APIENTRY d11_update_up(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_, UINT sub, const D3D10_DDI_BOX *b, const VOID *data,
                                   UINT row_pitch, UINT depth_pitch)
{ D11_TRACE("d11_update_up");
    D11_DEV *d = DEV(h);
    D11_RES *r = RES(hr_);

    if (sub >= r->nsub || !data) {
        return;
    }
    upload(d, r, sub, (const BYTE *)data, row_pitch, depth_pitch, b);
    r->host_newer = r->vram;
}

static void APIENTRY d11_copy(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, D3D10DDI_HRESOURCE src)
{ D11_TRACE("d11_copy");
    D11_DEV *d = DEV(h);
    d3dpt_d11_ref *t = (d3dpt_d11_ref *)tok(d, D3DPT_D11_COPY, sizeof(*t), 2);

    RES(dst)->host_newer = RES(dst)->vram;
    if (t) {
        patch_dst(d, &t->a, RES(dst));
        patch_res(d, &t->b, RES(src));
    }
}

static void APIENTRY d11_copy_region(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, UINT dst_sub, UINT x, UINT y, UINT z,
                                     D3D10DDI_HRESOURCE src, UINT src_sub, const D3D10_DDI_BOX *b)
{ D11_TRACE("d11_copy_region");
    D11_DEV *d = DEV(h);
    d3dpt_d11_copy *t = (d3dpt_d11_copy *)tok(d, D3DPT_D11_COPY_REGION, sizeof(*t), 2);

    RES(dst)->host_newer = RES(dst)->vram;
    if (!t) {
        return;
    }
    patch_dst(d, &t->dst, RES(dst));
    t->dst_sub = dst_sub;
    t->x = x; t->y = y; t->z = z;
    patch_res(d, &t->src, RES(src));
    t->src_sub = src_sub;
    t->has_box = b != NULL;
    if (b) {
        t->box[0] = b->left; t->box[1] = b->top; t->box[2] = b->front;
        t->box[3] = b->right; t->box[4] = b->bottom; t->box[5] = b->back;
    }
}

static void APIENTRY d11_resolve(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, UINT dst_sub, D3D10DDI_HRESOURCE src, UINT src_sub,
                                 DXGI_FORMAT fmt)
{ D11_TRACE("d11_resolve");
    D11_DEV *d = DEV(h);
    d3dpt_d11_resolve *t = (d3dpt_d11_resolve *)tok(d, D3DPT_D11_RESOLVE, sizeof(*t), 2);

    RES(dst)->host_newer = RES(dst)->vram;
    if (t) {
        patch_dst(d, &t->dst, RES(dst));
        t->dst_sub = dst_sub;
        patch_res(d, &t->src, RES(src));
        t->src_sub = src_sub;
        t->format = (UINT)fmt;
    }
}

static void APIENTRY d11_hazard_res(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE r)
{ D11_TRACE("d11_hazard_res");
    (void)h; (void)r;                             /* the host's device orders its own work */
}

static void APIENTRY d11_hazard_srv(D3D10DDI_HDEVICE h, D3D10DDI_HSHADERRESOURCEVIEW v, D3D10DDI_HRESOURCE r)
{ D11_TRACE("d11_hazard_srv");
    (void)h; (void)v; (void)r;
}

static void APIENTRY d11_min_lod(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE r, FLOAT lod)
{ D11_TRACE("d11_min_lod");
    (void)h; (void)r; (void)lod;
    D11_TODO("SetResourceMinLOD");
}

void d11_res_funcs(D3D11DDI_DEVICEFUNCS *f)
{
    f->pfnCalcPrivateResourceSize = d11_calc_res_size;
    f->pfnCreateResource = d11_create_resource;
    f->pfnCalcPrivateOpenedResourceSize = d11_calc_open_size;
    f->pfnOpenResource = d11_open_resource;
    f->pfnDestroyResource = d11_destroy_resource;
    f->pfnResourceMap = d11_map;
    f->pfnResourceUnmap = d11_unmap;
    f->pfnDynamicIABufferMapNoOverwrite = d11_map;
    f->pfnDynamicIABufferUnmap = d11_unmap;
    f->pfnDynamicConstantBufferMapDiscard = d11_map;
    f->pfnDynamicIABufferMapDiscard = d11_map;
    f->pfnDynamicConstantBufferUnmap = d11_unmap;
    f->pfnDynamicResourceMapDiscard = d11_map;
    f->pfnDynamicResourceUnmap = d11_unmap;
    f->pfnStagingResourceMap = d11_map;
    f->pfnStagingResourceUnmap = d11_unmap;
    f->pfnResourceIsStagingBusy = d11_staging_busy;
    f->pfnDefaultConstantBufferUpdateSubresourceUP = d11_update_up;
    f->pfnResourceUpdateSubresourceUP = d11_update_up;
    f->pfnResourceCopy = d11_copy;
    f->pfnResourceCopyRegion = d11_copy_region;
    f->pfnResourceConvert = d11_copy;
    f->pfnResourceConvertRegion = d11_copy_region;
    f->pfnResourceResolveSubresource = d11_resolve;
    f->pfnResourceReadAfterWriteHazard = d11_hazard_res;
    f->pfnShaderResourceViewReadAfterWriteHazard = d11_hazard_srv;
    f->pfnSetResourceMinLOD = d11_min_lod;
}

/* The 11.1 interface's copy flags (no overwrite, discard) only promise
 * what the host's device already orders itself */
static void APIENTRY d11_update_up1(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE hr_, UINT sub, const D3D10_DDI_BOX *b, const VOID *data,
                                    UINT row_pitch, UINT depth_pitch, UINT flags)
{ D11_TRACE("d11_update_up1");
    (void)flags;
    d11_update_up(h, hr_, sub, b, data, row_pitch, depth_pitch);
}

static void APIENTRY d11_copy_region1(D3D10DDI_HDEVICE h, D3D10DDI_HRESOURCE dst, UINT dst_sub, UINT x, UINT y, UINT z,
                                      D3D10DDI_HRESOURCE src, UINT src_sub, const D3D10_DDI_BOX *b, UINT flags)
{ D11_TRACE("d11_copy_region1");
    (void)flags;
    d11_copy_region(h, dst, dst_sub, x, y, z, src, src_sub, b);
}

/* after d11_res_funcs on the same table: the entries 11.1 changed or added */
void d11_res_funcs11_1(D3D11_1DDI_DEVICEFUNCS *f)
{
    f->pfnDefaultConstantBufferUpdateSubresourceUP = d11_update_up1;
    f->pfnResourceUpdateSubresourceUP = d11_update_up1;
    f->pfnResourceCopyRegion = d11_copy_region1;
    f->pfnResourceConvertRegion = d11_copy_region1;
    f->pfnDynamicConstantBufferMapNoOverwrite = d11_map;
}
