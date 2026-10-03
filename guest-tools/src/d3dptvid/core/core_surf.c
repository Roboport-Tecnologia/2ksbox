/*
 * core_surf.c: the surface table and everything the driver knows about
 * a surface (doc 19, "The split"). That is handles, mip levels, formats
 * and their row / pitch arithmetic (DXT included), VRAM offsets, the
 * registrations the host is told about, the colour-key bookkeeping, the
 * dirty ranges of a video-memory buffer, and readback.
 *
 * The OS's own surface object never appears here. The layer fills a
 * d3dpt_surf_desc (d3dpt_os_surf) and walks the attachment lists
 * (d3dpt_os_attached, d3dpt_os_next_mip), so NT's DD_SURFACE_LOCAL and
 * 9x's DDRAWI_DDRAWSURFACE_LCL, which hold the same facts at different
 * offsets, stay in the layer. The core never keeps the object past the
 * call that handed it over. The runtime frees it without always saying
 * so, and a pointer kept for the colour key once outlived its surface
 * (doc 19 §36).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windef.h>
#include <wingdi.h>
#include <ddraw.h>
#include "d3dpt_ddi.h"
#include "d3dpt_core.h"

/* --- the surface table (DX8 DDI): every surface dxg told us about, by
 * handle, VRAM and system memory alike. The DX8 tokens name vertex / index
 * buffers and the system-memory side of a TEXBLT by handle; the memory is
 * the caller's (user-mode pointers for system-memory surfaces, read only
 * inside DrawPrimitives2, which runs in the caller's context) --- */

static SURF *surf_tab;
static ULONG surf_tab_n;

SURF *surf_slot(ULONG handle, BOOL create)
{
    if (handle >= surf_tab_n) {
        SURF *t;
        ULONG n = surf_tab_n ? surf_tab_n : 256;

        if (!create || handle >= 0x100000) {
            return NULL;
        }
        while (n <= handle) n *= 2;
        t = d3dpt_os_alloc(n * sizeof(SURF));
        if (!t) {
            return NULL;
        }
        if (surf_tab) {
            memcpy(t, surf_tab, surf_tab_n * sizeof(SURF));
            d3dpt_os_free(surf_tab);
        }
        surf_tab = t;
        surf_tab_n = n;
    }
    if (!create && !surf_tab[handle].used) {
        return NULL;
    }
    return &surf_tab[handle];
}

/* bytes of one row of w pixels (blocks for DXT); 0 = unknown format */
ULONG fmt_row_bytes(ULONG f, ULONG w)
{
    switch (f) {
    case D3DFMT_X8R8G8B8_: case D3DFMT_A8R8G8B8_: case D3DFMT_D24X8_: case D3DFMT_D24S8_: case D3DFMT_D32_:
    case D3DFMT_X8L8V8U8_: case D3DFMT_Q8W8V8U8_:
    case D3DFMT_A2B10G10R10_: case D3DFMT_A8B8G8R8_: case D3DFMT_X8B8G8R8_: case D3DFMT_G16R16_: case D3DFMT_A2R10G10B10_:
    case D3DFMT_V16U16_: case D3DFMT_A2W10V10U10_: case D3DFMT_G16R16F_: case D3DFMT_R32F_:
        return w * 4;
    case D3DFMT_A16B16G16R16_: case D3DFMT_Q16W16V16U16_: case D3DFMT_A16B16G16R16F_: case D3DFMT_G32R32F_:
        return w * 8;
    case D3DFMT_A32B32G32R32F_:
        return w * 16;
    case D3DFMT_R5G6B5_: case D3DFMT_X1R5G5B5_: case D3DFMT_A1R5G5B5_: case D3DFMT_A4R4G4B4_: case D3DFMT_X4R4G4B4_:
    case D3DFMT_D16_: case D3DFMT_D15S1_: case D3DFMT_V8U8_: case D3DFMT_A8L8_: case D3DFMT_A8R3G3B2_: case D3DFMT_L6V5U5_:
    case D3DFMT_L16_: case D3DFMT_R16F_:
        return w * 2;
    case D3DFMT_P8_: case D3DFMT_L8_: case D3DFMT_A4L4_: case D3DFMT_A8_: case D3DFMT_R3G3B2_:
        return w;
    default:
        if (f == FOURCC_('D', 'X', 'T', '1')) return ((w + 3) / 4) * 8;
        if (f == FOURCC_('D', 'X', 'T', '2') || f == FOURCC_('D', 'X', 'T', '3') ||
            f == FOURCC_('D', 'X', 'T', '4') || f == FOURCC_('D', 'X', 'T', '5')) return ((w + 3) / 4) * 16;
        return 0;
    }
}

BOOL fmt_is_dxt(ULONG f)
{
    return f == FOURCC_('D', 'X', 'T', '1') || f == FOURCC_('D', 'X', 'T', '2') || f == FOURCC_('D', 'X', 'T', '3') ||
           f == FOURCC_('D', 'X', 'T', '4') || f == FOURCC_('D', 'X', 'T', '5');
}

