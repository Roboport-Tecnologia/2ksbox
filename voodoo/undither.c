/*
 * undither.c: the Voodoo's ordered dither, undone at scanout (doc 21 §12).
 *
 * The chip renders colour at more than 16 bits and stores it as RGB565
 * through an ordered dither; 3dfx's RAMDAC then put a box filter on the
 * scanout that partly undid it, which is where the "22-bit" of the marketing
 * came from. 86Box has leilei's approximation of that filter (`scrfilter`,
 * our `filter=on`), and on a Voodoo 2 it is a *single scanline* pass.
 * voodoo_filterline_v2() takes one row pointer and ignores its `line`
 * argument, so it softens the dither along a line, leaves the vertical half
 * of every pattern, and decides what is dither with a threshold the guest has
 * to program into maxRgbDelta first.
 *
 * This goes the other way round, because in an emulator the dither matrix is
 * not something to approximate: it is the table the rasterizer dithered
 * *with*. 86box/vid_voodoo_dither.h holds it, vid_voodoo_render.c:1329
 * applies it indexed by (real_y & 3, x & 3), and for a linear, non-SLI buffer
 * the row that was dithered as row r is scanned out as row r, so at scanout
 * the phase is just (y & 3, x & 3). Inverting the table gives, for each phase
 * and each stored code, the interval of 8-bit values that dither to it.
 *
 * Pixels that came from one pre-dither colour therefore carry intervals with
 * a common member, and the intersection is what that colour can have been.
 * Two window sizes, in that order:
 *
 *   4x4, centred.  The interval comes out exactly one value wide for every
 *                  value and every phase of both 4x4 tables, so the output is
 *                  the colour the rasterizer had, exactly, and (what a 2x2
 *                  cannot do) the *same* value at all sixteen phases. A
 *                  2x2 alone is within 2/255 but its midpoint moves with the
 *                  phase: measured, 255 of 256 flat colours come back out of
 *                  a 2x2 with more than one level in them, which is a
 *                  residual pattern where there should be none.
 *   2x2, anchored. The fallback where a 4x4 holds more than one colour (an
 *                  edge, a steep gradient). Within 2/255; the four tables
 *                  measure 2 for dither_rb, 1 for dither_g, 1 for both 2x2
 *                  ones. The 4x4 is the intersection of four of these, so it
 *                  costs four lookups on top of a row of them, not sixteen.
 *
 * An empty intersection at both sizes is the proof of the opposite: no single
 * colour could have dithered into those pixels, so the window straddles an
 * edge, and the pixel is written exactly as the unfiltered path would have
 * written it. That is why this needs no threshold and never blurs across an
 * edge. An edge here is not "a difference bigger than N", it is an
 * arithmetic impossibility.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <wchar.h>
#include <math.h>
#define HAVE_STDARG_H
#include <86box/86box.h>
#include "cpu.h"
#include <86box/device.h>
#include <86box/mem.h>
#include <86box/timer.h>
#include <86box/plat.h>
#include <86box/thread.h>
#include <86box/video.h>
#include <86box/vid_svga.h>
#include <86box/vid_voodoo_common.h>
#include <86box/vid_voodoo_regs.h>
#include <86box/vid_voodoo_dither.h>
#include "undither.h"

/* what vid_voodoo_display.c asserts of h_disp, and the shim bitmap's width */
#define UNDITHER_MAX_W 4096

typedef uint8_t row_t[3][UNDITHER_MAX_W]; /* one scanline, 0 = b, 1 = g, 2 = r */

/* (lo << 8) | hi: the 8-bit values that dither to this code at this phase.
 * lo > hi means no value does, a code the table never produces. */
static uint16_t inv_rb4[4][4][32];
static uint16_t inv_g4[4][4][64];
static uint16_t inv_rb2[2][2][32];
static uint16_t inv_g2[2][2][64];
static int      inv_built;

static void
invert_table(const uint8_t *tab, int phases_y, int phases_x, int ncodes, uint16_t *out)
{
    for (int i = 0; i < phases_y * phases_x * ncodes; i++) {
        out[i] = 0xff00; /* lo = 255, hi = 0: empty until something lands in it */
    }
    for (int v = 0; v < 256; v++) {
        for (int y = 0; y < phases_y; y++) {
            for (int x = 0; x < phases_x; x++) {
                int       code = tab[(v * phases_y + y) * phases_x + x];
                uint16_t *e    = &out[(y * phases_x + x) * ncodes + code];
                int       lo   = *e >> 8;
                int       hi   = *e & 0xff;

                if (v < lo) {
                    lo = v;
                }
                if (v > hi) {
                    hi = v;
                }
                *e = (uint16_t) ((lo << 8) | hi);
            }
        }
    }
}

