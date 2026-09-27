/*
 * core_dp2.c: the DrawPrimitives2 token stream (doc 19, "The split").
 * This is the walk that turns the runtime's tokens into D3DPT_DP2_*
 * records, the DirectX 8 rewrite (a draw becomes a self-contained DRAW8 naming its
 * vertex range and indices), TEXBLT, BUFFERBLT, the vs / ps 1.x
 * validation and the body-sizing table.
 *
 * It is the most expensive piece of the driver and concerns only the
 * protocol. The per-call DDI structures are field-for-field identical on
 * NT and 9x and the opcode values agree, so this walks the same bytes on
 * both. Where the command and vertex buffers live, and how a result is
 * handed back, differ; the layer handles those and passes a
 * d3dpt_dp2_call.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windef.h>
#include <wingdi.h>
#include <ddraw.h>
#include "d3dpt_ddi.h"
#include "d3dpt_core.h"

/* --- the DP2 stream: the DX7 tokens pass through, the DX8 ones are
 * rewritten (DX8 DDI, doc 15). The runtime's draw tokens name vertex and
 * index buffers the host cannot see, so each draw becomes a self-contained
 * D3DPT_DP2_DRAW8 token carrying its vertex range and indices; TEXBLT is a
 * copy done here (system memory -> VRAM, then VRAM_DIRTY); the shader
 * tokens pass through (the host keeps the shaders per context, protocol
 * v7, and a DRAW8 under a shader carries the handle in its fvf field); the
 * rest (patches, buffer blits, dirty rects) is dropped. Two passes over the
 * stream: the first measures the output and does the blits, the second
 * writes into the record. --- */

typedef struct _DP2WALK {
    d3dpt_core *p;
    const UCHAR *cmd;
    ULONG clen;
    const UCHAR *vtx;           /* the DP2 vertex buffer (user memory), from dwVertexOffset on */
    ULONG vlen, vsize, vcount;  /* its bytes (dwVertexLength * dwVertexSize), stride, vertex count */
    ULONG vall;                 /* the whole buffer from vtx on (a dxg buffer's linear size) */
    ULONG fvf;                  /* the current vertex format (SETVERTEXSHADER): an FVF, or a vertex shader handle (bit 0) */
    DP2STREAM st[D3D_MAX_STREAMS], ib;  /* the streams and the index buffer */
    ULONG freq[D3D_MAX_STREAMS]; /* SETSTREAMSOURCEFREQ's dividers (DX9 instancing, v16; 0 = never set) */
    ULONG st_um;                /* bit n: stream n is the DP2 vertex buffer */
    BOOL shader;                /* fvf is a vertex shader handle: the host reads the vertices through its declaration */
    BOOL one_stream;            /* DDF_ONE_STREAM: a draw carries stream 0 alone (the A/B) */
    BOOL needs_vb;              /* a DX7 draw token references the DP2 vertex buffer */
    BOOL dx8_filters;           /* the context is d3d8.dll's: its filter stage states are D3DTEXF_* (see tss_dx8_filter) */
    UCHAR *out;                 /* pass 2: the record's command area (NULL in pass 1) */
    ULONG outlen;
    ULONG skipped;              /* draws skipped (bad ranges, unknown buffers) */
    ULONG skip_why;             /* bits: 2 no fvf, 4 no stream, 8 stride < fvf, 16 vertex range, 32 index range, 64 prim */
    ULONG skip_info[6];         /* the first skipped draw: prim, count, voff, nverts, ioff, nindices */
    DWORD *rstates;
    BOOL eb;                    /* the stream is a DX3 execute buffer's instructions (doc 15) */
    ULONG bounce;               /* eb: offset of the first instruction the runtime must execute itself (~0 = none) */
    ULONG stop;                 /* eb: bytes of the stream walked (an EXIT or the bounce ends it early); a split: where */
    ULONG start;                /* where this record's walk begins in the stream (dp2_run splits a call: see walk) */
    BOOL can_split;             /* not an execute buffer, so a blit after a draw may end the record */
    BOOL drawn;                 /* a draw is in this record already */
    BOOL split;                 /* the walk stopped at stop, before a blit, for the next record to go on from */
    BOOL rt_set;                /* a SETRENDERTARGET was walked: rt / z are the context's target from here on */
    ULONG rt, z;
    ULONG_PTR ctx;              /* the call's context: DX9 query ids are per context */
} DP2WALK;

static ULONG prim_verts(ULONG prim, ULONG n)
{
    switch (prim) {
    case 1: return n;               /* points */
    case 2: return n * 2;           /* line list */
    case 3: return n + 1;           /* line strip */
    case 4: return n * 3;           /* triangle list */
    case 5: case 6: return n + 2;   /* strip, fan */
    default: return 0;
    }
}

/* the vertex size of an FVF (the host computes the same) */
ULONG fvf_stride(ULONG fvf)
{
    ULONG n = 0, tex = (fvf >> 8) & 0xf, i;

    switch (fvf & 0xe) {
    case 0x2: n = 12; break;
    case 0x4: n = 16; break;
    case 0x6: n = 16; break;
    case 0x8: n = 20; break;
    case 0xa: n = 24; break;
    case 0xc: n = 28; break;
    case 0xe: n = 32; break;
    default: return 0;
    }
    if (fvf & 0x10) n += 12;
    if (fvf & 0x20) n += 4;
    if (fvf & 0x40) n += 4;
    if (fvf & 0x80) n += 4;
    for (i = 0; i < tex; i++) {
        switch ((fvf >> (16 + 2 * i)) & 3) {
        case 0: n += 8; break;
        case 1: n += 12; break;
        case 2: n += 16; break;
        case 3: n += 4; break;
        }
    }
    return n;
}

/* A d3d8.dll context's filter stage state in the DDI's DX7 numbering, which
 * is what the host reads: the DX8 runtime hands a DX8 driver its own
 * D3DTEXF_* values (NONE 0, POINT 1, LINEAR 2, ANISOTROPIC 3, FLATCUBIC 4,
 * GAUSSIANCUBIC 5; measured, D3DGAME8's LINEAR mip filter arrives as 2)
 * where the DX7 runtime sent D3DTFG_* for MAGFILTER (POINT 1, LINEAR 2,
 * FLATCUBIC 3, GAUSSIANCUBIC 4, ANISOTROPIC 5) and D3DTFP_* for MIPFILTER
 * (NONE 1, POINT 2, LINEAR 3). MINFILTER's D3DTFN_* (POINT 1, LINEAR 2,
 * ANISOTROPIC 3) already agree. Read as DX7, every DX8 trilinear filter was
 * point-mipped and every point-mipped one unmipped. */
static ULONG tss_dx8_filter(ULONG state, ULONG v)
{
    if (state == 16) {                                  /* MAGFILTER */
        return v == 3 ? 5 : v == 4 ? 3 : v == 5 ? 4 : v;
    }
    if (state == 18) {                                  /* MIPFILTER */
        return v <= 2 ? v + 1 : v;
    }
    return v;
}

static void walk_put(DP2WALK *w, const void *src, ULONG bytes)
{
    if (w->out && bytes) {
        memcpy(w->out + w->outlen, src, bytes);
    }
    w->outlen += bytes;
}

static void walk_pad(DP2WALK *w)
{
    static const UCHAR zero[4] = { 0, 0, 0, 0 };
    ULONG pad = (4 - (w->outlen & 3)) & 3;

    walk_put(w, zero, pad);
}

/* a stream / the index buffer bound to a buffer of the table by handle
 * (SETSTREAMSOURCE / SETINDICES, or the context's bindings at the start of
 * a call): its memory and size; in VRAM (v9) the draws name it */
static void stream_bind(DP2STREAM *s, ULONG handle, ULONG stride)
{
    SURF *t = handle ? surf_slot(handle, FALSE) : NULL;

    s->mem = t ? t->mem : 0;
    s->bytes = t ? t->size : 0;
    s->stride = stride;
    s->handle = handle;
    s->vram = t && t->buffer && !t->sysmem;
    s->off = 0;
}

/* SETSTREAMSOURCE2 (DX9): the same binding from a byte offset on. An
 * offset past the buffer leaves nothing bound, so every draw from it is
 * skipped as out of range */
static void stream_bind_off(DP2STREAM *s, ULONG handle, ULONG off, ULONG stride)
{
    stream_bind(s, handle, stride);
    s->off = 0;
    if (!off || !s->mem) {
        return;
    }
    if (off >= s->bytes) {
        s->bytes = 0;
        return;
    }
    s->mem += off;
    s->bytes -= off;
    s->off = off;
}

/* a stream bound to the DP2 call's own vertex buffer (SETSTREAMSOURCEUM) */
static void stream_bind_um(DP2WALK *w, ULONG n, ULONG stride)
{
    DP2STREAM *s = &w->st[n];

    s->mem = (ULONG_PTR)w->vtx;
    s->bytes = w->vall;
    s->stride = stride;
    s->handle = 0;
    s->vram = FALSE;
    s->off = 0;
    w->st_um |= 1u << n;
}

/* one stream's vertices in the record: a VRAM buffer's handle and the
 * offset of the draw's first vertex, or the bytes themselves */
static void walk_stream_data(DP2WALK *w, const DP2STREAM *s, ULONG off, ULONG bytes)
{
    d3dpt_u32x2 ref;

    if (s->vram) {
        ref.a = s->handle;
        ref.b = s->off + off;
        walk_put(w, &ref, sizeof(ref));
    } else {
        walk_put(w, (const void *)(s->mem + off), bytes);
        walk_pad(w);
    }
}

/* an instanced draw's (inst instances) elements of a stream at frequency
 * f: one per divider instances for INSTANCEDATA; 0 for a stream read per
 * vertex, or when the draw is not instanced */
static ULONG inst_elements(ULONG f, ULONG inst)
{
    ULONG div = f & 0x3fffffff;

    if (!inst || !(f & D3DSTREAMSOURCE_INSTANCEDATA_) || !div) {
        return 0;
    }
    return (inst + div - 1) / div;
}

/* one self-contained draw for the host: the vertex range and the indices
 * copied into the record, or (v9) a VRAM buffer's handle and offset each.
 * Under a vertex shader (v10) every other stream bound whose range is
 * there goes along too: the driver does not know which streams the
 * declaration reads (the host does), and a DX8 draw indexes all its
 * streams with one vertex number, so stream n's range starts at the same
 * vertex as stream 0's (voff / stride), at its own stride */