/* the row pitch of a surface: dxg's lPitch, except that for a compressed
 * format the union holds the linear size (DDSD_LINEARSIZE), and a block
 * row is what the copies and the host want */
ULONG surf_pitch(ULONG fmt, ULONG w, ULONG lpitch)
{
    return fmt_is_dxt(fmt) ? fmt_row_bytes(fmt, w) : lpitch;
}

ULONG surf_rows(ULONG fmt, ULONG h)
{
    return fmt_is_dxt(fmt) ? (h + 3) / 4 : h;
}


/* the bytes a compressed surface of this size occupies, which dxg has to
 * be told before it can take one out of the heap (the layer's
 * DdCreateSurface) */
ULONG surf_dxt_size(ULONG fourcc, ULONG w, ULONG h)
{
    return fmt_row_bytes(fourcc, w) * ((h + 3) / 4);
}

/* A DirectX 9 lightweight mipmap (DDSCAPS3_LIGHTWEIGHTMIPMAP): d3d9.dll
 * makes the video-memory copy of a texture with a full mip chain (a managed
 * texture's, or a default-pool one's) as one surface, and the driver keeps
 * the other levels. The runtime never locks them; its TEXBLT brings every
 * level from the system-memory copy. Treated as one level, the host
 * sampled level 0 alone (M16 finding 8). The layout is the driver's own:
 * each level after the one before, dword-aligned rows (block rows for DXT),
 * down to 1x1. Fills off[] (bytes from level 0) and pitch[] for up to 16
 * levels and returns the bytes the whole chain takes; 0 for a format with
 * no known row size, which then stays one level. */