static void
build_tables(void)
{
    if (inv_built) {
        return;
    }
    invert_table(&dither_rb[0][0][0], 4, 4, 32, &inv_rb4[0][0][0]);
    invert_table(&dither_g[0][0][0], 4, 4, 64, &inv_g4[0][0][0]);
    invert_table(&dither_rb2x2[0][0][0], 2, 2, 32, &inv_rb2[0][0][0]);
    invert_table(&dither_g2x2[0][0][0], 2, 2, 64, &inv_g2[0][0][0]);
    inv_built = 1;
}

/* one scanline's (lo, hi) per channel */
static void
decode_row(const uint16_t *src, int w, int y, int two, row_t lo, row_t hi)
{
    const uint16_t *trb   = two ? &inv_rb2[y & 1][0][0] : &inv_rb4[y & 3][0][0];
    const uint16_t *tg    = two ? &inv_g2[y & 1][0][0] : &inv_g4[y & 3][0][0];
    const int       pmask = two ? 1 : 3;

    for (int x = 0; x < w; x++) {
        uint16_t p  = src[x];
        int      ph = x & pmask;
        uint16_t b  = trb[ph * 32 + (p & 31)];
        uint16_t g  = tg[ph * 64 + ((p >> 5) & 63)];
        uint16_t r  = trb[ph * 32 + ((p >> 11) & 31)];

        lo[0][x] = (uint8_t) (b >> 8);
        hi[0][x] = (uint8_t) (b & 0xff);
        lo[1][x] = (uint8_t) (g >> 8);
        hi[1][x] = (uint8_t) (g & 0xff);
        lo[2][x] = (uint8_t) (r >> 8);
        hi[2][x] = (uint8_t) (r & 0xff);
    }
}

/* the 2x2 windows anchored along one row: rows y and y + 1, columns x and
 * x + 1. lo > hi in the result means that 2x2 holds no single colour.
 *
 * Plane at a time with no clamp in the loop (the last column is fixed up
 * afterwards), because this is where the frame's time goes and a byte-wide
 * max/min over contiguous arrays is what the vectorizer wants. */
static void
quad_row(const row_t alo, const row_t ahi, const row_t blo, const row_t bhi,
         int w, row_t qlo, row_t qhi)
{
    for (int c = 0; c < 3; c++) {
        const uint8_t *al = alo[c];
        const uint8_t *ah = ahi[c];
        const uint8_t *bl = blo[c];
        const uint8_t *bh = bhi[c];
        uint8_t       *ql = qlo[c];
        uint8_t       *qh = qhi[c];

        for (int x = 0; x < w - 1; x++) {
            uint8_t l = al[x] > al[x + 1] ? al[x] : al[x + 1];
            uint8_t u = ah[x] < ah[x + 1] ? ah[x] : ah[x + 1];
            uint8_t l2 = bl[x] > bl[x + 1] ? bl[x] : bl[x + 1];
            uint8_t u2 = bh[x] < bh[x + 1] ? bh[x] : bh[x + 1];

            ql[x] = l > l2 ? l : l2;
            qh[x] = u < u2 ? u : u2;
        }
        if (w > 0) { /* the right edge: the last column stands in for its neighbour */
            int x = w - 1;
            uint8_t l = al[x] > bl[x] ? al[x] : bl[x];
            uint8_t u = ah[x] < bh[x] ? ah[x] : bh[x];

            ql[x] = l;
            qh[x] = u;
        }
    }
}

/* one channel of one output scanline, as 8-bit values. The 4x4 centred on a
 * pixel is the intersection of the 2x2s anchored at (y-1, x-1), (y-1, x+1),
 * (y+1, x-1) and (y+1, x+1); `m` is the 2x2 anchored at y, the fallback; and
 * `own` is what the unfiltered path would have written, for a pixel neither
 * window can answer for. Straight-line and branch-free so it vectorizes; the
 * two edge columns are combine_at()'s, with the window clamped. */