static void walk_draw(DP2WALK *w, ULONG prim, ULONG count, const DP2STREAM *vs, ULONG voff, ULONG nverts,
                      ULONG ioff, ULONG nindices, ULONG min_index)
{
    D3DHAL_DP2COMMAND_ h;
    d3dpt_dp2_draw8 t;
    d3dpt_dp2_draw8_stream sh;
    d3dpt_u32x2 ref;
    ULONG stride = vs->stride, vbytes, first = 0, ext[D3D_MAX_STREAMS], next = 0, i, inst;

    /* A long non-indexed draw goes to the host in pieces. The host takes at
     * most 0x10000 vertices a draw while the caps allow 0xffff primitives.
     * DrawPrimitive(TRIANGLELIST, 0, 30000) is 90 000 vertices, legal on
     * every card of the era, and would otherwise be skipped whole with
     * nothing said to the application. Lists are cut on a primitive
     * boundary; a strip's next piece starts on the last vertices of the one before, and a triangle
     * strip's pieces start on an even triangle so their winding is kept. A
     * fan has no such cut and still skips. */
    if (!nindices && prim >= 1 && prim <= 5 && stride && prim_verts(prim, count) > 0x10000) {
        ULONG per = prim == 1 ? 0x10000 : prim == 2 ? 0x8000 : prim == 4 ? 0x5555 : 0xfffe;

        while (count) {
            ULONG n = count < per ? count : per;

            walk_draw(w, prim, n, vs, voff, 0, 0, 0, 0);
            voff += (prim == 3 || prim == 5 ? n : prim_verts(prim, n)) * stride;
            count -= n;
        }
        return;
    }

    /* a stride wider than the FVF is legal (the runtime passes the
     * application's stride for user-memory draws); under a vertex shader
     * the declaration decides what the stride must cover (checked on the
     * host, which has the declaration) */
    if (!w->fvf) w->skip_why |= 2;
    else if (!stride || !vs->mem) w->skip_why |= 4;
    else if (!w->shader && fvf_stride(w->fvf) > stride) w->skip_why |= 8;
    else if (!prim_verts(prim, count)) w->skip_why |= 64;
    if (!w->fvf || !stride || !vs->mem || (!w->shader && fvf_stride(w->fvf) > stride) || !prim_verts(prim, count)) {
        if (!w->skipped) {
            w->skip_info[0] = prim; w->skip_info[1] = count; w->skip_info[2] = voff;
            w->skip_info[3] = w->fvf; w->skip_info[4] = stride; w->skip_info[5] = nindices;
        }
        w->skipped++;
        return;
    }
    if (!nindices) {
        nverts = prim_verts(prim, count);
    }
    /* an indexed draw whose declared range runs past the buffer (a
     * NumVertices one too many, Wine's test_sysmem_draw) draws on the
     * cards of the era: its range is what the buffer has, and the host
     * checks every index against it */
    if (nindices && stride && voff < vs->bytes && nverts * stride > vs->bytes - voff && (vs->bytes - voff) / stride) {
        nverts = (vs->bytes - voff) / stride;
    }
    vbytes = nverts * stride;
    /* stream 0's stride is bounded as the other streams' are below: the host
     * refuses a draw wider than 1024 and fails the whole stream with it */
    if (nverts > 0x10000 || stride > 1024 || voff > vs->bytes || vbytes > vs->bytes - voff ||
        (nindices && (!w->ib.mem || w->ib.stride != 2 || ioff > w->ib.bytes || nindices * 2 > w->ib.bytes - ioff))) {
        if (!w->skipped) {
            w->skip_info[0] = prim; w->skip_info[1] = count; w->skip_info[2] = voff;
            w->skip_info[3] = nverts; w->skip_info[4] = ioff; w->skip_info[5] = nindices;
        }
        w->skipped++;
        w->skip_why |= (nverts > 0x10000 || stride > 1024 || voff > vs->bytes || vbytes > vs->bytes - voff) ? 16 : 32;
        return;
    }
    /* the other streams: a shader's draw only, and only where stream 0's
     * offset is a whole vertex (it always is for the runtime's own draws; a
     * stream that is bound but short, a stale binding a stream-0 shader
     * does not read, is left out, and the host skips the draw if its
     * declaration wanted it) */
    /* DX9 instancing (v16): stream 0 INDEXEDDATA | n on an indexed draw
     * under a declaration draws the geometry n times; a stream with
     * INSTANCEDATA | d goes along from its first element, one per d
     * instances, whatever the draw's vertex range */
    inst = nindices && w->shader && (w->freq[0] & D3DSTREAMSOURCE_INDEXEDDATA_) && (w->freq[0] & 0x3fffffff) ?
           (w->freq[0] & 0x3fffffff) : 0;
    if (w->shader && !w->one_stream && vs == &w->st[0] && voff % stride == 0) {
        first = voff / stride;
        for (i = 1; i < D3D_MAX_STREAMS; i++) {
            const DP2STREAM *s = &w->st[i];
            ULONG n = inst_elements(w->freq[i], inst);
            if (n && s->mem && s->stride && s->stride <= 1024 && n <= s->bytes / s->stride) {
                ext[next++] = i;
            } else if (!n && s->mem && s->stride && s->stride <= 1024 && first <= s->bytes / s->stride &&
                       nverts <= (s->bytes - first * s->stride) / s->stride) {
                ext[next++] = i;
            }
        }
    }
    w->drawn = TRUE;
    h.bCommand = (BYTE)D3DPT_DP2_DRAW8;
    h.bReserved = 0;
    h.wPrimitiveCount = 0;
    t.prim_type = prim;
    t.prim_count = count;
    t.fvf = w->fvf;
    t.stride = stride;
    t.nverts = nverts;
    t.nindices = nindices;
    t.min_index = min_index;
    t.flags = (vs->vram ? D3DPT_DRAW8_VRAM_VB : 0) | (nindices && w->ib.vram ? D3DPT_DRAW8_VRAM_IB : 0) |
              (next || inst ? D3DPT_DRAW8_STREAMS : 0);
    walk_put(w, &h, sizeof(h));
    walk_put(w, &t, sizeof(t));
    walk_stream_data(w, vs, voff, vbytes);
    if (nindices) {
        if (w->ib.vram) {
            ref.a = w->ib.handle;
            ref.b = ioff;
            walk_put(w, &ref, sizeof(ref));
        } else {
            walk_put(w, (const void *)(w->ib.mem + ioff), nindices * 2);
            walk_pad(w);
        }
    }
    if (next || inst) {
        ref.a = next;
        ref.b = inst ? w->freq[0] : 0;              /* v16: stream 0's frequency */
        walk_put(w, &ref, sizeof(ref));
        for (i = 0; i < next; i++) {
            const DP2STREAM *s = &w->st[ext[i]];
            ULONG n = inst_elements(w->freq[ext[i]], inst);
            sh.stream = ext[i];
            sh.stride = s->stride;
            sh.flags = s->vram ? D3DPT_DRAW8_VRAM_VB : 0;
            sh.freq = n ? w->freq[ext[i]] : 0;
            walk_put(w, &sh, sizeof(sh));
            if (n) {
                walk_stream_data(w, s, 0, n * s->stride);
            } else {
                walk_stream_data(w, s, first * s->stride, nverts * s->stride);
            }
        }
    }
}

/* a TEXBLT's rectangle (b: the token) from one level list to another, level
 * 0 first, every level both have; DXT in blocks.
 *
 * A level's rectangle is every texel the level-0 one touches: the left / top
 * edge rounded down and the right / bottom edge rounded up. Shifting the
 * width instead drops the last texel of an odd-aligned rectangle: (5,0)-
 * (7,1) is texels 2..3 on level 1, not texel 2 alone. A DXT rectangle is
 * whole blocks from the block its left / top edge is in; rounding the
 * texel offset up to a block would copy the block to the right of it.
 * Only a dirty-rect update of a mipmapped texture reaches either; the
 * full-surface TEXBLT the probes use is the same both ways. */
static void blt_levels(ULONG fmt, ULONG src_w, ULONG src_h, const SURF_LEVEL *slv, ULONG dst_w, ULONG dst_h,
                       const SURF_LEVEL *dlv, ULONG levels, const ULONG *b)
{
    LONG dx = (LONG)b[2], dy = (LONG)b[3], sl = (LONG)b[4], st = (LONG)b[5], sr = (LONG)b[6], sb = (LONG)b[7];
    BOOL dxt = fmt_is_dxt(fmt);
    ULONG bpp = fmt_row_bytes(fmt, 1), lv;

    for (lv = 0; lv < levels; lv++) {
        ULONG_PTR smem = slv[lv].mem, dmem = dlv[lv].mem;
        ULONG spitch = slv[lv].pitch, dpitch = dlv[lv].pitch;
        ULONG sw = src_w >> lv, sh = src_h >> lv, dw = dst_w >> lv, dh = dst_h >> lv;
        ULONG x0 = (ULONG)sl >> lv, y0 = (ULONG)st >> lv, x1 = (ULONG)dx >> lv, y1 = (ULONG)dy >> lv;
        ULONG cw = (((ULONG)sr + (1u << lv) - 1) >> lv) - x0, ch = (((ULONG)sb + (1u << lv) - 1) >> lv) - y0;
        ULONG rows, rowbytes, y;

        if (!sw) sw = 1;
        if (!sh) sh = 1;
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        if (!cw) cw = 1;
        if (!ch) ch = 1;
        if (x0 + cw > sw) cw = sw > x0 ? sw - x0 : 0;
        if (y0 + ch > sh) ch = sh > y0 ? sh - y0 : 0;
        if (x1 + cw > dw) cw = dw > x1 ? dw - x1 : 0;
        if (y1 + ch > dh) ch = dh > y1 ? dh - y1 : 0;
        if (!cw || !ch || !smem || !dmem) {
            continue;
        }
        if (dxt) {
            ULONG block = fmt_row_bytes(fmt, 4);        /* one 4x4 block */

            rows = (y0 + ch + 3) / 4 - y0 / 4;
            rowbytes = ((x0 + cw + 3) / 4 - x0 / 4) * block;
            smem += (y0 / 4) * spitch + (x0 / 4) * block;
            dmem += (y1 / 4) * dpitch + (x1 / 4) * block;
        } else {
            rows = ch;
            rowbytes = cw * bpp;
            smem += y0 * spitch + x0 * bpp;
            dmem += y1 * dpitch + x1 * bpp;
        }
        for (y = 0; y < rows; y++) {
            memcpy((void *)(dmem + y * dpitch), (const void *)(smem + y * spitch), rowbytes);
        }
    }
}

/* VOLUMEBLT (v12): a box of a system-memory volume -> the VRAM volume, every
 * level, then VRAM_DIRTY. The record: destination, source, the destination
 * x / y / z, the source D3DBOX (left, top, right, bottom, front, back),
 * flags; a level's slices are pitch * rows apart on both sides */