ULONG surf_lw_layout(ULONG fmt, ULONG w, ULONG h, ULONG *off, ULONG *pitch, ULONG *levels)
{
    ULONG n = 0, total = 0, rb;

    while (n < 16) {
        rb = fmt_row_bytes(fmt, w);
        if (!rb) {
            return 0;
        }
        if (!fmt_is_dxt(fmt)) {
            rb = (rb + 3) & ~3u;
        }
        if (off) off[n] = total;
        if (pitch) pitch[n] = rb;
        total += rb * surf_rows(fmt, h);
        n++;
        if (w == 1 && h == 1) {
            break;
        }
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    *levels = n;
    return total;
}

/* x / 255 (x 1..255) as the IEEE bits of a float with mbits of mantissa
 * and an exponent biased by bias, rounded to nearest: integer long
 * division only, since the kernel-mode driver keeps off the FPU */
static ULONG unorm8_float(ULONG x, ULONG mbits, ULONG bias)
{
    ULONG k = 0, q, rem, i;

    if (!x) {
        return 0;
    }
    while ((x << k) < 255) k++;                 /* x * 2^k / 255 in [1, 2) */
    rem = x << k;
    q = rem / 255;
    rem %= 255;
    for (i = 0; i < mbits; i++) {
        rem <<= 1;
        q = (q << 1) | (rem >= 255);
        if (rem >= 255) rem -= 255;
    }
    if (2 * rem >= 255) q++;                    /* round; q may reach 2^(mbits+1), which the add carries into the exponent */
    return ((bias - k) << mbits) + (q - (1u << mbits));
}

/* D3DCOLOR in fmt's layout at out (up to 16 bytes), each channel rounded
 * as a float conversion rounds it (what the host's fill would write): the
 * float formats take R, then G, B, A, as far as they have channels, and
 * the 16-bit unorm ones x * 257. Returns the texel's bytes; 0 for a format
 * with no fill here */
ULONG fill_pack(ULONG fmt, ULONG c, UCHAR *out)
{
    ULONG a = c >> 24, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff, ch[4], i, n;
    ULONG *o32 = (ULONG *)out;
    USHORT *o16 = (USHORT *)out;
#define CH(x, bits) (((x) * ((1u << (bits)) - 1) + 127) / 255)
    ch[0] = r; ch[1] = g; ch[2] = b; ch[3] = a;
    switch (fmt) {
    case D3DFMT_X8R8G8B8_: case D3DFMT_A8R8G8B8_: o32[0] = c; return 4;
    case D3DFMT_A8B8G8R8_: case D3DFMT_X8B8G8R8_: o32[0] = (c & 0xff00ff00u) | r | (b << 16); return 4;
    case D3DFMT_A2R10G10B10_: o32[0] = CH(a, 2) << 30 | CH(r, 10) << 20 | CH(g, 10) << 10 | CH(b, 10); return 4;
    case D3DFMT_A2B10G10R10_: o32[0] = CH(a, 2) << 30 | CH(b, 10) << 20 | CH(g, 10) << 10 | CH(r, 10); return 4;
    case D3DFMT_G16R16_: o32[0] = g * 257 << 16 | r * 257; return 4;
    case D3DFMT_A16B16G16R16_: for (i = 0; i < 4; i++) o16[i] = (USHORT)(ch[i] * 257); return 8;
    case D3DFMT_R5G6B5_: o16[0] = (USHORT)(CH(r, 5) << 11 | CH(g, 6) << 5 | CH(b, 5)); return 2;
    case D3DFMT_X1R5G5B5_: case D3DFMT_A1R5G5B5_: o16[0] = (USHORT)(CH(a, 1) << 15 | CH(r, 5) << 10 | CH(g, 5) << 5 | CH(b, 5)); return 2;
    case D3DFMT_A4R4G4B4_: case D3DFMT_X4R4G4B4_: o16[0] = (USHORT)(CH(a, 4) << 12 | CH(r, 4) << 8 | CH(g, 4) << 4 | CH(b, 4)); return 2;
    case D3DFMT_A8_: out[0] = (UCHAR)a; return 1;
    case D3DFMT_R16F_: case D3DFMT_G16R16F_: case D3DFMT_A16B16G16R16F_:
        n = fmt == D3DFMT_R16F_ ? 1 : fmt == D3DFMT_G16R16F_ ? 2 : 4;
        for (i = 0; i < n; i++) o16[i] = (USHORT)unorm8_float(ch[i], 10, 15);
        return n * 2;
    case D3DFMT_R32F_: case D3DFMT_G32R32F_: case D3DFMT_A32B32G32R32F_:
        n = fmt == D3DFMT_R32F_ ? 1 : fmt == D3DFMT_G32R32F_ ? 2 : 4;
        for (i = 0; i < n; i++) o32[i] = unorm8_float(ch[i], 23, 127);
        return n * 4;
    default: return 0;
    }
#undef CH
}

/* one texel of an ARGB-group format as a D3DCOLOR (an X channel reads as
 * 0xff, each field widened by repeating its top bits, so fill_pack gives
 * the texel back); FALSE for a format outside the group */
BOOL px_unpack(ULONG fmt, const UCHAR *px, ULONG *c)
{
    ULONG v, r, g, b;

    switch (fmt) {
    case D3DFMT_A8R8G8B8_: *c = *(const ULONG *)px; return TRUE;
    case D3DFMT_X8R8G8B8_: *c = *(const ULONG *)px | 0xff000000u; return TRUE;
    case D3DFMT_R5G6B5_:
        v = *(const USHORT *)px;
        r = (v >> 11) & 0x1f; g = (v >> 5) & 0x3f; b = v & 0x1f;
        *c = 0xff000000u | ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (b << 3 | b >> 2);
        return TRUE;
    case D3DFMT_X1R5G5B5_: case D3DFMT_A1R5G5B5_:
        v = *(const USHORT *)px;
        r = (v >> 10) & 0x1f; g = (v >> 5) & 0x1f; b = v & 0x1f;
        *c = ((fmt == D3DFMT_X1R5G5B5_ || (v & 0x8000)) ? 0xff000000u : 0) |
             ((r << 3 | r >> 2) << 16) | ((g << 3 | g >> 2) << 8) | (b << 3 | b >> 2);
        return TRUE;
    default:
        return FALSE;
    }
}

/* one texel from sfmt to dfmt: as it is when they are the same, else
 * through a D3DCOLOR (both in the ARGB group, walk_blt9 checked) */
void px_copy(ULONG sfmt, ULONG dfmt, UCHAR *d, const UCHAR *s, ULONG bpp)
{
    ULONG c, v[4];

    if (sfmt == dfmt) {
        memcpy(d, s, bpp);
        return;
    }
    px_unpack(sfmt, s, &c);
    memcpy(d, v, fill_pack(dfmt, c, (UCHAR *)v));
}

/* a FOURCC code that is an uncompressed D3DFORMAT (Q8W8V8U8): the runtime
 * creates a DX8 format with no DDPIXELFORMAT this way, and DirectDraw knows
 * no bit count for it, so the layer's CreateSurface gives it its pitch */
BOOL fmt_fourcc_rows(ULONG fourcc)
{
    return fourcc < 256 && fmt_row_bytes(fourcc, 1) != 0;
}

/* a render target, a primary or a member of a flip chain: the surfaces
 * the host may have drawn into and so must read back before a lock */
BOOL surf_is_target(ULONG caps)
{
    return (caps & (DDSCAPS_3DDEVICE | DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP |
                    DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) != 0;
}

/* A Lock on a video-memory buffer. The byte range the runtime gave, or
 * the whole buffer when it gave none, is kept until the Unlock reports it
 * dirty. DISCARD / NOOVERWRITE need nothing: every draw
 * before the Lock has run, because a DP2 record executes in the doorbell
 * write. */
void surf_lock_range(ULONG handle, BOOL has_rect, LONG left, LONG right)
{
    SURF *t = surf_slot(handle, FALSE);

    if (!t || !t->buffer) {
        return;
    }
    t->lock_off = 0;
    t->lock_len = t->size;
    if (has_rect && left >= 0 && right > left && (ULONG)right <= t->size) {
        t->lock_off = (ULONG)left;
        t->lock_len = (ULONG)(right - left);
    }
}

/* and the Unlock: the locked range is the guest's now (v9) */
void surf_unlock_dirty(d3dpt_core *p, ULONG handle)
{
    SURF *t = surf_slot(handle, FALSE);

    if (p->d3d && t && t->buffer && !t->sysmem) {
        d3d_dirty_range(p, handle, t->lock_off, t->lock_len);
    }
}

/* The runtime set a source colour key on a video-memory texture: the
 * host keys the texels as it expands them, so it hears the key once, and
 * the table remembers it so surf_colorkey_check does not say it again. */
void surf_colorkey_set(d3dpt_core *p, ULONG handle, ULONG lo, ULONG hi)
{
    SURF *t = surf_slot(handle, FALSE);

    if (t) {
        t->ck_on = 1;
        t->ck_lo = lo;
        t->ck_hi = hi;
    }
    d3d_colorkey_op(p, handle, lo, hi, 1);
}

/* the handle is gone (the OS destroyed the surface) */
void surf_forget(ULONG handle)
{
    SURF *t = surf_slot(handle, FALSE);

    if (t) {
        t->used = 0;
        if (t->cube) {
            d3dpt_os_free(t->cube);
            t->cube = NULL;
        }
    }
}

/* the source colour key the surface carries as it is registered, for the
 * host to hear when the texture is first bound (surf_colorkey_check) */
static void surf_key_snapshot(SURF *t, const d3dpt_surf_desc *d)
{
    BOOL on = (d->flags & DDRAWISURF_HASCKEYSRCBLT_) != 0;

    t->ck_on = 0xff;
    t->ck_src = (UCHAR)on;
    t->ck_lo = on ? d->ck_lo : 0;
    t->ck_hi = on ? d->ck_hi : 0;
}

void d3d_colorkey_op(d3dpt_core *p, ULONG handle, ULONG lo, ULONG hi, ULONG flags)
{
    d3dpt_u32x4 *k;

    if (!p->d3d || !handle) {
        return;
    }
    k = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_COLORKEY, sizeof(*k), 0);
    if (k) {
        k->a = handle;
        k->b = lo;
        k->c = hi;
        k->d = flags;
    }
}

void d3d_handle_op(d3dpt_core *p, ULONG op, ULONG handle)
{
    d3dpt_handle *h;

    if (!p->d3d || !handle) {
        return;
    }
    h = d3dpt_enc_cmd(&p->enc, op, sizeof(*h), 0);
    if (h) {
        h->handle = handle;
        h->pad = 0;
    }
}

/* VRAM_DIRTY_RANGE (v9): the guest wrote len bytes at off of a VRAM buffer */
void d3d_dirty_range(d3dpt_core *p, ULONG handle, ULONG off, ULONG len)
{
    d3dpt_u32x3 *r;

    if (!p->d3d || !handle || !len) {
        return;
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_DIRTY_RANGE, sizeof(*r), 0);
    if (r) {
        r->a = handle;
        r->b = off;
        r->c = len;
        r->pad = 0;
    }
}


/* a cube's six faces (v11), found from its root: face[0] is the root, face
 * n the attached surface of the root's size carrying face bit n. DirectDraw
 * hangs all five off the root, but the search follows the faces' own lists
 * too rather than rely on that */
static ULONG cube_faces(d3dpt_core *p, void *root, void **face)
{
    void *q[8], *att[24];
    d3dpt_surf_desc r, d;
    ULONG nq = 0, found = 1, i, k, n, f, bits;

    for (i = 0; i < 6; i++) {
        face[i] = NULL;
    }
    if (!d3dpt_os_surf(p, root, &r)) {
        return 0;
    }
    face[0] = root;
    q[nq++] = root;
    for (i = 0; i < nq && found < 6; i++) {
        n = d3dpt_os_attached_all(q[i], att, 24);
        for (k = 0; k < n; k++) {
            if (!d3dpt_os_surf(p, att[k], &d) || !(d.caps2 & DDSCAPS2_CUBEMAP_) || d.w != r.w || d.h != r.h) {
                continue;
            }
            bits = (d.caps2 & DDSCAPS2_CUBEMAP_ALLFACES_) / DDSCAPS2_CUBEMAP_POSITIVEX_;
            for (f = 0; f < 6 && !(bits & (1u << f)); f++) {
            }
            if (f == 6 || face[f]) {
                continue;
            }
            face[f] = att[k];
            found++;
            if (nq < 8) {
                q[nq++] = att[k];
            }
        }
    }
    return found;
}

/* A cube texture (v11). The table gets one entry per face under the face's
 * own handle. The root's entry carries SURF_CUBE, every face's levels, for
 * a TEXBLT between two cubes; the others are ordinary entries a face-sized
 * TEXBLT or a Lock can name. A video-memory cube goes to the host as one
 * VRAM_SURFACE with D3DPT_VS_CUBE and every face's levels, then a
 * VRAM_CUBE_FACE for each face that has a handle of its own */
static void d3d_register_cube(d3dpt_core *p, const d3dpt_surf_desc *s, ULONG fmt, ULONG caps, BOOL sysmem)
{
    void *face[6];
    d3dpt_surf_desc fd[6];
    SURF_CUBE *cb;
    d3dpt_vram_surface *r;
    d3dpt_u32x2 *tail;
    ULONG levels = 16, f, l, n, i;
    ULONG_PTR fb = sysmem ? 0 : (ULONG_PTR)p->fb;

    /* the runtime's system-memory copies come here too, some with no pixel
     * format and a shape of their own (32 x 4 for a 16-texel cube): they
     * are only ever a TEXBLT's source, which a cube's UpdateTexture was not
     * seen to use (doc 15) */
    n = (s->w == s->h && fmt) ? cube_faces(p, s->os, face) : 0;
    if (n != 6) {
        if (p->reg_lines < 4096) {
            p->reg_lines++;
            dbg_hex(p, "d3dptdisp: cube ", s->handle);
            dbg_hex(p, " not mirrored: w ", s->w);
            dbg_hex(p, " h ", s->h);
            dbg_hex(p, " faces found ", n);
            dbg_puts(p, sysmem ? " (system memory)\n" : "\n");
        }
        return;
    }
    cb = d3dpt_os_alloc(sizeof(*cb));
    if (!cb) {
        return;
    }
    for (f = 0; f < 6; f++) {
        d3dpt_surf_desc d;
        void *m;

        if (!d3dpt_os_surf(p, face[f], &d)) {
            d3dpt_os_free(cb);
            return;
        }
        cb->handle[f] = d.handle;
        fd[f] = d;
        cb->f[f][0].mem = fb + d.vidmem;
        cb->f[f][0].pitch = surf_pitch(fmt, d.w, d.pitch);
        n = 1;
        for (m = d3dpt_os_next_mip(face[f]); m && n < 16; m = d3dpt_os_next_mip(m)) {
            d3dpt_surf_desc md;

            if (!d3dpt_os_surf(p, m, &md)) {
                break;
            }
            cb->f[f][n].mem = fb + md.vidmem;
            cb->f[f][n].pitch = surf_pitch(fmt, md.w, md.pitch);
            n++;
        }
        if (n < levels) {
            levels = n;                     /* the faces agree; the fewest if they do not */
        }
    }
    for (f = 0; f < 6; f++) {
        SURF *t = cb->handle[f] ? surf_slot(cb->handle[f], TRUE) : NULL;

        if (!t) {
            continue;
        }
        if (t->cube && t->cube != cb) {
            d3dpt_os_free(t->cube);
        }
        t->cube = f == 0 ? cb : NULL;
        t->used = 1;
        surf_key_snapshot(t, &fd[f]);
        t->sysmem = (UCHAR)sysmem;
        t->buffer = 0;
        t->mem = cb->f[f][0].mem;
        t->pitch = cb->f[f][0].pitch;
        t->w = s->w;
        t->h = s->h;
        t->fmt = fmt;
        t->size = t->pitch * surf_rows(fmt, s->h);
        t->levels = (UCHAR)levels;
        t->vram_off = sysmem ? 0 : (ULONG)(t->mem - fb);
        t->lock_off = 0;
        t->lock_len = t->size;
        for (l = 1; l < levels; l++) {
            t->lv[l - 1] = cb->f[f][l];
        }
    }
    if (!cb->handle[0]) {                   /* the root always has one (d3d_register_at checked) */
        d3dpt_os_free(cb);
        return;
    }
    if (sysmem) {
        return;
    }
    for (f = 0; f < 6; f++) {
        for (l = 0; l < levels; l++) {
            ULONG w = s->w >> l ? s->w >> l : 1;

            if ((ULONGLONG)(cb->f[f][l].mem - fb) + (ULONGLONG)cb->f[f][l].pitch * surf_rows(fmt, w) > heap_end(p)) {
                return;
            }
        }
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_SURFACE, sizeof(*r), (6 * levels - 1) * sizeof(d3dpt_u32x2));
    if (!r) {
        return;
    }
    r->handle = s->handle;
    r->offset = (ULONG)(cb->f[0][0].mem - fb);
    r->width = s->w;
    r->height = s->h;
    r->pitch = cb->f[0][0].pitch;
    r->format = fmt;
    r->caps = caps | D3DPT_VS_CUBE;
    r->levels = levels;
    tail = (d3dpt_u32x2 *)(r + 1);
    for (i = 1; i < 6 * levels; i++) {
        tail[i - 1].a = (ULONG)(cb->f[i / levels][i % levels].mem - fb);
        tail[i - 1].b = cb->f[i / levels][i % levels].pitch;
    }
    for (f = 1; f < 6; f++) {
        d3dpt_u32x4 *c;

        if (!cb->handle[f]) {
            continue;
        }
        c = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_CUBE_FACE, sizeof(*c), 0);
        if (c) {
            c->a = cb->handle[f];
            c->b = s->handle;
            c->c = f;
            c->d = 0;
        }
    }
    if (p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: cube ", s->handle);
        dbg_hex(p, " edge ", s->w);
        dbg_hex(p, " levels ", levels);
        dbg_hex(p, " faces ", cb->handle[1]);
        dbg_hex(p, " ", cb->handle[2]);
        dbg_hex(p, " ", cb->handle[3]);
        dbg_hex(p, " ", cb->handle[4]);
        dbg_hex(p, " ", cb->handle[5]);
        dbg_puts(p, "\n");
    }
}