static void
combine_span(const uint8_t *restrict tl, const uint8_t *restrict th,
             const uint8_t *restrict ml, const uint8_t *restrict mh,
             const uint8_t *restrict bl, const uint8_t *restrict bh,
             const uint8_t *restrict own, int w, uint8_t *restrict res)
{
    /* This loop is where the frame's time is: 1.88 ms of a 2.4 ms frame at
     * 640x480 before it was vectorized, 0.86 after. Three things were needed
     * and the third is the one that matters. `restrict` on every pointer and
     * x - 1 / x + 1 as literals (the offset was a variable, for the clamped
     * edge columns, which combine_at() does instead now). Even then clang
     * *still* left it scalar, because its cost model says vectorising is not
     * worth it here. It is: 2.2x, measured. Hoisting the two fallback loads
     * out of the selects, so nothing is speculative, does not change its
     * mind (it comes out slower). Hence the pragma.
     *
     * GCC's cost model has not been measured; `-fopt-info-vec` on this file
     * is how to check what it does with it on the Linux build. */
#if defined(__clang__)
#pragma clang loop vectorize(enable)
#endif
    for (int x = 1; x < w - 1; x++) {
        uint8_t l = tl[x - 1] > tl[x + 1] ? tl[x - 1] : tl[x + 1];
        uint8_t u = th[x - 1] < th[x + 1] ? th[x - 1] : th[x + 1];
        uint8_t l2 = bl[x - 1] > bl[x + 1] ? bl[x - 1] : bl[x + 1];
        uint8_t u2 = bh[x - 1] < bh[x + 1] ? bh[x - 1] : bh[x + 1];
        uint8_t lo4 = l > l2 ? l : l2;
        uint8_t hi4 = u < u2 ? u : u2;
        /* the 4x4 holds more than one colour: fall back to the 2x2 */
        uint8_t lo = lo4 <= hi4 ? lo4 : ml[x];
        uint8_t hi = lo4 <= hi4 ? hi4 : mh[x];

        res[x] = lo <= hi ? (uint8_t) ((lo + hi) >> 1) : own[x];
    }
}

/* the same for one pixel, with the window's columns named: the two edge
 * columns, where x - 1 or x + 1 falls outside the scanline */
static uint8_t
combine_at(const uint8_t *tl, const uint8_t *th, const uint8_t *ml, const uint8_t *mh,
           const uint8_t *bl, const uint8_t *bh, int x, int x0, int x1, uint8_t own)
{
    uint8_t l = tl[x0] > tl[x1] ? tl[x0] : tl[x1];
    uint8_t u = th[x0] < th[x1] ? th[x0] : th[x1];
    uint8_t l2 = bl[x0] > bl[x1] ? bl[x0] : bl[x1];
    uint8_t u2 = bh[x0] < bh[x1] ? bh[x0] : bh[x1];
    uint8_t lo4 = l > l2 ? l : l2;
    uint8_t hi4 = u < u2 ? u : u2;
    uint8_t lo = lo4 <= hi4 ? lo4 : ml[x];
    uint8_t hi = lo4 <= hi4 ? hi4 : mh[x];

    return lo <= hi ? (uint8_t) ((lo + hi) >> 1) : own;
}

/* what the unfiltered path writes, one channel of a scanline */
static void
own_span(const uint16_t *src, int w, int shift, int mask, int up, uint8_t *own)
{
    for (int x = 0; x < w; x++) {
        own[x] = (uint8_t) (((src[x] >> shift) & mask) << up);
    }
}

/* the CLUT is a plain ramp unless the guest loaded one, and the lookup is
 * three dependent byte loads a pixel, worth the 256 compares to skip */
static int
clut_is_identity(const voodoo_t *v)
{
    for (int i = 0; i < 256; i++) {
        if (v->clutData256[i].r != i || v->clutData256[i].g != i ||
            v->clutData256[i].b != i) {
            return 0;
        }
    }
    return 1;
}

static void
emit_row(const voodoo_t *v, const uint16_t *src, uint32_t *out, int w, int plain,
         const row_t tlo, const row_t thi, const row_t mlo, const row_t mhi,
         const row_t blo, const row_t bhi)
{
    static const int shift[3] = { 0, 5, 11 };
    static const int mask[3]  = { 31, 63, 31 };
    static const int up[3]    = { 3, 2, 3 };
    static row_t     res;
    static uint8_t   own[3][UNDITHER_MAX_W];

    for (int c = 0; c < 3; c++) {
        own_span(src, w, shift[c], mask[c], up[c], own[c]);
        /* the body, then the two edge columns with the window clamped */
        combine_span(tlo[c], thi[c], mlo[c], mhi[c], blo[c], bhi[c], own[c], w, res[c]);
        res[c][0] = combine_at(tlo[c], thi[c], mlo[c], mhi[c], blo[c], bhi[c],
                               0, 0, w > 1 ? 1 : 0, own[c][0]);
        if (w > 1) {
            res[c][w - 1] = combine_at(tlo[c], thi[c], mlo[c], mhi[c], blo[c], bhi[c],
                                       w - 1, w - 2, w - 1, own[c][w - 1]);
        }
    }
    if (plain) {
        for (int x = 0; x < w; x++) {
            out[x] = (uint32_t) res[0][x] | ((uint32_t) res[1][x] << 8) |
                     ((uint32_t) res[2][x] << 16);
        }
    } else {
        for (int x = 0; x < w; x++) {
            out[x] = (uint32_t) v->clutData256[res[0][x]].b |
                     ((uint32_t) v->clutData256[res[1][x]].g << 8) |
                     ((uint32_t) v->clutData256[res[2][x]].r << 16);
        }
    }
}