static void walk_volumeblt(DP2WALK *w, const ULONG *b)
{
    d3dpt_core *p = w->p;
    SURF *dst = surf_slot(b[0], FALSE), *src = surf_slot(b[1], FALSE);
    ULONG bpp, levels, lv, z, y, skip, w0;
    BOOL dxt, same;

    if (p->reg_lines < 4096 && p->bufblt_lines < 8) {
        p->reg_lines++;
        p->bufblt_lines++;
        dbg_hex(p, "d3dptdisp: volumeblt dst ", b[0]);
        dbg_hex(p, " src ", b[1]);
        dbg_hex(p, " at ", b[2]);
        dbg_hex(p, ",", b[3]);
        dbg_hex(p, ",", b[4]);
        dbg_hex(p, " box ", b[5]);
        dbg_hex(p, ",", b[6]);
        dbg_hex(p, "..", b[7]);
        dbg_hex(p, ",", b[8]);
        dbg_hex(p, " z ", b[9]);
        dbg_hex(p, "..", b[10]);
        dbg_hex(p, " flags ", b[11]);
        dbg_hex(p, " depths ", dst ? dst->depth : 0);
        dbg_hex(p, "/", src ? src->depth : 0);
        dbg_puts(p, "\n");
    }
    /* a system-memory source with no pixel format (d3d9.dll's, as for
     * TEXBLT) is the target's format, and so is one of another format of
     * the same texel size, as walk_texblt takes it: 9x's runtime registers
     * the source with its own format, and Wine's test_updatetexture updates
     * an A8R8G8B8 volume into an X8R8G8B8 one (dropped, the target showed
     * the VRAM's last contents) */
    if (!dst || !src) {
        return;
    }
    same = dst->fmt == src->fmt || src->nopf ||
           (!fmt_is_dxt(src->fmt) && !fmt_is_dxt(dst->fmt) && fmt_row_bytes(src->fmt, 1) &&
            fmt_row_bytes(src->fmt, 1) == fmt_row_bytes(dst->fmt, 1));
    if (!same || !dst->depth || !src->depth || !dst->fmt ||
        dst->buffer || src->buffer || b[7] <= b[5] || b[8] <= b[6] || b[10] <= b[9]) {
        return;
    }
    dxt = fmt_is_dxt(dst->fmt);
    bpp = dxt ? fmt_row_bytes(dst->fmt, 4) : fmt_row_bytes(dst->fmt, 1);    /* DXT: one 4x4 block */
    /* A source larger than the target: its top levels are left out until
     * the sizes match, the box (in the source's level 0) scaled down with
     * them, as walk_texblt does (Wine's test_updatetexture: an 8x8x8 chain
     * of 4 into a 2x2x2 chain of 2 is the source's levels 2 and 3). A
     * no-pixel-format DXT source has its width in bytes per block row */
    w0 = src->w;
    if (src->nopf && dxt && bpp) {
        w0 = src->w / bpp * 4;
    }
    for (skip = 0; skip < 15 && (w0 >> skip) > dst->w && skip + 1 < src->levels; skip++) {
    }
    levels = dst->levels < src->levels - skip ? dst->levels : src->levels - skip;
    if (levels > 16) {
        levels = 16;
    }
    for (lv = 0; lv < levels && lv + skip < 16; lv++) {
        ULONG sl = lv + skip;
        ULONG_PTR smem = sl ? src->lv[sl - 1].mem : src->mem, dmem = lv ? dst->lv[lv - 1].mem : dst->mem;
        ULONG spitch = sl ? src->lv[sl - 1].pitch : src->pitch, dpitch = lv ? dst->lv[lv - 1].pitch : dst->pitch;
        ULONG sw = src->w >> sl, sh = src->h >> sl, sd = src->depth >> sl, dw = dst->w >> lv, dh = dst->h >> lv, dd = dst->depth >> lv;
        ULONG x0 = b[5] >> sl, y0 = b[6] >> sl, z0 = b[9] >> sl, x1 = b[2] >> lv, y1 = b[3] >> lv, z1 = b[4] >> lv;
        ULONG cw = (b[7] - b[5]) >> sl, ch = (b[8] - b[6]) >> sl, cd = (b[10] - b[9]) >> sl;

        if (!sw) sw = 1;
        if (!sh) sh = 1;
        if (!sd) sd = 1;
        if (!dw) dw = 1;
        if (!dh) dh = 1;
        if (!dd) dd = 1;
        if (!cw) cw = 1;
        if (!ch) ch = 1;
        if (!cd) cd = 1;
        if (x0 + cw > sw) cw = sw > x0 ? sw - x0 : 0;
        if (y0 + ch > sh) ch = sh > y0 ? sh - y0 : 0;
        if (z0 + cd > sd) cd = sd > z0 ? sd - z0 : 0;
        if (x1 + cw > dw) cw = dw > x1 ? dw - x1 : 0;
        if (y1 + ch > dh) ch = dh > y1 ? dh - y1 : 0;
        if (z1 + cd > dd) cd = dd > z1 ? dd - z1 : 0;
        if (!cw || !ch || !cd || !smem || !dmem) {
            continue;
        }
        if (dxt) {
            /* whole blocks from the block the box's left / top edge is in
             * (blt_levels' rule); a slice is its block rows apart */
            ULONG bx0 = x0 / 4, by0 = y0 / 4, bx1 = x1 / 4, by1 = y1 / 4;
            ULONG bw = (x0 + cw + 3) / 4 - bx0, bh = (y0 + ch + 3) / 4 - by0;
            ULONG srows = (sh + 3) / 4, drows = (dh + 3) / 4;
            for (z = 0; z < cd; z++) {
                for (y = 0; y < bh; y++) {
                    memcpy((void *)(dmem + (z1 + z) * dpitch * drows + (by1 + y) * dpitch + bx1 * bpp),
                           (const void *)(smem + (z0 + z) * spitch * srows + (by0 + y) * spitch + bx0 * bpp), bw * bpp);
                }
            }
            continue;
        }
        for (z = 0; z < cd; z++) {
            for (y = 0; y < ch; y++) {
                memcpy((void *)(dmem + (z1 + z) * dpitch * dh + (y1 + y) * dpitch + x1 * bpp),
                       (const void *)(smem + (z0 + z) * spitch * sh + (y0 + y) * spitch + x0 * bpp), cw * bpp);
            }
        }
    }
    if (!dst->sysmem) {
        d3d_handle_op(p, D3DPT_OP_VRAM_DIRTY, b[0]);
    }
}

/* TEXBLT: system memory -> the VRAM texture, every level (a cube's every
 * face, v11), then VRAM_DIRTY */
static void blt_log(d3dpt_core *p, const char *what, const ULONG *b, ULONG n);

static void walk_texblt(DP2WALK *w, const ULONG *b)
{
    d3dpt_core *p = w->p;
    SURF *dst = surf_slot(b[0], FALSE), *src = surf_slot(b[1], FALSE);
    LONG dx = (LONG)b[2], dy = (LONG)b[3], sl = (LONG)b[4], st = (LONG)b[5], sr = (LONG)b[6], sb = (LONG)b[7];
    SURF_LEVEL slv[16], dlv[16];
    ULONG lv, levels, f, sfmt, sw, sh, skip, bs[8];

    /* A system-memory source with no pixel format of its own: d3d9.dll
     * registers its textures' system-memory copies so (a DXT1 one as its
     * block rows' bytes by block rows). UpdateTexture wants the two formats
     * equal, so the target's format and size are the source's (M16) */
    sfmt = src ? src->fmt : 0;
    sw = src ? src->w : 0;
    sh = src ? src->h : 0;
    if (dst && src && src->nopf && src->sysmem && dst->fmt && !dst->cube && !src->cube) {
        sfmt = dst->fmt;
        sw = dst->w;
        sh = dst->h;
    } else if (dst && src && src->sysmem && dst->fmt && sfmt != dst->fmt && !fmt_is_dxt(sfmt) && !fmt_is_dxt(dst->fmt) &&
               fmt_row_bytes(sfmt, 1) && fmt_row_bytes(sfmt, 1) == fmt_row_bytes(dst->fmt, 1)) {
        /* UpdateTexture wants one format on both sides, so a system-memory
         * source registered as another of the same texel size (d3d9.dll's
         * cube faces come as the mode's X8R8G8B8) is the target's */
        sfmt = dst->fmt;
    }
    if (!dst || !src || !dst->fmt || dst->fmt != sfmt || !src->sysmem || dst->buffer || src->buffer) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: texblt refused, dst ", b[0]);
            dbg_hex(p, " fmt ", dst ? dst->fmt : 0);
            dbg_hex(p, " w ", dst ? dst->w : 0);
            dbg_hex(p, " h ", dst ? dst->h : 0);
            dbg_hex(p, " src ", b[1]);
            dbg_hex(p, " fmt ", src ? src->fmt : 0);
            dbg_hex(p, " w ", src ? src->w : 0);
            dbg_hex(p, " h ", src ? src->h : 0);
            dbg_hex(p, " pitch ", src ? src->pitch : 0);
            dbg_hex(p, " sysmem ", src ? src->sysmem : 0);
            dbg_hex(p, " rect ", (b[4] << 16) | (b[5] & 0xffff));
            dbg_hex(p, "..", (b[6] << 16) | (b[7] & 0xffff));
            dbg_puts(p, "\n");
        }
        return;
    }
    if (sl < 0 || st < 0 || sr <= sl || sb <= st || dx < 0 || dy < 0) {
        return;
    }
    {
        ULONG v[7];
        v[0] = b[0]; v[1] = b[1]; v[2] = dst->levels; v[3] = src->levels;
        v[4] = (dst->cube ? 2 : 0) | (src->cube ? 1 : 0); v[5] = (b[4] << 16) | (b[5] & 0xffff); v[6] = (b[6] << 16) | (b[7] & 0xffff);
        blt_log(p, "texblt (dst, src, levels dst/src, cubes, rect):", v, 7);
    }
    /* A source larger than the target: its top levels are left out until
     * the sizes match, the rectangle (in the source's level 0) scaled down
     * with them. UpdateTexture from a bigger chain does this, and so does a
     * managed texture's SetLOD, for which d3d9.dll makes the video-memory
     * copy again without the levels above the LOD and TEXBLTs the whole
     * system-memory chain into it. A system-memory copy with no pixel
     * format has a DXT's width in bytes per block row */
    {
        ULONG w0 = src->w;
        if (src->nopf && fmt_is_dxt(dst->fmt) && fmt_row_bytes(dst->fmt, 4)) {
            w0 = src->w / fmt_row_bytes(dst->fmt, 4) * 4;
        }
        for (skip = 0; skip < 15 && (w0 >> skip) > dst->w && skip + 1 < src->levels; skip++) {
        }
    }
    if (skip) {
        bs[0] = b[0]; bs[1] = b[1]; bs[2] = b[2]; bs[3] = b[3];
        bs[4] = b[4] >> skip; bs[5] = b[5] >> skip;
        bs[6] = (b[6] + (1u << skip) - 1) >> skip; bs[7] = (b[7] + (1u << skip) - 1) >> skip;
        b = bs;
        if (!src->nopf) {
            sw = src->w >> skip ? src->w >> skip : 1;
            sh = src->h >> skip ? src->h >> skip : 1;
        }
        /* (a no-pixel-format source already took the target's size) */
    }
    levels = dst->levels < src->levels - skip ? dst->levels : src->levels - skip;
    if (levels > 16) {
        levels = 16;
    }
    if (dst->cube && !src->cube) {
        /* a cube's root is also its +X face: d3d9.dll updates a cube face
         * by face from system-memory faces it registers as plain surfaces */
        for (lv = 0; lv < levels; lv++) {
            if (lv + skip) {
                slv[lv] = src->lv[lv + skip - 1];
            } else {
                slv[0].mem = src->mem;
                slv[0].pitch = src->pitch;
            }
        }
        blt_levels(dst->fmt, sw, sh, slv, dst->w, dst->h, dst->cube->f[0], levels, b);
    } else if (dst->cube || src->cube) {
        /* a cube's root names the whole cube: the rectangle on every face
         * (a face's own handle is an ordinary entry, the path below) */
        if (!dst->cube || !src->cube) {
            if (p->dp2_errors < 8) {
                p->dp2_errors++;
                dbg_hex(p, "d3dptdisp: texblt between a cube and a texture refused, dst ", b[0]);
                dbg_hex(p, " src ", b[1]);
                dbg_puts(p, "\n");
            }
            return;
        }
        for (f = 0; f < 6; f++) {
            blt_levels(dst->fmt, src->w >> skip ? src->w >> skip : 1, src->h >> skip ? src->h >> skip : 1,
                       src->cube->f[f] + skip, dst->w, dst->h, dst->cube->f[f], levels, b);
        }
    } else {
        dlv[0].mem = dst->mem;
        dlv[0].pitch = dst->pitch;
        for (lv = 0; lv < levels; lv++) {
            if (lv + skip) {
                slv[lv] = src->lv[lv + skip - 1];
            } else {
                slv[0].mem = src->mem;
                slv[0].pitch = src->pitch;
            }
            if (lv) dlv[lv] = dst->lv[lv - 1];
        }
        blt_levels(dst->fmt, sw, sh, slv, dst->w, dst->h, dlv, levels, b);
    }
    if (!dst->sysmem) {
        d3d_handle_op(p, D3DPT_OP_VRAM_DIRTY, b[0]);
    }
}

