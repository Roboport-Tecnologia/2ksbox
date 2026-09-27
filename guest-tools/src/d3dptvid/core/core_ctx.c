/*
 * core_ctx.c: Direct3D contexts (doc 19, "The split"). This holds the
 * table of render-target / Z pairs the host keeps for us, the render
 * target a context draws into, Clear2, and the readback at EndScene that
 * puts the frame where Flip / Blt / Lock expect it.
 *
 * The table is global rather than per-device on purpose. A game's
 * exclusive mode switch gives GDI a new device object (the context is
 * created on that one), and its switch back at exit gives another, before
 * the runtime's ContextDestroyAll for the process arrives. A table in the
 * device object was empty by then and the host kept the context, so the
 * game's next run had its CTX_CREATE of the same handle refused. That was
 * E_FAIL from CreateDevice and a crash in GTA 2.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windef.h>
#include <wingdi.h>
#include <ddraw.h>
#include "d3dpt_ddi.h"
#include "d3dpt_core.h"

D3DCTX d3d_ctx[D3D_MAX_CTX];
ULONG d3d_ctx_live;

D3DCTX *ctx_of(d3dpt_core *p, ULONG_PTR h)
{
    (void)p;
    if (h == 0 || h > D3D_MAX_CTX || !d3d_ctx[h - 1].used) {
        return NULL;
    }
    return &d3d_ctx[h - 1];
}

/* A context for the render target rt and the Z buffer z (both already
 * registered by the layer, which knows how to reach the surfaces). On
 * success *handle is the context the runtime will name from now on. */
HRESULT ctx_create(d3dpt_core *p, ULONG_PTR *handle, ULONG pid, ULONG rt, ULONG z)
{
    d3dpt_ctx_create *c;
    ULONG i, off, hr, iface = (ULONG)*handle;   /* on input: the runtime's interface version */

    if (!p || !p->d3d) {
        return DDERR_GENERIC;
    }
    for (i = 0; i < D3D_MAX_CTX && d3d_ctx[i].used; i++) {
    }
    if (i == D3D_MAX_CTX) {
        dbg_puts(p, "d3dptdisp: out of contexts\n");
        return DDERR_GENERIC;
    }
    off = d3dpt_enc_ret(&p->enc, 0);
    c = d3dpt_enc_cmd(&p->enc, D3DPT_OP_CTX_CREATE, sizeof(*c), 0);
    if (!c) {
        return DDERR_GENERIC;
    }
    c->handle = i + 1;
    c->ret_off = off;
    c->rt = rt;
    c->z = z;
    /* the caps this runtime was shown (core_caps.c): d3d9.dll's (interface
     * 5) claim conditional sizes on 9x only, the older runtimes' unless
     * DDF_TEX_ANYSIZE; the host clamps such a texture then (v20) */
    c->flags = 0;
    if (!(ddflags(p) & DDF_TEX_ANYSIZE) && (!(p->dx9 && iface >= 5) || p->pow2_mips)) {
        c->flags |= D3DPT_CTX_NP2_CONDITIONAL;
    }
    c->pad = 0;
    d3dpt_enc_flush(&p->enc);
    hr = p->enc.last_status ? 0x80004005u : d3dpt_enc_result(&p->enc, off)->hr;
    dbg_hex(p, "d3dptdisp: d3d context ", i + 1);
    dbg_hex(p, " rt ", c->rt);
    dbg_hex(p, " z ", c->z);
    dbg_hex(p, " pid ", pid);
    dbg_hex(p, " iface ", iface);
    dbg_hex(p, " flags ", c->flags);
    dbg_hex(p, " -> ", hr);
    dbg_puts(p, "\n");
    if (hr & 0x80000000u) {
        return DDERR_GENERIC;
    }
    memset(&d3d_ctx[i], 0, sizeof(d3d_ctx[i]));
    d3d_ctx[i].used = TRUE;
    d3d_ctx[i].pid = pid;
    d3d_ctx[i].rt = c->rt;
    d3d_ctx[i].z = c->z;
    d3d_ctx[i].iface = iface;
    d3d_ctx_live++;
    *handle = i + 1;
    return DD_OK;
}

void ctx_destroy(d3dpt_core *p, ULONG i)
{
    query_forget_ctx(p, i + 1);
    d3d_handle_op(p, D3DPT_OP_CTX_DESTROY, i + 1);
    d3d_ctx[i].used = FALSE;
    d3d_ctx_live--;
    d3dpt_enc_flush(&p->enc);
    dbg_hex(p, "d3dptdisp: d3d context gone ", i + 1);
    dbg_hex(p, " dp2 calls ", p->dp2_calls);
    dbg_puts(p, "\n");
}

HRESULT ctx_destroy_one(d3dpt_core *p, ULONG_PTR h)
{
    if (p && ctx_of(p, h)) {
        ctx_destroy(p, (ULONG)h - 1);
    }
    return DD_OK;
}

void ctx_destroy_all(d3dpt_core *p, ULONG pid)
{
    ULONG i;

    if (!p) {
        return;
    }
    for (i = 0; i < D3D_MAX_CTX; i++) {
        if (d3d_ctx[i].used && d3d_ctx[i].pid == pid) {
            ctx_destroy(p, i);
        }
    }
}

/* EndScene: the frame into the target's VRAM, where Flip / Blt / Lock expect it */
HRESULT ctx_scene_capture(d3dpt_core *p, ULONG_PTR h, BOOL end)
{
    D3DCTX *c = p ? ctx_of(p, h) : NULL;

    if (c && end) {
        d3dpt_enc_sync(&p->enc, D3DPT_OP_READBACK, c->rt);
    }
    return DD_OK;
}