/* VRAM_SURFACE for s at the given VRAM offset (its own, or the one a flip hands it) */
void d3d_register_at(d3dpt_core *p, const d3dpt_surf_desc *s, ULONG offset, BOOL quiet)
{
    d3dpt_vram_surface *r;
    d3dpt_u32x2 lv[15];
    ULONG lvh[15] = { 0 };          /* the mip levels' own handles (0 = none known) */
    ULONG handle, fmt, caps, n = 1, i;
    void *m;
    BOOL sysmem, buffer;
    SURF *t;
    ULONG pitch0, rows0, bsize, depth;

    if (!p->d3d || !s) {
        return;
    }
    handle = s->handle;
    fmt = s->fmt;
    caps = 0;
    if (s->caps & DDSCAPS_TEXTURE) caps |= D3DPT_VS_TEXTURE;
    if (s->caps & DDSCAPS_ZBUFFER) caps |= D3DPT_VS_ZBUFFER;
    if (s->caps & DDSCAPS_3DDEVICE) caps |= D3DPT_VS_RENDER_TARGET;
    if (s->caps & (DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_FRONTBUFFER | DDSCAPS_BACKBUFFER)) caps |= D3DPT_VS_PRIMARY;
    /* v13: a multisampled render target / depth buffer carries its sample
     * count; the host renders it multisampled and resolves it into VRAM */
    if (s->samples > 1 && !(caps & D3DPT_VS_TEXTURE) && (caps & (D3DPT_VS_RENDER_TARGET | D3DPT_VS_ZBUFFER | D3DPT_VS_PRIMARY))) {
        caps |= (s->samples << D3DPT_VS_SAMPLES_SHIFT) & D3DPT_VS_SAMPLES_MASK;
    }
    pitch0 = surf_pitch(fmt, s->w, s->pitch);
    rows0 = surf_rows(fmt, s->h);
    sysmem = (s->caps & DDSCAPS_SYSTEMMEMORY) != 0;
    buffer = (s->caps & DDSCAPS_EXECUTEBUFFER_) != 0 ||
             (s->caps2 & (DDSCAPS2_VERTEXBUFFER_ | DDSCAPS2_INDEXBUFFER_)) != 0;
    bsize = buffer ? s->linear : 0;
    /* a volume texture (v12): its slices follow level 0's first one, each
     * pitch * rows apart (DdCreateSurface sized it so); every level is a
     * surface of its own with its own depth */
    depth = ((s->caps2 & DDSCAPS2_VOLUME_) && !buffer) ? (s->depth ? s->depth : 1) : 0;
    if (depth && (caps & D3DPT_VS_TEXTURE)) {
        caps |= D3DPT_VS_VOLUME;
    }
    if (buffer) {
        /* a vertex / index buffer: no pixel format; in VRAM it is the host's
         * D3DPT_VS_BUFFER (v9), a byte range a DRAW8 names */
        fmt = 0;
        caps = D3DPT_VS_BUFFER;
    }
    /* one line per surface in the QEMU log: what the host will know it as
     * (a "skipped" surface is one a later SETRENDERTARGET / TEXTUREMAP
     * would report unknown) */
    if (!quiet && p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: surface ", handle);
        dbg_hex(p, " caps ", s->caps);
        dbg_hex(p, " w ", s->w);
        dbg_hex(p, " h ", s->h);
        dbg_hex(p, " fmt ", fmt);
        dbg_hex(p, " pitch ", s->pitch);
        dbg_hex(p, " pf ", s->pf_flags);
        if (s->samples > 1) dbg_hex(p, " samples ", s->samples);
        if (s->lwmip) dbg_puts(p, " lightweight mips");
        if (s->autogen) dbg_puts(p, " autogen");
        if (s->caps2 & DDSCAPS2_VOLUME_) {
            dbg_hex(p, " volume caps2 ", s->caps2);
            dbg_hex(p, " depth ", s->depth);
            dbg_hex(p, " linear ", s->linear);
        }
        dbg_hex(p, " at ", offset);
        if (buffer) dbg_hex(p, " buffer of ", s->linear);
        if (sysmem) dbg_puts(p, " sysmem");
        else if (!fmt && !buffer) dbg_puts(p, " no format, skipped");
        dbg_puts(p, "\n");
    }
    if (!handle) {
        return;
    }
    if ((s->caps2 & DDSCAPS2_CUBEMAP_) && !buffer) {
        /* a cube texture (v11): the root (+X, level 0) brings all six
         * faces; a face reached on its own is its root's to register */
        if ((caps & D3DPT_VS_TEXTURE) && (s->caps2 & DDSCAPS2_CUBEMAP_POSITIVEX_)) {
            d3d_register_cube(p, s, fmt, caps, sysmem);
        }
        return;
    }
    if ((caps & D3DPT_VS_TEXTURE) && s->autogen && !sysmem && !depth && !(s->caps2 & DDSCAPS2_CUBEMAP_)) {
        /* an autogen texture (v15): level 0 here, the rest the host's */
        caps |= D3DPT_VS_AUTOGEN;
    } else if ((caps & D3DPT_VS_TEXTURE) && s->lwmip && !sysmem && !depth) {
        /* a lightweight mipmap: the levels are the layer's DdCreateSurface
         * layout inside this surface (surf_lw_layout) */
        ULONG off[16], pl[16];

        if (surf_lw_layout(fmt, s->w, s->h, off, pl, &n)) {
            for (i = 1; i < n; i++) {
                lv[i - 1].a = offset + off[i];
                lv[i - 1].b = pl[i];
            }
        } else {
            n = 1;
        }
    } else if (caps & D3DPT_VS_TEXTURE) {
        for (m = d3dpt_os_next_mip(s->os); m && n < 16; m = d3dpt_os_next_mip(m)) {
            d3dpt_surf_desc d;

            if (!d3dpt_os_surf(p, m, &d)) {
                break;
            }
            lv[n - 1].a = d.vidmem;
            lv[n - 1].b = surf_pitch(fmt, d.w, d.pitch);
            lvh[n - 1] = d.handle;
            n++;
        }
    }
    /* the table entry (system-memory surfaces live only here) */
    t = surf_slot(handle, TRUE);
    if (t) {
        if (t->cube) {                      /* the handle was a cube's before */
            d3dpt_os_free(t->cube);
            t->cube = NULL;
        }
        t->used = 1;
        surf_key_snapshot(t, s);
        t->sysmem = sysmem;
        t->buffer = buffer;
        t->mem = sysmem ? (ULONG_PTR)s->vidmem : (ULONG_PTR)p->fb + offset;
        t->pitch = pitch0;
        t->w = s->w;
        t->h = s->h;
        t->fmt = fmt;
        t->nopf = s->pf_flags == 0xffffffffu;
        t->size = buffer ? bsize : pitch0 * rows0 * (depth ? depth : 1);
        t->depth = depth;
        t->levels = (UCHAR)n;
        t->vram_off = offset;
        t->lock_off = 0;
        t->lock_len = t->size;
        for (i = 0; i + 1 < n; i++) {
            t->lv[i].mem = sysmem ? (ULONG_PTR)lv[i].a : (ULONG_PTR)p->fb + lv[i].a;
            t->lv[i].pitch = lv[i].b;
        }
        for (i = 0; i < 15; i++) {
            t->lvh[i] = (!sysmem && (caps & D3DPT_VS_TEXTURE) && (caps & D3DPT_VS_RENDER_TARGET) && !s->lwmip && i + 1 < n) ? lvh[i] : 0;
        }
    }
    if (sysmem) {
        return;
    }
    if (buffer ? (!bsize || (ULONGLONG)offset + bsize > heap_end(p))
               : (!fmt || (ULONGLONG)offset + (ULONGLONG)pitch0 * rows0 * (depth ? depth : 1) > heap_end(p))) {
        return;
    }
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_SURFACE, sizeof(*r), (n - 1 + (depth ? 1 : 0)) * sizeof(d3dpt_u32x2));
    if (!r) {
        return;
    }
    r->handle = handle;
    r->offset = offset;
    r->width = buffer ? bsize : s->w;       /* a buffer: width = pitch = its bytes, one row */
    r->height = buffer ? 1 : s->h;
    r->pitch = buffer ? bsize : pitch0;
    r->format = fmt;
    r->caps = caps;
    r->levels = n;
    for (i = 0; i + 1 < n; i++) ((d3dpt_u32x2 *)(r + 1))[i] = lv[i];
    if (depth) {
        ((d3dpt_u32x2 *)(r + 1))[n - 1].a = depth;          /* v12: {depth, level 0's slice pitch} */
        ((d3dpt_u32x2 *)(r + 1))[n - 1].b = pitch0 * rows0;
    }
    /* v19: a render-target texture's levels under their own handles, which
     * a SETRENDERTARGET to one of them names */
    if ((caps & D3DPT_VS_TEXTURE) && (caps & D3DPT_VS_RENDER_TARGET) && !s->lwmip && !depth) {
        for (i = 0; i + 1 < n; i++) {
            d3dpt_u32x4 *ml;
            if (!lvh[i] || lvh[i] == handle) continue;
            ml = d3dpt_enc_cmd(&p->enc, D3DPT_OP_VRAM_MIP_LEVEL, sizeof(*ml), 0);
            if (!ml) break;
            ml->a = lvh[i];
            ml->b = handle;
            ml->c = i + 1;
            ml->d = 0;
        }
    }
}