/* BUFFERBLT (v9): the DX8 runtime fills a managed vertex / index buffer's
 * video-memory copy from its system-memory one (dwDDDestSurface,
 * dwDDSrcSurface, dwOffset, D3DRANGE rSrc); copied here, the range is the
 * guest's (VRAM_DIRTY_RANGE). Never sent while every buffer was in system
 * memory (before v9). */
static void walk_bufferblt(DP2WALK *w, const ULONG *b)
{
    d3dpt_core *p = w->p;
    SURF *dst = surf_slot(b[0], FALSE), *src = surf_slot(b[1], FALSE);
    ULONG doff = b[2], soff = b[3], len = b[4];

    if (!dst || !src || !dst->buffer || !src->buffer || !dst->mem || !src->mem ||
        doff > dst->size || len > dst->size - doff || soff > src->size || len > src->size - soff) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: bufferblt refused, dst ", b[0]);
            dbg_hex(p, " size ", dst ? dst->size : 0);
            dbg_hex(p, " at ", doff);
            dbg_hex(p, " src ", b[1]);
            dbg_hex(p, " size ", src ? src->size : 0);
            dbg_hex(p, " at ", soff);
            dbg_hex(p, " len ", len);
            dbg_puts(p, "\n");
        }
        return;
    }
    if (p->bufblt_lines < 8) {
        p->bufblt_lines++;
        dbg_hex(p, "d3dptdisp: bufferblt dst ", b[0]);
        dbg_hex(p, " +", doff);
        dbg_hex(p, " src ", b[1]);
        dbg_hex(p, " +", soff);
        dbg_hex(p, " len ", len);
        dbg_hex(p, " sixth ", b[5]);                /* the 24-byte token's last dword (the first cut parsed 20 and desynchronised) */
        dbg_puts(p, dst->sysmem ? " (sysmem)\n" : " (vram)\n");
    }
    if (len) {
        memcpy((void *)(dst->mem + doff), (const void *)(src->mem + soff), len);
    }
    if (!dst->sysmem) {
        d3d_dirty_range(p, b[0], doff, len);
    }
}

/* The DX9 blits (M16): BLT (StretchRect), SURFACEBLT (UpdateSurface) and
 * COLORFILL, done here in pass 1 on the surfaces' memory as TEXBLT is. A
 * video-memory surface the host may have drawn into is read back first
 * (d3d_readback; nothing happens when the host has not), and one written
 * gets VRAM_DIRTY, so the host re-reads it before it next samples or draws
 * into it. walk splits the record before any of them that follows other
 * tokens, so the host has run those by then. */

/* level lv of a surface: its memory, pitch and size; FALSE for a cube,
 * a volume, a buffer or a level it lacks */
static BOOL blt_level(const SURF *t, ULONG lv, ULONG_PTR *mem, ULONG *pitch, ULONG *w, ULONG *h)
{
    if (!t || !t->mem || t->buffer || t->cube || t->depth || !t->fmt || lv >= t->levels || lv > 15) {
        return FALSE;
    }
    *mem = lv ? t->lv[lv - 1].mem : t->mem;
    *pitch = lv ? t->lv[lv - 1].pitch : t->pitch;
    *w = t->w >> lv ? t->w >> lv : 1;
    *h = t->h >> lv ? t->h >> lv : 1;
    return *mem != 0;
}

/* the handle the host knows level lv of a surface by: a render-target
 * texture's level has its own (v19), whose readback and VRAM_DIRTY are
 * that level's; else the surface's */
static ULONG level_handle(const SURF *t, ULONG handle, ULONG lv)
{
    return (lv >= 1 && lv <= 15 && t->lvh[lv - 1]) ? t->lvh[lv - 1] : handle;
}