/* the surfaces are the layer's to register before it calls this */
HRESULT ctx_set_render_target(d3dpt_core *p, ULONG_PTR h, ULONG rt, ULONG z)
{
    D3DCTX *c = p ? ctx_of(p, h) : NULL;
    d3dpt_u32x3 *r;

    if (!c) {
        return DDERR_GENERIC;
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_CTX_SET_RT, sizeof(*r), 0);
    if (!r) {
        return DDERR_GENERIC;
    }
    r->a = (ULONG)h;
    r->b = rt;
    r->c = z;
    r->pad = 0;
    c->rt = rt;
    c->z = z;
    return DD_OK;
}

/* the rectangles are D3DRECT_ (four LONGs), the same on every Windows */
HRESULT ctx_clear2(d3dpt_core *p, ULONG_PTR h, ULONG flags, ULONG colour, float z, ULONG stencil,
                   const void *rects, ULONG nrects)
{
    D3DCTX *c = p ? ctx_of(p, h) : NULL;
    ULONG done = 0, n;

    if (!c) {
        return DDERR_GENERIC;
    }
    do {
        d3dpt_ctx_clear *r;

        n = nrects - done;
        if (n > 64) n = 64;
        r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_CTX_CLEAR, sizeof(*r), n * sizeof(D3DRECT_));
        if (!r) {
            return DDERR_GENERIC;
        }
        r->ctx = (ULONG)h;
        r->flags = flags;
        r->color = colour;
        r->count = n;
        r->z = z;
        r->stencil = stencil;
        if (n) {
            memcpy(r + 1, (const D3DRECT_ *)rects + done, n * sizeof(D3DRECT_));
        }
        done += n;
    } while (done < nrects);
    return DD_OK;
}

/* A value in a Z buffer's own bits (v & mask, shifted down) as the IEEE
 * float the host clears to, q / m in [0, 1], with integers only. On NT
 * this runs in a kernel-mode display driver, which may not touch the FPU
 * without saving its state. 25 bits of quotient by long division, then
 * the top 24 of them as the mantissa (truncated). */
static ULONG z_unit_bits(ULONG q, ULONG m)
{
    ULONGLONG rem = q;
    ULONG frac = 0, top = 0;
    int k;

    if (!q || !m) {
        return 0;
    }
    if (q >= m) {
        return 0x3f800000u;                 /* 1.0 */
    }
    for (k = 0; k < 25; k++) {              /* frac = q / m * 2^25 */
        rem <<= 1;
        frac <<= 1;
        if (rem >= m) {
            rem -= m;
            frac |= 1;
        }
    }
    if (!frac) {
        return 0;
    }
    while (frac >> (top + 1)) {
        top++;                              /* the leading bit's position, 0..24 */
    }
    return ((ULONG)(127 + (int)top - 25) << 23) |
           ((top >= 23 ? frac >> (top - 23) : frac << (23 - top)) & 0x7fffffu);
}

/* A Z buffer the title wrote through a Lock (doc 19 §34). The host's depth
 * buffer is never read back from VRAM, so a Z buffer written to one value
 * becomes a Z-only clear on every context whose Z buffer it is. That value
 * comes from a clear done by hand (Crimson Skies writes 0 every frame) or
 * from the runtime's own depth fill, which also goes through a Lock. The
 * scan reads every 7th row, whole, and the last, about 15% of the buffer
 * top to bottom. That tells a clear from a depth image or a partial fill,
 * since no frame has that much of the screen at one depth, and the guest's
 * CPU pays for the scan every frame. bits / mask are the surface's own Z
 * format. Returns the contexts cleared. */
ULONG d3d_z_written(d3dpt_core *p, ULONG handle, ULONG bits, ULONG mask)
{
    SURF *s = surf_slot(handle, FALSE);
    const UCHAR *base;
    ULONG v0, shift = 0, y = 0, x, n = 0, i, zb;
    BOOL uniform = TRUE;
    float z;

    if (!s || !s->mem || s->sysmem || !s->w || !s->h || (bits != 16 && bits != 32)) {
        return 0;
    }
    base = (const UCHAR *)s->mem;
    if (!mask) {
        mask = bits == 32 ? 0xffffffffu : 0xffffu;
    }
    v0 = bits == 16 ? *(const USHORT *)base : *(const ULONG *)base;
    for (;;) {
        const UCHAR *row = base + (SIZE_T)y * s->pitch;

        if (bits == 16) {
            for (x = 0; x < s->w; x++) if (((const USHORT *)row)[x] != v0) { uniform = FALSE; break; }
        } else {
            for (x = 0; x < s->w; x++) if (((const ULONG *)row)[x] != v0) { uniform = FALSE; break; }
        }
        if (!uniform || y == s->h - 1) {
            break;
        }
        y = y + 7 < s->h ? y + 7 : s->h - 1;
    }
    if (uniform) {
        while (shift < 31 && !(mask & (1u << shift))) {
            shift++;
        }
        zb = z_unit_bits((v0 & mask) >> shift, mask >> shift);
        memcpy(&z, &zb, 4);
        for (i = 0; i < D3D_MAX_CTX; i++) {
            if (d3d_ctx[i].used && d3d_ctx[i].z == handle) {
                ctx_clear2(p, i + 1, 2 /* D3DCLEAR_ZBUFFER */, 0, z, 0, NULL, 0);
                n++;
            }
        }
    }
    if (p->zwrites_said < 16) {
        p->zwrites_said++;
        dbg_hex(p, "d3dpt: Z buffer written through a lock, surface ", handle);
        if (uniform) {
            dbg_hex(p, ": all ", v0);
            dbg_hex(p, " -> contexts ", n);
        } else {
            dbg_hex(p, ": not one value (row ", y);
            dbg_puts(p, ")");
        }
        dbg_puts(p, "\n");
    }
    return n;
}
