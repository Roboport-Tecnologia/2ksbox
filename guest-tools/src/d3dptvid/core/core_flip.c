/*
 * core_flip.c: the adapter's geometry and its frame counter (doc 19,
 * "The split"). It knows where the DirectDraw heap, the cursor image and
 * the command window sit in VRAM, reads the ddflags bisection register,
 * waits for a vertical blank and tells whether the last page flip has
 * been scanned out. The debug log lives here too, because it is a
 * register write and nothing else.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windef.h>
#include <wingdi.h>
#include <ddraw.h>
#include "d3dpt_ddi.h"
#include "d3dpt_core.h"

d3dpt_core *d3d_core;               /* the core whose Direct3D is on (the primary display) */

/* ------------------------------------------------------------ debug log */

void dbg_puts(d3dpt_core *p, const char *s)
{
    if (!p || !p->regs) {
        return;
    }
    while (*s) {
        p->regs[D3DPT_FB_REG_DEBUG / 4] = (unsigned char)*s++;
    }
}

void dbg_hex(d3dpt_core *p, const char *tag, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[12];
    int i;

    if (!p || !p->regs) {
        return;
    }
    dbg_puts(p, tag);
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - 4 * i)) & 0xf];
    }
    buf[10] = 0;
    dbg_puts(p, buf);
}

/* ------------------------------------------------ the VRAM heap layout */

ULONG heap_start(d3dpt_core *p)
{
    return (p->fb_base + p->pitch * p->h + 4095) & ~4095u;
}

/* the DirectDraw heap ends where the command window starts */
ULONG heap_end(d3dpt_core *p)
{
    return p->cmd_offset ? p->cmd_offset : p->fb_len;
}

/* the DirectDraw heap ends below the hardware cursor's image (v4) */
ULONG dd_heap_end(d3dpt_core *p)
{
    return heap_end(p) - D3DPT_FB_CURSOR_BYTES;
}

ULONG cursor_offset(d3dpt_core *p)
{
    return dd_heap_end(p);
}


ULONG ddflags(d3dpt_core *p)
{
    return p->regs ? p->regs[D3DPT_FB_REG_DDFLAGS / 4] : 0;
}

void wait_frame(d3dpt_core *p)
{
    ULONG f, i;
    volatile ULONG spin = 0;
    LONGLONG t0, t, freq;

    if (!p->regs) {
        return;
    }
    f = p->regs[D3DPT_FB_REG_FRAMES / 4];
    d3dpt_os_ticks(&t0, &freq);
    /* the counter moves at the mode's refresh rate; give up after 50 ms so
     * a device that has stopped counting cannot stop the guest with it */
    for (;;) {
        if (p->regs[D3DPT_FB_REG_FRAMES / 4] != f) {
            return;
        }
        /* both the register read and XP's performance counter leave the
         * guest; pause between two polls so a 16 ms wait costs hundreds of
         * exits instead of tens of thousands */
        for (i = 0; i < 4000; i++) {
            spin = i;
        }
        (void)spin;
        d3dpt_os_ticks(&t, NULL);
        if ((t - t0) * 20 > freq) {
            return;
        }
    }
}

/* Has the last page flip been scanned out?
 *
 * FRAMES counts the mode's vertical blanks, so a flip issued at one count
 * is on the screen once the count has moved. Until then the flip is
 * pending. A real card does the same to a double-buffered chain, and it is
 * the only frame-rate cap a game of the era has. Without it a flip
 * chain runs at thousands of frames a second and every title that paces
 * itself by its own frame loop (Moto Racer, most 1997 racers) plays far
 * too fast.
 *
 * Bounded like wait_frame: a device that has stopped counting must not
 * stop the guest with it, so after 50 ms the flip counts as done.
 * DDF_NO_VSYNC brings the M7b behaviour back for throughput runs.
 */
BOOL flip_done(d3dpt_core *p)
{
    LONGLONG t, freq;

    if (!p->flip_pending) {
        return TRUE;
    }
    if (p->regs[D3DPT_FB_REG_FRAMES / 4] != p->flip_frame) {
        p->flip_pending = FALSE;
        return TRUE;
    }
    d3dpt_os_ticks(&t, &freq);
    if ((t - p->flip_qpc) * 20 > freq) {
        p->flip_pending = FALSE;
        return TRUE;
    }
    return FALSE;
}

/* A flip has just been issued. It is pending until the frame counter
 * moves (the layer's DdFlip calls this after writing the OFFSET
 * register). */
void flip_issued(d3dpt_core *p)
{
    if (ddflags(p) & DDF_NO_VSYNC) {
        p->flip_pending = FALSE;
        return;
    }
    p->flip_pending = TRUE;
    p->flip_frame = p->regs[D3DPT_FB_REG_FRAMES / 4];
    d3dpt_os_ticks(&p->flip_qpc, NULL);
}

/* DDWAITVB_I_TESTVB: is the display in its vertical blank right now? The
 * device counts frames and has no beam position, so the answer is yes once
 * per frame (the first time anyone asks after the counter moved) and no
 * otherwise. That keeps both loops a title of the era writes around
 * GetVerticalBlankStatus finite: `while (!in_vb)` ends at the next frame,
 * `while (in_vb)` at the next question. An answer of always no leaves
 * the first loop spinning forever. */
BOOL vb_test(d3dpt_core *p)
{
    ULONG f;

    if (!p->regs) {
        return FALSE;
    }
    f = p->regs[D3DPT_FB_REG_FRAMES / 4];
    if (f == p->vb_frame) {
        return FALSE;
    }
    p->vb_frame = f;
    return TRUE;
}

/* ------------------------------------------------- the command window */

static void d3d_doorbell(d3dpt_enc *e)
{
    d3dpt_core *p = (d3dpt_core *)((UCHAR *)e - (ULONG_PTR)&((d3dpt_core *)0)->enc);

    p->regs[D3DPT_FB_REG_DOORBELL / 4] = 1;
    if (d3dpt_enc_hdr(e)->ret_status && p->dp2_errors < 8) {
        p->dp2_errors++;
        dbg_hex(p, "d3dptdisp: batch error ", d3dpt_enc_hdr(e)->ret_status);
        dbg_hex(p, " at record ", d3dpt_enc_hdr(e)->ret_index);
        dbg_puts(p, "\n");
    }
}

BOOL d3d_init(d3dpt_core *p)
{
    if (p->d3d) {
        return TRUE;
    }
    if (!p->regs || !p->fb || !p->cmd_offset || (ddflags(p) & DDF_NO_D3D)) {
        return FALSE;
    }
    /* offered at 8 bpp too (no DX7 device can be created on a palettized
     * primary, the runtime refuses that itself). ddraw.dll fails a mode
     * switch when the HAL loses its Direct3D between two PDEVs (DDTEST
     * 640x480x8 got DDERR_UNSUPPORTEDMODE until this was consistent) */
    if (p->regs[D3DPT_FB_REG_D3D_STATUS / 4] != D3DPT_STATUS_READY) {
        dbg_puts(p, "d3dptdisp: no Direct3D executor on the host\n");
        return FALSE;
    }
    d3d_caps_init(p);
    d3dpt_enc_init(&p->enc, (uint8_t *)p->fb + p->cmd_offset, d3d_doorbell);
    p->d3d = TRUE;
    d3d_core = p;
    dbg_hex(p, "d3dptdisp: d3d window at ", p->cmd_offset);
    dbg_puts(p, "\n");
    return TRUE;
}