/* a RECTL (l, t, r, b) inside w x h, not empty */
static BOOL blt_rect_ok(const LONG *r, ULONG w, ULONG h)
{
    return r[0] >= 0 && r[1] >= 0 && r[0] < r[2] && r[1] < r[3] && (ULONG)r[2] <= w && (ULONG)r[3] <= h;
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
static ULONG fill_pack(ULONG fmt, ULONG c, UCHAR *out)
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

static void blt_log(d3dpt_core *p, const char *what, const ULONG *b, ULONG n)
{
    static ULONG lines;
    ULONG i;

    if (p->reg_lines >= 4096 || lines >= 48) {
        return;
    }
    p->reg_lines++;
    lines++;
    dbg_puts(p, "d3dptdisp: ");
    dbg_puts(p, what);
    for (i = 0; i < n; i++) dbg_hex(p, " ", b[i]);
    dbg_puts(p, "\n");
}

/* COLORFILL: surface, RECTL, D3DCOLOR */
static void walk_colorfill(DP2WALK *w, const ULONG *b)
{
    d3dpt_core *p = w->p;
    SURF *t = surf_slot(b[0], FALSE);
    ULONG_PTR mem;
    ULONG pitch, sw, sh, bpp, x, y;
    ULONG v[4];
    const LONG *r = (const LONG *)(b + 1);

    if (!blt_level(t, 0, &mem, &pitch, &sw, &sh) || !blt_rect_ok(r, sw, sh) || !(bpp = fill_pack(t->fmt, b[5], (UCHAR *)v))) {
        blt_log(p, "colorfill refused:", b, 6);
        return;
    }
    blt_log(p, "colorfill", b, 6);
    if (!t->sysmem) d3d_readback(p, b[0]);
    for (y = (ULONG)r[1]; y < (ULONG)r[3]; y++) {
        UCHAR *row = (UCHAR *)(mem + y * pitch) + r[0] * bpp;
        for (x = 0; x < (ULONG)(r[2] - r[0]); x++) {
            if (bpp == 4) ((ULONG *)row)[x] = v[0];
            else if (bpp == 2) ((USHORT *)row)[x] = (USHORT)v[0];
            else if (bpp == 1) row[x] = (UCHAR)v[0];
            else memcpy(row + x * bpp, v, bpp);
        }
    }
    if (!t->sysmem) d3d_handle_op(p, D3DPT_OP_VRAM_DIRTY, b[0]);
}

/* a depth format: the host keeps such a surface's contents */
static BOOL fmt_is_depth(ULONG f)
{
    return f == D3DFMT_D16_ || f == D3DFMT_D24X8_ || f == D3DFMT_D24S8_ || f == D3DFMT_D15S1_ || f == D3DFMT_D32_;
}

/* a BLT (StretchRect) between two video-memory depth buffers (v18) */
static BOOL blt_depth(const ULONG *b)
{
    SURF *src = surf_slot(b[0], FALSE), *dst = surf_slot(b[6], FALSE);

    return src && dst && !src->sysmem && !dst->sysmem && fmt_is_depth(src->fmt) && fmt_is_depth(dst->fmt);
}

/* one texel of an ARGB-group format as a D3DCOLOR (an X channel reads as
 * 0xff, each field widened by repeating its top bits, so fill_pack gives
 * the texel back); FALSE for a format outside the group */
static BOOL px_unpack(ULONG fmt, const UCHAR *px, ULONG *c)
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
static void px_copy(ULONG sfmt, ULONG dfmt, UCHAR *d, const UCHAR *s, ULONG bpp)
{
    ULONG c, v[4];

    if (sfmt == dfmt) {
        memcpy(d, s, bpp);
        return;
    }
    px_unpack(sfmt, s, &c);
    memcpy(d, v, fill_pack(dfmt, c, (UCHAR *)v));
}

/* BLT (StretchRect) and SURFACEBLT (UpdateSurface): source, RECTL, level,
 * destination, RECTL, level, flags. Same-size rectangles copy (DXT in
 * whole blocks); StretchRect's scaled ones take the nearest texel, as the
 * host's point filter does (and for now its linear one too). Formats of
 * one texel size copy as they are; between two others of the ARGB group
 * StretchRect converts through a D3DCOLOR. */
static void walk_blt9(DP2WALK *w, const ULONG *b, BOOL stretch)
{
    d3dpt_core *p = w->p;
    SURF *src = surf_slot(b[0], FALSE), *dst = surf_slot(b[6], FALSE);
    const LONG *sr = (const LONG *)(b + 1), *dr = (const LONG *)(b + 7);
    ULONG_PTR smem, dmem;
    ULONG spitch, dpitch, sw, sh, dw, dh, sfmt, dfmt, bpp, dbpp, x, y, cw, ch, dxt, c;
    BOOL ok;

    sfmt = src ? (src->nopf && dst ? dst->fmt : src->fmt) : 0;
    dfmt = dst ? dst->fmt : 0;
    /* a pair of formats with no plain copy between them: the ARGB group
     * converts, anything else is refused */
    if (fmt_row_bytes(sfmt, 1) != fmt_row_bytes(dfmt, 1) || (sfmt != dfmt && (fmt_is_dxt(sfmt) || fmt_is_dxt(dfmt)))) {
        UCHAR zero[16];
        memset(zero, 0, sizeof(zero));
        if (!px_unpack(sfmt, zero, &c) || !fill_pack(dfmt, 0, zero)) {
            sfmt = 0;
        }
    } else if (sfmt != dfmt && sfmt) {
        /* one texel size, two formats: copied as they are (the same
         * layout with an X or A channel, or what a game asked for) */
        dfmt = sfmt;
    }
    ok = blt_level(src, b[5], &smem, &spitch, &sw, &sh);
    if (ok && src->nopf && fmt_is_dxt(sfmt) && fmt_row_bytes(sfmt, 4)) {
        /* a system-memory DXT surface with no pixel format: its width is
         * bytes per block row and its height block rows (d3d9.dll's) */
        ULONG w0 = src->w / fmt_row_bytes(sfmt, 4) * 4, h0 = src->h * 4;
        sw = w0 >> b[5] ? w0 >> b[5] : 1;
        sh = h0 >> b[5] ? h0 >> b[5] : 1;
    }
    if (!ok || !blt_level(dst, b[11], &dmem, &dpitch, &dw, &dh) ||
        !blt_rect_ok(sr, sw, sh) || !blt_rect_ok(dr, dw, dh) || !sfmt) {
        blt_log(p, stretch ? "blt refused:" : "surfaceblt refused:", b, 13);
        if (p->dp2_errors < 8) {
            /* which half: the levels each side has, and the source's size */
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp:   levels src ", src ? src->levels : 0);
            dbg_hex(p, " dst ", dst ? dst->levels : 0);
            dbg_hex(p, " src ok ", ok);
            dbg_hex(p, " sw ", sw);
            dbg_hex(p, " sh ", sh);
            dbg_hex(p, " fmt ", sfmt);
            dbg_puts(p, "\n");
        }
        return;
    }
    blt_log(p, stretch ? "blt" : "surfaceblt", b, 13);
    cw = (ULONG)(dr[2] - dr[0]);
    ch = (ULONG)(dr[3] - dr[1]);
    if (!src->sysmem) d3d_readback(p, level_handle(src, b[0], b[5]));
    if (!dst->sysmem) d3d_readback(p, level_handle(dst, b[6], b[11]));
    dxt = fmt_is_dxt(sfmt);
    if (dxt) {
        ULONG block = fmt_row_bytes(sfmt, 4), rows, rowbytes;

        if (cw != (ULONG)(sr[2] - sr[0]) || ch != (ULONG)(sr[3] - sr[1])) {
            blt_log(p, "blt refused, a scaled DXT rectangle:", b, 13);
            return;
        }
        rows = (sr[1] + ch + 3) / 4 - sr[1] / 4;
        rowbytes = ((sr[0] + cw + 3) / 4 - sr[0] / 4) * block;
        smem += (sr[1] / 4) * spitch + (sr[0] / 4) * block;
        dmem += (dr[1] / 4) * dpitch + (dr[0] / 4) * block;
        for (y = 0; y < rows; y++) {
            memcpy((void *)(dmem + y * dpitch), (const void *)(smem + y * spitch), rowbytes);
        }
    } else if (cw == (ULONG)(sr[2] - sr[0]) && ch == (ULONG)(sr[3] - sr[1]) && sfmt == dfmt) {
        bpp = fmt_row_bytes(sfmt, 1);
        for (y = 0; y < ch; y++) {
            memcpy((void *)(dmem + (dr[1] + y) * dpitch + dr[0] * bpp),
                    (const void *)(smem + (sr[1] + y) * spitch + sr[0] * bpp), cw * bpp);
        }
    } else {
        ULONG scw = (ULONG)(sr[2] - sr[0]), sch = (ULONG)(sr[3] - sr[1]);

        bpp = fmt_row_bytes(sfmt, 1);
        dbpp = fmt_row_bytes(dfmt, 1);
        for (y = 0; y < ch; y++) {
            const UCHAR *srow = (const UCHAR *)(smem + (sr[1] + ((2 * y + 1) * sch) / (2 * ch)) * spitch);
            UCHAR *drow = (UCHAR *)(dmem + (dr[1] + y) * dpitch) + dr[0] * dbpp;
            for (x = 0; x < cw; x++) {
                px_copy(sfmt, dfmt, drow + x * dbpp, srow + (sr[0] + ((2 * x + 1) * scw) / (2 * cw)) * bpp, bpp);
            }
        }
    }
    if (!dst->sysmem) d3d_handle_op(p, D3DPT_OP_VRAM_DIRTY, level_handle(dst, b[6], b[11]));
}

/* The DX9 queries (M16): event and occlusion. The runtime names a query by
 * a per-context id (CREATEQUERY, DELETEQUERY) and ISSUEQUERY begins or ends
 * it. It learns a result from a response the driver writes at the start of
 * the command buffer, dwErrorOffset bytes of them on a successful call
 * (d3d9.dll's parser at 0x4fd75950: RESPONSEQUERY, the block's bytes, then
 * {id, size, data} per query). An occlusion query is the host's; ending one
 * waits for its count, which the host has the moment it has run the draws
 * before it (walk ends the record before an ISSUEQUERY, and every record
 * runs before the doorbell returns). An event is done by then too. */

static ULONG query_find(d3dpt_core *p, ULONG ctx, ULONG id)
{
    ULONG i;

    for (i = 0; i < D3D_MAX_QUERIES; i++) {
        if (p->queries[i].type && p->queries[i].ctx == ctx && p->queries[i].id == id) {
            return i;
        }
    }
    return ~0u;
}

static void query_release(d3dpt_core *p, ULONG i)
{
    if (p->queries[i].host) {
        d3d_handle_op(p, D3DPT_OP_RELEASE, p->queries[i].host);
    }
    p->queries[i].type = 0;
    p->queries[i].host = 0;
}

/* a context's queries, when the runtime destroys it (a process that never
 * deleted them) */
void query_forget_ctx(d3dpt_core *p, ULONG_PTR ctx)
{
    ULONG i;

    for (i = 0; i < D3D_MAX_QUERIES; i++) {
        if (p->queries[i].type && p->queries[i].ctx == (ULONG)ctx) {
            query_release(p, i);
        }
    }
}

/* CREATEQUERY: id, D3DQUERYTYPE */
static void walk_query_create(DP2WALK *w, const ULONG *b)
{
    d3dpt_core *p = w->p;
    ULONG i = query_find(p, (ULONG)w->ctx, b[0]), off, h = 0;
    d3dpt_create_query *a;

    if (i != ~0u) {
        query_release(p, i);                /* an id the runtime reuses */
    }
    for (i = 0; i < D3D_MAX_QUERIES && p->queries[i].type; i++) {
    }
    if (i == D3D_MAX_QUERIES || (b[1] != 8 && b[1] != 9)) {
        blt_log(p, "query refused:", b, 2);
        return;
    }
    if (b[1] == 9) {                        /* D3DQUERYTYPE_OCCLUSION: the host counts */
        h = 0x51000000u | (++p->query_host_next & 0xffffffu);
        off = d3dpt_enc_ret(&p->enc, 0);
        a = (d3dpt_create_query *)d3dpt_enc_cmd(&p->enc, D3DPT_OP_CREATE_QUERY, sizeof(*a), 0);
        if (!a) {
            return;
        }
        a->handle = h;
        a->ret_off = off;
        a->type = b[1];
        a->pad = 0;
        d3dpt_enc_flush(&p->enc);
        if (p->enc.last_status || d3dpt_enc_result(&p->enc, off)->hr != 0) {
            blt_log(p, "query refused by the host:", b, 2);
            return;
        }
    }
    p->queries[i].ctx = (ULONG)w->ctx;
    p->queries[i].id = b[0];
    p->queries[i].type = b[1];
    p->queries[i].host = h;
}

/* a response entry: the query's id and one dword of data */
static void resp_add(d3dpt_core *p, ULONG id, ULONG value)
{
    if (p->resp_len + 3 > D3D_RESP_DWORDS - 2) {
        blt_log(p, "query response dropped, the buffer is full:", &id, 1);
        return;
    }
    p->resp[2 + p->resp_len++] = id;
    p->resp[2 + p->resp_len++] = 4;
    p->resp[2 + p->resp_len++] = value;
    p->resp_n++;
}

/* ISSUEQUERY: id, flags (D3DISSUE_END 1, D3DISSUE_BEGIN 2) */
static void walk_query_issue(DP2WALK *w, const ULONG *b)
{
    d3dpt_core *p = w->p;
    ULONG i = query_find(p, (ULONG)w->ctx, b[0]), n, off;
    d3dpt_query_get *a;
    d3dpt_ret *r;

    if (i == ~0u) {
        blt_log(p, "issue of an unknown query:", b, 2);
        return;
    }
    blt_log(p, "issuequery", b, 2);
    if (p->queries[i].type == 8) {          /* an event: every command before it has run */
        if (b[1] & 1) resp_add(p, b[0], 1);
        return;
    }
    d3dpt_enc_u32x2(&p->enc, D3DPT_OP_QUERY_ISSUE, p->queries[i].host, b[1] & 3);
    if (!(b[1] & 1)) {
        return;
    }
    for (n = 0; n < 100000; n++) {
        off = d3dpt_enc_ret(&p->enc, 4);
        a = (d3dpt_query_get *)d3dpt_enc_cmd(&p->enc, D3DPT_OP_QUERY_GET_DATA, sizeof(*a), 0);
        if (!a) {
            return;
        }
        a->handle = p->queries[i].host;
        a->ret_off = off;
        a->flags = 1;                       /* D3DGETDATA_FLUSH */
        a->size = 4;
        d3dpt_enc_flush(&p->enc);
        r = d3dpt_enc_result(&p->enc, off);
        if (p->enc.last_status || (r->hr != 0 && r->hr != 1)) {
            break;
        }
        if (r->hr == 0) {
            ULONG v[3];
            v[0] = b[0];
            v[1] = *(const ULONG *)(r + 1);
            v[2] = n;
            blt_log(p, "occlusion query result (id, count, polls):", v, 3);
            resp_add(p, b[0], v[1]);
            return;
        }
    }
    blt_log(p, "occlusion query without a result, answered 0:", b, 2);
    resp_add(p, b[0], 0);
}

/* the body size of a token, ~0 when unknown or truncated; the IMM tokens'
 * payload is DWORD-aligned by offset (their size depends on where they sit) */
static ULONG walk_body_size(ULONG op, ULONG count, const UCHAR *q, ULONG left, ULONG pos, ULONG stride)
{
    ULONG pad = (4 - ((pos + 4) & 3)) & 3, n, i, sz;

    switch (op) {
    case 1: return count * 4;                                   /* POINTS */
    case 2: return count * 4;                                   /* INDEXEDLINELIST */
    case 3: return count * 8;                                   /* INDEXEDTRIANGLELIST */
    case 8: return count * 8;                                   /* RENDERSTATE */
    case 15: case 16: case 18: case 19: case 21: return 2;      /* LINELIST .. TRIANGLEFAN: wVStart */
    case 17: return 2 + (count + 1) * 2;                        /* INDEXEDLINESTRIP */
    case 20: case 22: return 2 + (count + 2) * 2;               /* INDEXEDTRIANGLESTRIP / FAN */
    case 23: return pad + 4 + (count + 2) * stride;             /* TRIANGLEFAN_IMM (+ the pad after) */
    case 24: return pad + count * 2 * stride;                   /* LINELIST_IMM */
    case 25: return count * 8;                                  /* TEXTURESTAGESTATE */
    case 26: return 2 + count * 6;                              /* INDEXEDTRIANGLELIST2 */
    case 27: return 2 + count * 4;                              /* INDEXEDLINELIST2 */
    case 28: return count * 16;                                 /* VIEWPORTINFO */
    case 29: return count * 8;                                  /* WINFO */
    case 30: return count * 12;                                 /* SETPALETTE */
    case 31: return left < 8 ? ~0u : 8 + (ULONG)((const USHORT *)q)[3] * 4;   /* UPDATEPALETTE */
    case 32: return count * 8;                                  /* ZRANGE */
    case 33: return count * 68;                                 /* SETMATERIAL */
    case 34:                                                    /* SETLIGHT: 8, + 104 with data */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            n = ((const ULONG *)(q + sz))[1] == 2 ? 112 : 8;
            sz += n;
        }
        return sz;
    case 35: return count * 4;                                  /* CREATELIGHT */
    case 36: return count * 68;                                 /* SETTRANSFORM */
    case 38: return count * 36;                                 /* TEXBLT */
    case 39: return count * 12;                                 /* STATESET */
    case 40: return count * 8;                                  /* SETPRIORITY */
    case 41: return count * 8;                                  /* SETRENDERTARGET */
    case 42: return 16 + count * 16;                            /* CLEAR */
    case 43: return count * 8;                                  /* SETTEXLOD */
    case 44: return count * 20;                                 /* SETCLIPPLANE */
    case 45:                                                    /* CREATEVERTEXSHADER: handle, decl size, code size */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 12) return ~0u;
            sz += 12 + ((const ULONG *)(q + sz))[1] + ((const ULONG *)(q + sz))[2];
        }
        return sz;
    case 46: case 47: case 55: case 56: return count * 4;       /* shader handles */
    case 48: case 57:                                           /* shader constants: register, count, count * 16 */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            sz += 8 + ((const ULONG *)(q + sz))[1] * 16;
        }
        return sz;
    case 49: return count * 12;                                 /* SETSTREAMSOURCE */
    case 50: return count * 8;                                  /* SETSTREAMSOURCEUM */
    case 51: return count * 8;                                  /* SETINDICES */
    case 52: return count * 12;                                 /* DRAWPRIMITIVE */
    case 53: return count * 24;                                 /* DRAWINDEXEDPRIMITIVE */
    case 54:                                                    /* CREATEPIXELSHADER: handle, code size */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            sz += 8 + ((const ULONG *)(q + sz))[1];
        }
        return sz;
    case 58: return count * 12;                                 /* CLIPPEDTRIANGLEFAN */
    case 59: return count * 12;                                 /* DRAWPRIMITIVE2 */
    case 60: return count * 24;                                 /* DRAWINDEXEDPRIMITIVE2 */
    case 61: case 62:                                           /* patches: handle, flags [, segments] [, info] */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            n = ((const ULONG *)(q + sz))[1];
            sz += 8 + ((n & 1) ? (op == 61 ? 16 : 12) : 0) + ((n & 2) ? (op == 61 ? 28 : 16) : 0);
        }
        return sz;
    case 63: return count * 48;                                 /* VOLUMEBLT */
    case 64: return count * 24;                                 /* BUFFERBLT: dst, src, dst offset, D3DRANGE (offset, size), one more dword (see walk_bufferblt) */
    case 65: return count * 68;                                 /* MULTIPLYTRANSFORM */
    case 66: return count * 20;                                 /* ADDDIRTYRECT */
    case 67: return count * 28;                                 /* ADDDIRTYBOX */
    /* DX9 (M16; d3dhal.h of the DX9 DDK, Microsoft's DDI reference) */
    case 71:                                                    /* CREATEVERTEXSHADERDECL: handle, n, n D3DVERTEXELEMENT9 */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            n = ((const ULONG *)(q + sz))[1];
            if (n > 64) return ~0u;
            sz += 8 + n * 8;
        }
        return sz;
    case 72: case 73: case 75: case 76: return count * 4;       /* DELETE / SET VERTEXSHADERDECL, DELETE / SET ...FUNC */
    case 74:                                                    /* CREATEVERTEXSHADERFUNC: handle, code size, code */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            sz += 8 + ((const ULONG *)(q + sz))[1];
            if (sz > left) return ~0u;
        }
        return sz;
    case 77: case 93:                                           /* SET{VERTEX,PIXEL}SHADERCONSTI: register, count, count * 4 ints */
    case 83: case 94:                                           /* ...CONSTB: register, count, count BOOLs */
        sz = 0;
        for (i = 0; i < count; i++) {
            if (left < sz + 8) return ~0u;
            n = ((const ULONG *)(q + sz))[1];
            if (n > 256) return ~0u;
            sz += 8 + n * (op == 77 || op == 93 ? 16 : 4);
        }
        return sz;
    case 79: return count * 16;                                 /* SETSCISSORRECT: a RECT */
    case 80: return count * 16;                                 /* SETSTREAMSOURCE2: stream, handle, offset, stride */
    case 81: case 96: return count * 52;                        /* BLT / SURFACEBLT: src, RECTL, level, dst, RECTL, level, flags */
    case 82: return count * 24;                                 /* COLORFILL: surface, RECTL, colour */
    case 84: return count * 8;                                  /* CREATEQUERY: id, type */
    case 85: return count * 8;                                  /* SETRENDERTARGET2: index, target */
    case 86: return count * 4;                                  /* SETDEPTHSTENCIL: z buffer */
    case 89: return count * 8;                                  /* GENERATEMIPSUBLEVELS: surface, filter */
    case 90: return count * 4;                                  /* DELETEQUERY: id */
    case 91: return count * 8;                                  /* ISSUEQUERY: id, flags */
    case 95: return count * 8;                                  /* SETSTREAMSOURCEFREQ: stream, divider */
    default: return ~0u;
    }
}