static const uint16_t *
fb_row(const voodoo_t *v, int y)
{
    uint32_t off = (v->front_offset + (uint32_t) y * (uint32_t) v->row_width) & v->fb_mask;

    return (const uint16_t *) &v->fb_mem[off];
}

/* VOODOO2_UNDITHER_PATTERN=4x4|2x2 overrides what fbzMode says the
 * rasterizer used. It is the A/B when a frame looks wrong */
static int
forced_pattern(void)
{
    static int forced = -2;

    if (forced == -2) {
        const char *s = getenv("VOODOO2_UNDITHER_PATTERN");

        forced = !s ? -1 : !strcmp(s, "2x2") ? 1 : !strcmp(s, "4x4") ? 0 : -1;
    }
    return forced;
}

int
voodoo_undither_frame(struct voodoo_t *v, uint8_t *dst, int dst_stride,
                      int w, int h, const char **why)
{
    /* the main loop is the only caller (voodoo2_present, BQL held), so the
     * scanline scratch is static rather than 100 KB of stack */
    static row_t dlo[2], dhi[2]; /* decoded rows: k and k + 1        */
    static row_t qlo[4], qhi[4]; /* 2x2 rows: y - 1, y, y + 1        */
    uint32_t     fbzMode = v->params.fbzMode;
    int          two;
    int          plain;

    if (!(fbzMode & FBZ_DITHER)) {
        *why = "the guest is not dithering";
        return 0;
    }
    if (w <= 0 || h <= 0 || w > UNDITHER_MAX_W) {
        *why = "the mode is outside the frame buffer";
        return 0;
    }
    if (v->params.col_tiled) {
        /* a tiled colour buffer is not what the display path reads linearly */
        *why = "the colour buffer is tiled";
        return 0;
    }
    if ((uint64_t) v->front_offset + (uint64_t) h * (uint32_t) v->row_width >
        (uint64_t) v->fb_mask + 1 ||
        /* each row is read 2*w bytes from its start, whatever the stride */
        (uint64_t) v->front_offset + (uint64_t) (h - 1) * (uint32_t) v->row_width + 2u * (uint32_t) w >
        (uint64_t) v->fb_mask + 1) {
        *why = "the front buffer runs past the frame buffer";
        return 0;
    }

    build_tables();
    plain = clut_is_identity(v);
    two = (fbzMode & FBZ_DITHER_2x2) ? 1 : 0;
    if (forced_pattern() >= 0) {
        two = forced_pattern();
    }

    decode_row(fb_row(v, 0), w, 0, two, dlo[0], dhi[0]);
    for (int k = 0; k < h; k++) {
        int nk = k + 1;
        int y  = k - 1;

        if (nk < h) {
            decode_row(fb_row(v, nk), w, nk, two, dlo[nk & 1], dhi[nk & 1]);
        } else {
            nk = k; /* the bottom edge: the last row stands in for the next */
        }
        quad_row(dlo[k & 1], dhi[k & 1], dlo[nk & 1], dhi[nk & 1], w,
                 qlo[k & 3], qhi[k & 3]);
        if (y >= 0) {
            int t = (y ? y - 1 : 0) & 3;

            emit_row(v, fb_row(v, y), (uint32_t *) (dst + (size_t) y * (size_t) dst_stride),
                     w, plain, qlo[t], qhi[t], qlo[y & 3], qhi[y & 3],
                     qlo[(y + 1) & 3], qhi[(y + 1) & 3]);
        }
    }
    {   /* the last row, whose 2x2 row below it is its own */
        int y = h - 1;
        int t = (h >= 2 ? h - 2 : 0) & 3;

        emit_row(v, fb_row(v, y), (uint32_t *) (dst + (size_t) y * (size_t) dst_stride),
                 w, plain, qlo[t], qhi[t], qlo[y & 3], qhi[y & 3], qlo[y & 3], qhi[y & 3]);
    }
    return 1;
}