void d3d_register(d3dpt_core *p, const d3dpt_surf_desc *s)
{
    d3d_register_at(p, s, s->vidmem, FALSE);
}

/* A texture a TEXTURESTAGESTATE binds. The first time, the host hears the
 * source colour key the surface was registered with, if it had one: a key
 * set before the surface was mirrored, or on a surface re-created under
 * the same handle (surf_key_snapshot). A key set later reaches
 * surf_colorkey_set through the layer's SetColorKey, which the runtime
 * calls because both layers claim the colour-key DirectDraw caps.
 *
 * Only the table is read here, never the OS's surface. Keeping the
 * surface object and reading the key off it at every bind left a pointer
 * nothing cleared when the runtime freed the surface. 3DMark2001 SE's
 * demo bound a texture whose slot pointed at a freed and reused object.
 * The read faulted inside DrawPrimitives2 with the command-window lock
 * held, a handler in the faulting thread caught it, and the next thread
 * to want the lock waited for ever (doc 19 §36). The cost is that a key
 * taken off with no SetColorKey call is no longer seen. */
void surf_colorkey_check(d3dpt_core *p, ULONG handle)
{
    SURF *t = surf_slot(handle, FALSE);

    if (!t || t->sysmem || t->buffer || t->ck_on != 0xff) {
        return;
    }
    if (p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: texture ", handle);
        dbg_hex(p, t->ck_src ? " bound, src key " : " bound, no key ", t->ck_lo);
        dbg_hex(p, "..", t->ck_hi);
        dbg_puts(p, "\n");
    }
    t->ck_on = t->ck_src;
    if (!t->ck_src) {
        return;                     /* never keyed: nothing to tell */
    }
    if (p->reg_lines < 4096) {
        p->reg_lines++;
        dbg_hex(p, "d3dptdisp: colour key of surface ", handle);
        dbg_hex(p, " on ", t->ck_lo);
        dbg_hex(p, "..", t->ck_hi);
        dbg_puts(p, "\n");
    }
    d3d_colorkey_op(p, handle, t->ck_lo, t->ck_hi, 1);
}