/* one pass over the runtime's stream; FALSE = an unknown token stopped it
 * there (the rest is copied verbatim for the host to report) */
static BOOL walk(DP2WALK *w)
{
    ULONG pos = w->start, i;

    while (pos + 4 <= w->clen) {
        const D3DHAL_DP2COMMAND_ *c = (const D3DHAL_DP2COMMAND_ *)(w->cmd + pos);
        const UCHAR *q = w->cmd + pos + 4;
        ULONG op = c->bCommand, count = c->wPrimitiveCount, left = w->clen - pos - 4, size;
        ULONG stride = w->vsize ? w->vsize : (w->fvf ? fvf_stride(w->fvf) : 0);

        if (w->eb) {
            /* A DX3 execute buffer (IDirect3DDevice::Execute, d3dim.dll's
             * UNCLIPPED path): the stream is the buffer's own D3DINSTRUCTION
             * list from the current instruction on, and the runtime is a
             * pass-through that executes nothing here itself. The opcodes
             * share the DP2 numbering where the payloads match (POINT /
             * LINE / TRIANGLE / STATERENDER are 1 / 2 / 3 / 8: POINTS,
             * INDEXEDLINELIST, the 8-byte INDEXEDTRIANGLELIST, RENDERSTATE),
             * and those the driver must consume, along with SPAN (13,
             * skipped) and EXIT (11, the end). Everything else is the
             * runtime's: PROCESSVERTICES (9) first of all, the matrix / light
             * opcodes 4..7, TEXTURELOAD, BRANCHFORWARD, SETSTATUS. The call
             * ends before it with D3DERR_COMMAND_UNPARSED and its
             * offset in dwErrorOffset, the runtime executes it (a
             * PROCESSVERTICES COPY / TRANSFORM fills the TL vertex buffer
             * we draw from) and calls again from the next instruction.
             * Skipping it instead leaves the vertices unwritten (doc 15
             * "Execute buffers"). */
            if (op == 11) {                                     /* EXIT */
                w->stop = pos;
                return TRUE;
            }
            if (op == 13) {                                     /* SPAN: bSize x count, nothing for the host */
                size = (ULONG)c->bReserved * count;
                if (size > left) size = left;
                pos += 4 + size;
                continue;
            }
            if (op != 1 && op != 2 && op != 3 && op != 8) {
                w->bounce = pos;
                w->stop = pos;
                if (!w->out && w->p->parse_lines < 8) {
                    w->p->parse_lines++;
                    dbg_hex(w->p, "d3dptdisp: execute buffer: opcode ", op);
                    dbg_hex(w->p, " x", count);
                    dbg_hex(w->p, " bounced to the runtime at ", pos);
                    dbg_puts(w->p, "\n");
                }
                return TRUE;
            }
        }
        size = walk_body_size(op, count, q, left, pos, stride);
        if (size == ~0u && w->p->parse_unknown && !(ddflags(w->p) & DDF_NO_PARSEUNKNOWN_CALL)) {
            /* not one of ours and not a legacy opcode: the runtime's parser
             * gets a chance (it knows VIEWPORTINFO / WINFO, which the host
             * handles anyway); the token is dropped from the host's stream */
            PVOID next = NULL;
            HRESULT hr = w->p->parse_unknown((PVOID)c, &next);
            if (hr == DD_OK && next && (const UCHAR *)next > q && (const UCHAR *)next <= w->cmd + w->clen) {
                size = (ULONG)((const UCHAR *)next - q);
                if (!w->out && w->p->parse_lines < 8) {
                    w->p->parse_lines++;
                    dbg_hex(w->p, "d3dptdisp: dp2 token ", op);
                    dbg_hex(w->p, " x", count);
                    dbg_hex(w->p, " parsed by the runtime, bytes ", size);
                    dbg_puts(w->p, "\n");
                }
                pos += 4 + size;
                continue;
            }
            if (!w->out && w->p->parse_lines < 8) {
                w->p->parse_lines++;
                dbg_hex(w->p, "d3dptdisp: dp2 token ", op);
                dbg_hex(w->p, " unparsed by the runtime too, hr ", (ULONG)hr);
                dbg_puts(w->p, "\n");
            }
        }
        if (size == ~0u || size > left) {
            walk_put(w, w->cmd + pos, w->clen - pos);
            return FALSE;
        }
        if (op == 23 || op == 24) {                             /* the next token is DWORD-aligned by offset */
            ULONG e = (pos + 4 + size + 3) & ~3u;
            size = e - pos - 4 <= left ? e - pos - 4 : left;
        }
        /* A blit after a draw ends the record here. The guest does the copy
         * now, while the host reads a buffer or a texture when it runs the
         * draw, so in one record every draw would see the last blit's bytes,
         * including draws issued before that blit. A managed vertex buffer
         * locked, drawn, locked again and drawn again arrives exactly so,
         * BUFFERBLT DRAW BUFFERBLT DRAW in one call, and both draws would get
         * the second fill. dp2_run sends what came before and goes on from
         * here; the blit then starts the next record. */
        if ((op == 38 || op == 63 || op == 64) && w->drawn && w->can_split) {
            w->split = TRUE;
            w->stop = pos;
            return TRUE;
        }
        /* The DX9 blits read and write render targets as well, which a
         * clear or a target switch changes as much as a draw does, and an
         * ISSUEQUERY must fall between the draws it counts and the ones it
         * does not: anything before one in the record goes to the host first */
        if ((op == 81 || op == 82 || op == 91 || op == 96) && w->outlen && pos > w->start && w->can_split) {
            w->split = TRUE;
            w->stop = pos;
            return TRUE;
        }
        switch (op) {
        case 8:                                                 /* RENDERSTATE: mirrored for the runtime */
            if (w->rstates && !w->out) {
                for (i = 0; i < count; i++) {
                    const ULONG *e = (const ULONG *)(q + i * 8);
                    if (e[0] < 256) w->rstates[e[0]] = e[1];
                }
            }
            walk_put(w, c, 4 + size);
            break;
        case 23: case 24: {                                     /* IMM: re-pad for the output offset */
            ULONG ipad = (4 - ((pos + 4) & 3)) & 3;
            walk_put(w, c, 4);
            walk_pad(w);
            walk_put(w, q + ipad, size - ipad);
            walk_pad(w);
            w->needs_vb = TRUE;
            w->drawn = TRUE;
            break;
        }
        case 1: case 2: case 3: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 22:
        case 26: case 27:
            w->needs_vb = TRUE;
            w->drawn = TRUE;
            walk_put(w, c, 4 + size);
            break;
        case 25: {                                              /* TEXTURESTAGESTATE: a bound texture's colour key, in pass 1 */
            ULONG o = w->outlen;
            if (!w->out) {
                for (i = 0; i < count; i++) {
                    const USHORT *e = (const USHORT *)(q + i * 8);
                    if (e[1] == 0 && ((const ULONG *)(q + i * 8))[1] != 0) {
                        surf_colorkey_check(w->p, ((const ULONG *)(q + i * 8))[1]);
                    }
                }
            }
            walk_put(w, c, 4 + size);
            if (w->out && w->dx8_filters) {
                for (i = 0; i < count; i++) {
                    UCHAR *e = w->out + o + 4 + i * 8;
                    ((ULONG *)e)[1] = tss_dx8_filter(((USHORT *)e)[1], ((ULONG *)e)[1]);
                }
            }
            break;
        }
        case 38:                                                /* TEXBLT: done here, in pass 1 */
            if (!w->out) {
                for (i = 0; i < count; i++) walk_texblt(w, (const ULONG *)(q + i * 36));
            }
            break;
        case 47:                                                /* SETVERTEXSHADER: an FVF, or a shader handle (bit 0) */
            for (i = 0; i < count; i++) {
                ULONG h = ((const ULONG *)q)[i];
                w->fvf = h;
                w->shader = (h & 1) != 0;
            }
            walk_put(w, c, 4 + size);
            break;
        case 49:                                                /* SETSTREAMSOURCE: stream, handle, stride */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                if (e[0] < D3D_MAX_STREAMS) {
                    stream_bind(&w->st[e[0]], e[1], e[2]);
                    w->st_um &= ~(1u << e[0]);
                }
            }
            break;
        case 50:                                                /* SETSTREAMSOURCEUM: stream, stride (the DP2 vertex buffer) */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 8);
                if (e[0] < D3D_MAX_STREAMS) {
                    stream_bind_um(w, e[0], e[1]);
                }
            }
            break;
        case 51:                                                /* SETINDICES: handle, stride */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 8);
                stream_bind(&w->ib, e[0], e[1]);
            }
            break;
        case 52:                                                /* DRAWPRIMITIVE: type, VStart, count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                walk_draw(w, e[0], e[2], &w->st[0], e[1] * w->st[0].stride, 0, 0, 0, 0);
            }
            break;
        case 59:                                                /* DRAWPRIMITIVE2: type, first vertex offset (bytes into stream 0), count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                walk_draw(w, e[0], e[2], &w->st[0], e[1], 0, 0, 0, 0);
            }
            break;
        case 53:                                                /* DRAWINDEXEDPRIMITIVE: type, base, min, nverts, start index, count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 24);
                walk_draw(w, e[0], e[5], &w->st[0], (e[1] + e[2]) * w->st[0].stride, e[3], e[4] * 2, prim_verts(e[0], e[5]), e[2]);
            }
            break;
        case 60:                                                /* DRAWINDEXEDPRIMITIVE2: type, base offset, min, nverts, start offset, count */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 24);
                walk_draw(w, e[0], e[5], &w->st[0], (ULONG)((LONG)e[1] + (LONG)(e[2] * w->st[0].stride)), e[3], e[4], prim_verts(e[0], e[5]), e[2]);
            }
            break;
        case 58:                                                /* CLIPPEDTRIANGLEFAN: first vertex offset, edge flags, count */
            /* the DX8 runtime clips pre-transformed triangles itself into
             * a vertex buffer of its own and binds that buffer as stream 0
             * (SETSTREAMSOURCE, its stride) before these tokens: the offset
             * is a byte offset into stream 0, not into the DP2 vertex
             * buffer (which is a dummy under d3d8.dll; doc 15) */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 12);
                walk_draw(w, 6, e[2], &w->st[0], e[0], 0, 0, 0, 0);
            }
            break;
        case 64:                                                /* BUFFERBLT: done here, in pass 1 (v9) */
            if (!w->out) {
                for (i = 0; i < count; i++) walk_bufferblt(w, (const ULONG *)(q + i * 24));
            }
            break;
        case 63:                                                /* VOLUMEBLT: done here, in pass 1 (v12) */
            if (!w->out) {
                for (i = 0; i < count; i++) walk_volumeblt(w, (const ULONG *)(q + i * 48));
            }
            break;
        case 41:                                                /* SETRENDERTARGET: render target, Z buffer */
            /* passed through, and remembered: EndScene reads back the
             * context's target, which was otherwise still the one from
             * ContextCreate for a DX8 application rendering to a texture */
            if (count) {
                const ULONG *e = (const ULONG *)(q + (count - 1) * 8);
                w->rt = e[0];
                w->z = e[1];
                w->rt_set = TRUE;
            }
            walk_put(w, c, 4 + size);
            break;
        case 61: case 62: case 66: case 67:                    /* patches, dirty rects */
            break;
        case 73:                                                /* SETVERTEXSHADERDECL (DX9): an FVF, or a declaration handle (bit 0) */
            /* the same namespace as DX8's SETVERTEXSHADER: the draws carry
             * it (DRAW8's fvf); the declaration itself went to the host with
             * CREATEVERTEXSHADERDECL, the shader code with ...FUNC */
            for (i = 0; i < count; i++) {
                ULONG h = ((const ULONG *)q)[i];
                w->fvf = h;
                w->shader = (h & 1) != 0;
            }
            break;
        case 80:                                                /* SETSTREAMSOURCE2 (DX9): stream, handle, offset, stride */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 16);
                if (e[0] < D3D_MAX_STREAMS) {
                    stream_bind_off(&w->st[e[0]], e[1], e[2], e[3]);
                    w->st_um &= ~(1u << e[0]);
                }
            }
            break;
        case 85: case 86: {                                     /* SETRENDERTARGET2 (index, target) / SETDEPTHSTENCIL (z) */
            /* DX9 sets the two apart; the host takes DX7's pair, so each
             * becomes a SETRENDERTARGET with the other half as it stands.
             * Targets 1..3 (multiple render targets, v17) go to the host
             * as the runtime's own token, one entry each */
            D3DHAL_DP2COMMAND_ h;
            ULONG pair[2];
            BOOL any = FALSE;

            for (i = 0; i < count; i++) {
                if (op == 86) {
                    w->z = ((const ULONG *)q)[i];
                    any = TRUE;
                } else if (((const ULONG *)(q + i * 8))[0] == 0) {
                    w->rt = ((const ULONG *)(q + i * 8))[1];
                    any = TRUE;
                } else if (((const ULONG *)(q + i * 8))[0] < 4) {
                    h.bCommand = 85;
                    h.bReserved = 0;
                    h.wPrimitiveCount = 1;
                    walk_put(w, &h, sizeof(h));
                    walk_put(w, q + i * 8, 8);
                }
            }
            if (any) {
                w->rt_set = TRUE;
                h.bCommand = 41;
                h.bReserved = 0;
                h.wPrimitiveCount = 1;
                pair[0] = w->rt;
                pair[1] = w->z;
                walk_put(w, &h, sizeof(h));
                walk_put(w, pair, sizeof(pair));
            }
            break;
        }
        case 81: case 96:                                       /* BLT / SURFACEBLT: done here, in pass 1 (M16) */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 52);
                if (op == 81 && blt_depth(e)) {
                    /* between two video-memory depth buffers (v18): the
                     * host's, whose depth never comes back to VRAM */
                    D3DHAL_DP2COMMAND_ h;
                    h.bCommand = 81;
                    h.bReserved = 0;
                    h.wPrimitiveCount = 1;
                    walk_put(w, &h, sizeof(h));
                    walk_put(w, e, 52);
                } else if (!w->out) {
                    walk_blt9(w, e, op == 81);
                }
            }
            break;
        case 82:                                                /* COLORFILL: done here, in pass 1 (M16) */
            if (!w->out) {
                for (i = 0; i < count; i++) walk_colorfill(w, (const ULONG *)(q + i * 24));
            }
            break;
        case 84:                                                /* CREATEQUERY: id, type (M16) */
            if (!w->out) {
                for (i = 0; i < count; i++) walk_query_create(w, (const ULONG *)(q + i * 8));
            }
            break;
        case 90:                                                /* DELETEQUERY: id (M16) */
            if (!w->out) {
                for (i = 0; i < count; i++) {
                    ULONG k = query_find(w->p, (ULONG)w->ctx, ((const ULONG *)q)[i]);
                    if (k != ~0u) query_release(w->p, k);
                }
            }
            break;
        case 91:                                                /* ISSUEQUERY: id, flags (M16) */
            if (!w->out) {
                for (i = 0; i < count; i++) walk_query_issue(w, (const ULONG *)(q + i * 8));
            }
            break;
        case 95:                                                /* SETSTREAMSOURCEFREQ: stream, divider (v16) */
            for (i = 0; i < count; i++) {
                const ULONG *e = (const ULONG *)(q + i * 8);
                if (e[0] < D3D_MAX_STREAMS) {
                    w->freq[e[0]] = e[1];
                }
            }
            break;
        default:                                                /* the DX7 state tokens and the shader tokens (45, 46, 48, 54..57) */
            walk_put(w, c, 4 + size);
            break;
        }
        pos += 4 + size;
    }
    if (pos < w->clen) {
        walk_put(w, w->cmd + pos, w->clen - pos);
    }
    w->stop = w->clen;
    return TRUE;
}


/* One record of a DrawPrimitives2 call: the stream from start to its end or
 * to the next split (a blit after a draw, see walk). The first pass
 * measures the output and does the blits, the second writes into the
 * record, then the record goes into the command window and the doorbell
 * rings. The doorbell runs it on the host before this returns, so the next record's
 * blits land after this one's draws have read what they read. Returns where
 * the next record starts, or ~0 when the stream is done or this one failed. */
static ULONG dp2_record(d3dpt_core *p, D3DCTX *c, const d3dpt_dp2_call *call, ULONG start, d3dpt_dp2_result *out)
{
    ULONG vcopy, off, i, next;
    d3dpt_dp2 *r;
    d3dpt_ret *res;
    DP2WALK w, w0;

    out->hr = DDERR_GENERIC;
    out->offset = 0;
    out->bounce = FALSE;
    /* pass 1: the output size, the render-state mirror, the TEXBLTs; both
     * passes start from the context's DX8 state */
    memset(&w0, 0, sizeof(w0));
    w0.p = p;
    w0.start = start;
    w0.ctx = call->ctx;
    w0.can_split = !call->eb;
    w0.cmd = call->cmd;
    w0.clen = call->clen;
    w0.vtx = call->vtx;
    w0.vlen = call->vlen;
    w0.vsize = call->vsize;
    w0.vcount = call->vcount;
    w0.vall = call->vall;
    w0.eb = call->eb;
    w0.bounce = ~0u;
    w0.fvf = c->fvf ? c->fvf : (call->vertex_type & 1 ? 0 : call->vertex_type);
    w0.shader = c->shader;
    w0.one_stream = (ddflags(p) & DDF_ONE_STREAM) != 0;
    w0.dx8_filters = c->iface >= 4;
    w0.rt = c->rt;
    w0.z = c->z;
    for (i = 0; i < D3D_MAX_STREAMS; i++) {
        w0.freq[i] = c->st_freq[i];
    }
    for (i = 0; i < D3D_MAX_STREAMS; i++) {
        if (c->st_um & (1u << i)) {
            stream_bind_um(&w0, i, c->st_stride[i]);
        } else {
            stream_bind_off(&w0.st[i], c->st_handle[i], c->st_off[i], c->st_stride[i]);
        }
    }
    stream_bind(&w0.ib, c->ib_handle, c->ib_stride);
    w = w0;
    w.rstates = call->rstates;
    walk(&w);
    if (w.eb && w.bounce == 0) {
        /* nothing of ours before the runtime's instruction: bounce at once */
        out->hr = D3DERR_COMMAND_UNPARSED_;
        out->bounce = TRUE;
        out->offset = 0;
        return ~0u;
    }
    next = w.split ? w.stop : ~0u;
    vcopy = w.needs_vb ? call->vlen : 0;
    if (w.outlen > (48u << 20) || D3DPT_ALIGN8(w.outlen) + vcopy + sizeof(*r) + sizeof(d3dpt_cmd) > D3DPT_CMD_SIZE) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dp2 refused, output bytes ", w.outlen);
            dbg_hex(p, " vertex copy ", vcopy);
            dbg_puts(p, "\n");
        }
        return ~0u;
    }
    p->dp2_calls++;
    off = d3dpt_enc_ret(&p->enc, 0);
    r = d3dpt_enc_cmd(&p->enc, D3DPT_OP_DP2, sizeof(*r), D3DPT_ALIGN8(w.outlen) + vcopy);
    if (!r) {
        return ~0u;
    }
    /* pass 2: the same walk, writing into the record; its end state is the
     * context's for the next call */
    {
        ULONG outlen = w.outlen, skipped = w.skipped;
        w = w0;
        w.out = (UCHAR *)(r + 1);
        walk(&w);
        if (w.outlen != outlen) {           /* cannot happen: the same stream twice */
            w.outlen = outlen;
        }
        c->fvf = w.fvf;
        c->shader = w.shader;
        c->st_um = w.st_um;
        for (i = 0; i < D3D_MAX_STREAMS; i++) {
            c->st_handle[i] = w.st[i].handle;
            c->st_stride[i] = w.st[i].stride;
            c->st_off[i] = w.st[i].off;
            c->st_freq[i] = w.freq[i];
        }
        c->ib_handle = w.ib.handle;
        c->ib_stride = w.ib.stride;
        if (w.rt_set) {
            c->rt = w.rt;
            c->z = w.z;
        }
        if (skipped && p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dx8 draws skipped ", skipped);
            dbg_hex(p, " why ", w.skip_why);
            dbg_hex(p, " fvf ", w0.fvf);
            dbg_hex(p, " stride ", w0.st[0].stride);
            dbg_hex(p, " bytes ", w0.st[0].bytes);
            dbg_hex(p, " ib ", w0.ib.bytes);
            dbg_hex(p, "; first: prim ", w.skip_info[0]);
            dbg_hex(p, " count ", w.skip_info[1]);
            dbg_hex(p, " voff ", w.skip_info[2]);
            dbg_hex(p, " nverts ", w.skip_info[3]);
            dbg_hex(p, " ioff ", w.skip_info[4]);
            dbg_hex(p, " nindices ", w.skip_info[5]);
            dbg_puts(p, "\n");
        }
    }
    r->ctx = (ULONG)call->ctx;
    r->ret_off = off;
    r->flags = call->flags;
    r->fvf = call->vertex_type;
    r->vertex_stride = call->vsize;
    r->command_bytes = w.outlen;
    r->vertex_bytes = vcopy;
    r->pad = 0;
    if (vcopy) {
        memcpy((UCHAR *)(r + 1) + D3DPT_ALIGN8(w.outlen), call->vtx, vcopy);
    }
    d3dpt_enc_flush(&p->enc);
    res = d3dpt_enc_result(&p->enc, off);
    if (p->enc.last_status) {
        out->hr = DDERR_GENERIC;
        out->offset = 0;
    } else if (w.eb && w.bounce != ~0u && res->hr == DD_OK) {
        /* the host drew what came before it; the runtime takes over at
         * this instruction and calls again from the next one */
        out->hr = D3DERR_COMMAND_UNPARSED_;
        out->bounce = TRUE;
        out->offset = w.bounce;
    } else {
        out->hr = (HRESULT)res->hr;
        out->offset = res->bytes;
    }
    if (out->hr != DD_OK && !out->bounce && p->dp2_errors < 8) {
        p->dp2_errors++;
        dbg_hex(p, "d3dptdisp: dp2 ", (ULONG)out->hr);
        dbg_hex(p, " at ", out->offset);
        dbg_hex(p, " len ", call->clen);
        dbg_hex(p, " fvf ", call->vertex_type);
        dbg_puts(p, "\n");
    }
    return out->hr == DD_OK ? next : ~0u;
}