/* s and what is attached to it (a flip chain's other buffers, a Z
 * buffer), each under its own handle, the ones the host does not know yet
 * or knows at another offset. The runtime's CreateSurfaceEx comes for the
 * root of a complex surface. A DirectX 7 interface's flip chain gets one
 * call per member as well, but a DirectX 6 title's arrives as its primary
 * alone (GTA 2). Without this walk the back buffer, handle 2, was never
 * registered, the runtime's SETRENDERTARGET 2 was unknown to the host,
 * every frame went into the front buffer's VRAM while the flips
 * alternated the scanout, and half the frames showed the buffer nobody
 * drew. A flip chain's attach list is a ring; mip levels go with their
 * texture (d3d_register_at), so the layer leaves them out. */
void d3d_register_chain(d3dpt_core *p, void *os)
{
    void *seen[8];
    ULONG n = 0, i, j;
    d3dpt_surf_desc d;

    if (!d3dpt_os_surf(p, os, &d)) {
        return;
    }
    d3d_register(p, &d);
    seen[n++] = os;
    for (i = 0; i < n; i++) {
        void *att[8];
        ULONG na = d3dpt_os_attached(seen[i], att, 8), k;

        for (k = 0; k < na && n < 8; k++) {
            SURF *slot;

            for (j = 0; j < n && seen[j] != att[k]; j++) {
            }
            /* a cube's faces come with their root (d3d_register_cube): a
             * one-level cube's faces carry no MIPMAP cap, so the layer's
             * list has them */
            if (j < n || !d3dpt_os_surf(p, att[k], &d) || (d.caps2 & DDSCAPS2_CUBEMAP_)) {
                continue;
            }
            seen[n++] = att[k];
            slot = (d.caps & DDSCAPS_SYSTEMMEMORY) ? NULL : surf_slot(d.handle, FALSE);
            if (!slot || slot->mem != (ULONG_PTR)p->fb + d.vidmem) {
                d3d_register(p, &d);
            }
        }
    }
}

/* a video-memory surface the host knows at another offset, registered
 * again where it is now, silently. Under the runtime's flip model nothing
 * moves (DdFlip); a runtime that swaps two buffers' memory instead is
 * caught here at the next flip or lock */
void d3d_register_moved(d3dpt_core *p, void *os)
{
    d3dpt_surf_desc d;
    SURF *k;

    if (!p->d3d || !os || !d3dpt_os_surf(p, os, &d) || (d.caps & DDSCAPS_SYSTEMMEMORY)) {
        return;
    }
    k = surf_slot(d.handle, FALSE);
    if (!k) {
        d3d_register(p, &d);
    } else if (k->mem != (ULONG_PTR)p->fb + d.vidmem) {
        d3d_register_at(p, &d, d.vidmem, TRUE);
    }
}

/* the host's rendering into the surface, into its VRAM (S_FALSE = nothing pending) */
static ULONG readbacks_said;

HRESULT d3d_readback(d3dpt_core *p, ULONG handle)
{
    HRESULT hr;
    if (!p->d3d || !handle) {
        return DD_OK;
    }
    hr = (HRESULT)d3dpt_enc_sync(&p->enc, D3DPT_OP_READBACK, handle);
    /* a readback is per-frame, so say the first few and then be quiet: the
     * thing this ever has to answer is whether readbacks happen at all */
    if (readbacks_said < 8) {
        readbacks_said++;
        dbg_hex(p, "d3dptdisp: d3d_readback handle ", handle);
        dbg_hex(p, " -> ", (ULONG)hr);
        dbg_puts(p, "\n");
    }
    return hr;
}