/* One DrawPrimitives2 call: one record, or one per stretch between a draw
 * and a blit after it (dp2_record). The layer has already resolved where the
 * buffers are and which context this is; what comes back is what it must
 * tell the runtime. */
void dp2_run(d3dpt_core *p, const d3dpt_dp2_call *call, d3dpt_dp2_result *out)
{
    D3DCTX *c = p ? ctx_of(p, call->ctx) : NULL;
    ULONG start = 0;

    out->hr = DDERR_GENERIC;
    out->offset = 0;
    out->bounce = FALSE;
    out->resp_bytes = 0;
    if (!c) {
        if (p && p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dp2 refused, context ", (ULONG)call->ctx);
            dbg_puts(p, "\n");
        }
        return;
    }
    if (call->clen > (16u << 20) || call->vlen > (32u << 20)) {
        if (p->dp2_errors < 8) {
            p->dp2_errors++;
            dbg_hex(p, "d3dptdisp: dp2 refused, command bytes ", call->clen);
            dbg_hex(p, " vertex bytes ", call->vlen);
            dbg_puts(p, "\n");
        }
        return;
    }
    p->resp_n = p->resp_len = 0;
    do {
        start = dp2_record(p, c, call, start, out);
    } while (start != ~0u);
    /* the query responses, now that nothing more is read from the buffer
     * they overwrite */
    if (p->resp_n) {
        ULONG bytes = (2 + p->resp_len) * 4;

        if (call->resp && bytes <= call->resp_max && out->hr == DD_OK) {
            p->resp[0] = 88u | (p->resp_n << 16);  /* D3DDP2OP_RESPONSEQUERY, wPrimitiveCount entries */
            p->resp[1] = bytes;                     /* the block's bytes, header included */
            memcpy(call->resp, p->resp, bytes);
            out->resp_bytes = bytes;
            blt_log(p, "query responses (bytes, entries):", p->resp + 1, 1);
        }
        p->resp_n = p->resp_len = 0;
    }
}
