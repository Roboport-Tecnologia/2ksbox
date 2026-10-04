/*
 * d3dpt_vga.c: the d3dpt-vga display adapter (doc 15, ADR-008 / M7a).
 *
 * A PCI VGA (QEMU's standard VGA core: SeaBIOS' stdvga ROM boots it, XP's
 * inbox vga.sys drives it at 640x480x16 until our driver is installed) with
 * one extra MMIO BAR of paravirtual registers (d3dpt/d3dpt_fb.h). Our XP
 * video miniport reads the host's mode table from it, programs a linear
 * mode and sets ENABLE; from then on the console shows VRAM directly:
 * the DisplaySurface is created over the guest's framebuffer bytes (no
 * copy inside QEMU), dirty lines come from the memory dirty log, and the
 * embed library's listener hands the same pointer to the player. 16 bpp
 * and 8 bpp (palettized, the PALETTE register block) modes are converted
 * into an x8r8g8b8 shadow per dirty span because the embed listener takes
 * one format; a palette write repaints the whole frame at the next
 * refresh. A gamma ramp (register set v5, GAMMA_ENABLE) is applied to
 * that shadow per span, as a RAMDAC would, and moves a 32 bpp mode onto a
 * shadow too while it changes anything. ENABLE = 0 returns the console to
 * the VGA core (BSOD, reboot, the BIOS).
 *
 * M7c (doc 15): the top 64 MiB of BAR 0 is a command window in the
 * d3dpt_proto.h layout. The display driver's Direct3D DDI appends records
 * there (surfaces it registers by VRAM offset, contexts, DrawPrimitives2
 * token streams) and writes DOORBELL; the batch runs synchronously in
 * the same libd3dpt_exec as the SysBus device (its own instance), which
 * reads texels straight out of VRAM and writes rendered frames back into
 * the render target's VRAM (marked dirty here so the scanout shows them).
 *
 * The device is not a QEMU patch: prepare-qemu.sh overlays d3dpt/hw into
 * qemu/hw/d3dpt and patch 40 adds the meson subdir. Use it as
 *   -vga none -device d3dpt-vga
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "ui/console.h"
#include "ui/qemu-pixman.h"
#include "qom/object.h"
#include "../display/vga_int.h"

#include "hw/d3dpt/d3dpt_fb.h"
#include "hw/d3dpt/d3dpt_proto.h"
#include "hw/d3dpt/d3dpt_exec_load.h"

#define TYPE_D3DPT_VGA "d3dpt-vga"
OBJECT_DECLARE_SIMPLE_TYPE(D3dptVgaState, D3DPT_VGA)

typedef struct D3dptLinearMode {
    uint32_t w, h, bpp, pitch, offset;
} D3dptLinearMode;

struct D3dptVgaState {
    PCIDevice dev;
    VGACommonState vga;
    MemoryRegion regs;

    /* guest-programmed registers */
    uint32_t r_enable, r_w, r_h, r_bpp, r_pitch, r_offset, r_hz, r_sel;
    int64_t vbl_ns;             /* clock at the last ENABLE: the vblank counter's origin */
    uint32_t flips;
    uint32_t flips_last;        /* flips at the last rate report */
    int64_t flips_ns;           /* and when it was made */
    uint32_t ddflags;           /* property: test knob read by the guest driver */
    bool irq;                   /* property: an interrupt pin (INTA). dxgkrnl will not
                                   start a WDDM adapter without one (M18); off keeps
                                   XP, 9x and snapshots unchanged */
    uint32_t irq_enable;        /* IRQ_ENABLE (register set v6) */
    uint32_t irq_status;        /* IRQ_STATUS: raised while enabled, not yet acknowledged */
    QEMUTimer *vbl_timer;       /* the vertical blank interrupt's, while IRQ_VBLANK is enabled */
    int64_t vbl_next_ns;        /* its next deadline (QEMU_CLOCK_VIRTUAL) */
    uint32_t dma_lo, dma_hi, dma_bytes, dma_status; /* DMA_* (register set v7) */
    uint32_t fence_done;        /* FENCE_DONE */
    uint64_t dma_appends, dma_appended; /* appends and their bytes, for the rate line */
    bool no_exec;              /* property: act as a host with no Vulkan 1.3 device
                                   (ADR-013's floor unmet); D3D_STATUS then reads
                                   NO_EXEC and the guest driver offers DirectDraw only */
    char *d3d9;                 /* property: which Direct3D 9 the executor runs on:
                                   auto (DXVK, then this host's own on Windows),
                                   dxvk or system (d3dpt_exec_load.h) */
    char *exec_pick;            /* property `exec`: which executor library: auto, dxvk
                                   (in process), wine (another process, M15), none */
    uint32_t fb_version;        /* property: the VERSION register (D3DPT_FB_VERSION);
                                   a newer one checks that installed drivers accept it */

    /* the hardware cursor (version 4): the guest's registers, and what was
     * defined / shown so far for the log */
    uint32_t cur_addr, cur_w, cur_h, cur_hot_x, cur_hot_y;
    int32_t cur_x, cur_y;
    bool cur_on, cur_defined;
    uint32_t cur_defines, cur_moves;
    bool cur_shown;             /* the visibility last published */
    bool cur_flip_hidden;       /* a flip chain has the screen: see fb_cursor_move */
    uint32_t cur_flip_desktop;  /* the scanout offset before the chain's first flip */
    QEMUTimer *cur_flip_timer;  /* no flip for D3DPT_FB_FLIP_IDLE_MS: see fb_cursor_flip_idle */
    uint32_t cur_flip_idles;    /* chains ended that way, for the log */
    uint32_t cur_flips;         /* show / hide changes logged so far */

    /* the linear mode currently shown (lin_on) */
    bool lin_on;
    bool full_update;
    bool full_frames;           /* property: every refresh a whole frame */
    bool resurface;             /* invalidated: put our own surface on the console again */
    int64_t vga_grace_until;    /* hold the last frame after ENABLE 1->0 until then (ms) */
    uint8_t vga_sig[8];         /* the VGA core mode last reported */
    D3dptLinearMode lin;
    pixman_image_t *shadow;     /* x8r8g8b8 copy for 16 and 8 bpp modes */
    pixman_image_t *src;        /* r5g6b5 / c8 view of VRAM for the conversion */
    uint32_t pal[D3DPT_FB_PALETTE_SIZE]; /* the PALETTE registers */
    bool pal_dirty;             /* written since the last refresh */
    pixman_indexed_t *indexed;  /* pal as pixman's palette for the c8 view */
    /* the gamma ramp (version 5): the GAMMA registers, GAMMA_ENABLE, the
     * tables made of them at that write, whether they change anything (an
     * identity ramp keeps a 32 bpp mode straight on VRAM), and a repaint
     * pending since */
    uint32_t gamma[D3DPT_FB_GAMMA_SIZE];
    bool gamma_on, gamma_active, gamma_dirty;
    uint8_t lut_r[256], lut_g[256], lut_b[256];
    unsigned gamma_lines;

    char dbg[256];
    unsigned dbg_len;

    /* M7c: the command window at the top of VRAM and its executor */
    uint32_t cmd_offset;        /* 0 = VRAM too small for a window */
    const D3dptExecLib *lib;
    d3dpt_exec_t *exec;
    bool exec_tried;
    uint32_t d3d_err;           /* last D3DPT_ERR_* of the doorbell */
    uint32_t batches;
    int64_t exec_ns;            /* host time inside the executor, all batches */
    uint32_t batches_last;      /* both at the last rate report */
    int64_t exec_ns_last;
};

/* ------------------------------------------------------------ mode table
 * Static for M7a; the M2 mode table (pixel aspect, per-preset lists) will
 * feed this from the player. Every size is offered at each refresh and
 * depth: XP's mode picker and fullscreen games choose from it, the refresh
 * is what the CRT preset later honours, no timing is emulated. */
static const struct { uint16_t w, h; } fb_sizes[] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 },
    { 1280, 960 }, { 1280, 1024 }, { 1600, 1200 },
};
static const uint8_t fb_hz[] = { 60, 75, 85 };
static const uint8_t fb_bpp[] = { 8, 16, 32 };
#define FB_MODE_COUNT (ARRAY_SIZE(fb_sizes) * ARRAY_SIZE(fb_hz) * ARRAY_SIZE(fb_bpp))

/* how long (ms) the last linear frame stays up after ENABLE goes 0 before
 * the VGA core is shown. Wall clock, not refreshes. 15 refreshes is 250 ms
 * under the player but 45 s in a headless run, where an idle console
 * refreshes every 3 s, and 45 s of a stale desktop over a DirectDraw
 * Mode X game read as the game drawing nothing (doc 19 §30). */
#define D3DPT_FB_VGA_GRACE_MS 250

static bool fb_mode_entry(uint32_t sel, uint32_t *w, uint32_t *h,
                          uint32_t *bpp, uint32_t *hz)
{
    if (sel >= FB_MODE_COUNT) {
        return false;
    }
    *bpp = fb_bpp[sel % ARRAY_SIZE(fb_bpp)];
    sel /= ARRAY_SIZE(fb_bpp);
    *hz = fb_hz[sel % ARRAY_SIZE(fb_hz)];
    sel /= ARRAY_SIZE(fb_hz);
    *w = fb_sizes[sel].w;
    *h = fb_sizes[sel].h;
    return true;
}

/* --------------------------------------------------------------- display */

static bool fb_get_mode(D3dptVgaState *s, D3dptLinearMode *m)
{
    uint32_t bytepp;

    if (!s->r_enable) {
        return false;
    }
    if (s->r_bpp != 8 && s->r_bpp != 16 && s->r_bpp != 32) {
        return false;
    }
    bytepp = s->r_bpp / 8;
    m->w = s->r_w;
    m->h = s->r_h;
    m->bpp = s->r_bpp;
    m->pitch = s->r_pitch ? s->r_pitch : m->w * bytepp;
    m->offset = s->r_offset;
    if (m->w < 64 || m->h < 64 || m->w > 8192 || m->h > 8192) {
        return false;
    }
    if (m->pitch < m->w * bytepp || m->pitch & 3) {
        return false;
    }
    if ((uint64_t)m->offset + (uint64_t)m->pitch * m->h > s->vga.vram_size) {
        return false;
    }
    return true;
}

static void fb_drop_shadow(D3dptVgaState *s)
{
    if (s->shadow) {
        qemu_pixman_image_unref(s->shadow);
        s->shadow = NULL;
    }
    if (s->src) {
        qemu_pixman_image_unref(s->src);
        s->src = NULL;
    }
}

static void fb_apply_palette(D3dptVgaState *s)
{
    int i;

    for (i = 0; i < D3DPT_FB_PALETTE_SIZE; i++) {
        s->indexed->rgba[i] = 0xff000000u | (s->pal[i] & 0xffffffu);
    }
    s->pal_dirty = false;
}

/* the GAMMA block as three tables, at the GAMMA_ENABLE write: the guest
 * writes the entries first. Said in the log whenever it starts or stops
 * changing the picture (bounded: a game's fade is a ramp per frame) */
static void fb_gamma_apply(D3dptVgaState *s)
{
    bool ident = true, was = s->gamma_active;
    int i;

    for (i = 0; i < D3DPT_FB_GAMMA_SIZE; i++) {
        s->lut_r[i] = (uint8_t)(s->gamma[i] >> 16);
        s->lut_g[i] = (uint8_t)(s->gamma[i] >> 8);
        s->lut_b[i] = (uint8_t)s->gamma[i];
        if (s->lut_r[i] != i || s->lut_g[i] != i || s->lut_b[i] != i) {
            ident = false;
        }
    }
    s->gamma_active = s->gamma_on && !ident;
    s->gamma_dirty = true;
    if (s->gamma_active != was && s->gamma_lines < 64) {
        s->gamma_lines++;
        info_report("d3dpt-vga: gamma ramp %s (0 -> %06x, 128 -> %06x, 255 -> %06x)",
                    s->gamma_active ? "on" : "off", s->gamma[0] & 0xffffffu,
                    s->gamma[128] & 0xffffffu, s->gamma[255] & 0xffffffu);
    }
}

static void fb_switch(D3dptVgaState *s, const D3dptLinearMode *m)
{
    uint8_t *ptr = memory_region_get_ram_ptr(&s->vga.vram) + m->offset;
    DisplaySurface *ds;

    fb_drop_shadow(s);
    if (m->bpp == 32 && !s->gamma_active) {
        ds = qemu_create_displaysurface_from(m->w, m->h, PIXMAN_x8r8g8b8,
                                             m->pitch, ptr);
    } else {
        if (m->bpp == 32) {
            /* a gamma ramp on (version 5): a copy it can be applied to */
            s->src = pixman_image_create_bits(PIXMAN_x8r8g8b8, m->w, m->h,
                                              (uint32_t *)ptr, m->pitch);
        } else if (m->bpp == 16) {
            s->src = pixman_image_create_bits(PIXMAN_r5g6b5, m->w, m->h,
                                              (uint32_t *)ptr, m->pitch);
        } else {
            /* indices through the palette: pixman's c8 fetcher looks each
             * byte up in the indexed table the image points at */
            s->src = pixman_image_create_bits(PIXMAN_c8, m->w, m->h,
                                              (uint32_t *)ptr, m->pitch);
            fb_apply_palette(s);
            pixman_image_set_indexed(s->src, s->indexed);
        }
        s->shadow = pixman_image_create_bits(PIXMAN_x8r8g8b8, m->w, m->h,
                                             NULL, 0);
        ds = qemu_create_displaysurface_pixman(s->shadow);
    }
    qemu_console_set_surface(s->vga.con, ds);
    s->lin = *m;
    s->lin_on = true;
    s->full_update = true;
    s->resurface = false;
}

static void fb_update_span(D3dptVgaState *s, int y0, int y1)
{
    if (s->shadow) {
        pixman_image_composite(PIXMAN_OP_SRC, s->src, NULL, s->shadow,
                               0, y0, 0, 0, 0, y0, s->lin.w, y1 - y0);
        if (s->gamma_active) {
            /* the ramp, per channel, over the converted span */
            uint32_t *d = pixman_image_get_data(s->shadow);
            int stride = pixman_image_get_stride(s->shadow) / 4, x, y;

            for (y = y0; y < y1; y++) {
                uint32_t *row = d + (size_t)y * stride;
                for (x = 0; x < (int)s->lin.w; x++) {
                    uint32_t v = row[x];
                    row[x] = (v & 0xff000000u) | ((uint32_t)s->lut_r[(v >> 16) & 0xff] << 16) |
                             ((uint32_t)s->lut_g[(v >> 8) & 0xff] << 8) | s->lut_b[v & 0xff];
                }
            }
        }
    }
    qemu_console_update(s->vga.con, 0, y0, s->lin.w, y1 - y0);
}

/* The vertical blank the guest waits on (REG_FRAMES).
 *
 * It counts periods of the mode's refresh rate since the mode was enabled,
 * off the host clock, not the display client's pull. The guest's frame
 * pacing then does not depend on whether anything is looking: the player
 * pulls at its own interval, a headless run pulls not at all, and a game
 * still gets the 60 (or 85) Hz it asked for.
 */
static uint32_t fb_vblank_count(D3dptVgaState *s)
{
    uint32_t hz = s->r_hz;

    if (!s->vbl_ns) {
        return 0;
    }
    if (hz < 30 || hz > 200) {
        hz = 60;   /* the guest left HZ unset or nonsensical */
    }
    return (uint32_t)((qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - s->vbl_ns) *
                      hz / NANOSECONDS_PER_SECOND);
}

/* The interrupt line (register set v6): high while an enabled event is
 * pending, until the driver acknowledges it in IRQ_STATUS. */
static void fb_irq_update(D3dptVgaState *s)
{
    if (s->irq) {
        pci_set_irq(&s->dev, (s->irq_status & s->irq_enable) != 0);
    }
}

/* The vertical blank interrupt (IRQ_VBLANK): every period of the mode's
 * refresh on the guest's clock, which stops with the VM, so a paused or
 * snapshotted guest is not handed a burst of them. A deadline missed by
 * more than a period (a host stall) is skipped, not caught up: one
 * interrupt, as a display that kept scanning out would give. */
static uint32_t fb_refresh_hz(D3dptVgaState *s)
{
    return s->r_hz >= 30 && s->r_hz <= 200 ? s->r_hz : 60;
}

static void fb_vbl_arm(D3dptVgaState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t period = NANOSECONDS_PER_SECOND / fb_refresh_hz(s);

    s->vbl_next_ns += period;
    if (s->vbl_next_ns <= now) {
        s->vbl_next_ns = now + period;
    }
    timer_mod(s->vbl_timer, s->vbl_next_ns);
}

static void fb_vbl_tick(void *opaque)
{
    D3dptVgaState *s = opaque;

    if (!(s->irq_enable & D3DPT_FB_IRQ_VBLANK)) {
        return;
    }
    s->irq_status |= D3DPT_FB_IRQ_VBLANK;
    fb_irq_update(s);
    fb_vbl_arm(s);
}

static void fb_irq_enable(D3dptVgaState *s, uint32_t val)
{
    uint32_t was = s->irq_enable;

    s->irq_enable = s->irq ? val & (D3DPT_FB_IRQ_VBLANK | D3DPT_FB_IRQ_DMA) : 0;
    s->irq_status &= s->irq_enable;         /* nothing stays pending that is off */
    if ((s->irq_enable & D3DPT_FB_IRQ_VBLANK) && !(was & D3DPT_FB_IRQ_VBLANK)) {
        s->vbl_next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        fb_vbl_arm(s);
    } else if (!(s->irq_enable & D3DPT_FB_IRQ_VBLANK)) {
        timer_del(s->vbl_timer);
    }
    fb_irq_update(s);
}

/* The guest's real frame rate. A game of the era paces itself by its flip
 * chain, so page flips per second is the number that says whether it runs
 * at the display's rate or as fast as the CPU allows; a game that blits to
 * the primary instead prints nothing here, which is the answer too. It is
 * driven by the flips themselves, not by the refresh, so a headless run
 * (xp-game-test.sh) reports the same as the player. */
static void fb_flip_rate(D3dptVgaState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    double secs;

    if (!s->flips_ns) {
        s->flips_ns = now;
        s->flips_last = s->flips;
        return;
    }
    if (now - s->flips_ns < 5 * NANOSECONDS_PER_SECOND) {
        return;
    }
    secs = (double)(now - s->flips_ns) / NANOSECONDS_PER_SECOND;
    if (s->flips != s->flips_last) {
        info_report("d3dpt-vga: %u page flips in %.1f s (%.1f/s)",
                    s->flips - s->flips_last, secs,
                    (s->flips - s->flips_last) / secs);
    }
    if (s->batches != s->batches_last) {
        /* the host's own share of the frame: see d3d_doorbell */
        info_report("d3dpt-vga: %u batches in %.1f s, %.1f ms of them in the "
                    "executor (%.2f ms a batch, %.0f%% of the time)",
                    s->batches - s->batches_last, secs,
                    (s->exec_ns - s->exec_ns_last) / 1e6,
                    (s->exec_ns - s->exec_ns_last) / 1e6 /
                        (s->batches - s->batches_last),
                    100.0 * (s->exec_ns - s->exec_ns_last) /
                        (now - s->flips_ns));
    }
    if (s->dma_appends) {
        /* register set v7: the records the WDDM driver had the device copy */
        info_report("d3dpt-vga: %" PRIu64 " DMA appends, %" PRIu64 " KiB of records, fence %u",
                    s->dma_appends, s->dma_appended >> 10, s->fence_done);
        s->dma_appends = s->dma_appended = 0;
    }
    s->batches_last = s->batches;
    s->exec_ns_last = s->exec_ns;
    s->flips_ns = now;
    s->flips_last = s->flips;
}

static bool d3dpt_vga_gfx_update(void *opaque)
{
    D3dptVgaState *s = opaque;
    D3dptLinearMode m;
    DirtyBitmapSnapshot *snap;
    int y, ys;

    if (!fb_get_mode(s, &m)) {
        if (s->lin_on) {
            /* A mode switch is RESET (ENABLE = 0) followed by the new mode a
             * few ms later; showing the VGA core in between flashes text
             * mode or stale VGA memory in the player. Hold the last frame
             * for a moment: a real return to VGA (BSOD, full-screen console,
             * reboot) is only delayed by that. */
            if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) < s->vga_grace_until) {
                return true;
            }
            /* back to the VGA core: it recreates its own surface */
            s->lin_on = false;
            fb_drop_shadow(s);
            s->vga.hw_ops->invalidate(&s->vga);
        }
        s->vga.hw_ops->gfx_update(&s->vga);
        /* The VGA core's mode, once per change: which VGA mode a guest
         * programmed after the driver let go (a DirectDraw Mode X, a DOS
         * game's mode 13h, a blue screen's text mode) is otherwise
         * invisible in a headless run (doc 19 §30). */
        {
            VGACommonState *v = &s->vga;
            uint8_t sig[8] = { v->cr[0x01], v->cr[0x07], v->cr[0x09], v->cr[0x12],
                               v->sr[0x04], v->gr[0x05], v->gr[0x06], v->ar[0x10] };
            if (memcmp(sig, s->vga_sig, sizeof(sig)) != 0) {
                memcpy(s->vga_sig, sig, sizeof(sig));
                info_report("d3dpt-vga: vga core cr1=%02x cr7=%02x cr9=%02x cr12=%02x "
                            "sr4=%02x gr5=%02x gr6=%02x ar10=%02x",
                            sig[0], sig[1], sig[2], sig[3], sig[4], sig[5], sig[6], sig[7]);
            }
        }
        return true;
    }

    if (s->gamma_dirty) {
        /* a new ramp recolours every pixel; a 32 bpp mode moves between
         * VRAM itself and the shadow the ramp is applied in */
        s->gamma_dirty = false;
        if (s->lin_on && m.bpp == 32 && (s->shadow != NULL) != s->gamma_active) {
            s->lin_on = false;
        }
        s->full_update = true;
    }
    if (!s->lin_on || s->resurface || memcmp(&s->lin, &m, sizeof(m)) != 0) {
        fb_switch(s, &m);
    }
    if (s->pal_dirty && m.bpp == 8) {
        /* a new palette recolours every pixel: one full conversion */
        fb_apply_palette(s);
        s->full_update = true;
    }
    if (s->full_frames) {
        s->full_update = true;
    }

    if (s->full_update) {
        s->full_update = false;
        /* consume the dirty log so the next refresh starts clean */
        snap = memory_region_snapshot_and_clear_dirty(&s->vga.vram, m.offset,
                                                      (uint64_t)m.pitch * m.h,
                                                      DIRTY_MEMORY_VGA);
        g_free(snap);
        fb_update_span(s, 0, m.h);
        return true;
    }

    snap = memory_region_snapshot_and_clear_dirty(&s->vga.vram, m.offset,
                                                  (uint64_t)m.pitch * m.h,
                                                  DIRTY_MEMORY_VGA);
    ys = -1;
    for (y = 0; y < (int)m.h; y++) {
        bool dirty = memory_region_snapshot_get_dirty(&s->vga.vram, snap,
                                                      m.offset + (uint64_t)m.pitch * y,
                                                      m.pitch);
        if (dirty && ys < 0) {
            ys = y;
        }
        if (!dirty && ys >= 0) {
            fb_update_span(s, ys, y);
            ys = -1;
        }
    }
    if (ys >= 0) {
        fb_update_span(s, ys, y);
    }
    g_free(snap);
    return true;
}

static void d3dpt_vga_invalidate(void *opaque)
{
    D3dptVgaState *s = opaque;

    /* A full frame, and into a surface of our own: another device may have
     * had the console meanwhile and left its surface there (a Voodoo 2
     * giving the monitor back through VGA pass-through does exactly that),
     * and a linear mode that did not change would otherwise go on updating
     * that stranger's surface for ever, so the picture never came back
     * (doc 21 §7). */
    s->full_update = true;
    s->resurface = true;
    s->vga.hw_ops->invalidate(&s->vga);
}

static void d3dpt_vga_text_update(void *opaque, uint32_t *chardata)
{
    D3dptVgaState *s = opaque;

    if (!s->r_enable) {
        s->vga.hw_ops->text_update(&s->vga, chardata);
    }
}

static const GraphicHwOps d3dpt_vga_gfx_ops = {
    .invalidate  = d3dpt_vga_invalidate,
    .gfx_update  = d3dpt_vga_gfx_update,
    .text_update = d3dpt_vga_text_update,
};

/* ------------------------------------------------------------- Direct3D */

static void d3d_log(void *ud, const char *msg)
{
    info_report("d3dpt-vga: %s", msg);
}

static void d3d_active(void *ud, int on)
{
    /* frames land in VRAM: nothing to switch in the presenter */
}

static void d3d_vram_dirty(void *ud, uint32_t offset, uint32_t bytes)
{
    D3dptVgaState *s = ud;

    if ((uint64_t)offset + bytes <= s->cmd_offset) {
        memory_region_set_dirty(&s->vga.vram, offset, bytes);
    }
}

static bool d3d_load(D3dptVgaState *s)
{
    d3dpt_exec_ops ops = { s, d3d_log, d3d_active, d3d_vram_dirty };

    if (s->exec_tried || !s->cmd_offset) {
        return s->exec != NULL;
    }
    s->exec_tried = true;
    if (!s->lib) {
        s->lib = d3dpt_exec_lib();
    }
    if (!s->lib) {
        return false;
    }
    s->exec = s->lib->create(&ops);
    if (!s->exec) {
        warn_report("d3dpt-vga: executor refused to start (no DXVK / Vulkan device)");
        return false;
    }
    s->lib->set_vram(s->exec, memory_region_get_ram_ptr(&s->vga.vram), s->cmd_offset);
    s->lib->attach(s->exec, 1);
    info_report("d3dpt-vga: Direct3D executor ready, window at %u MiB", s->cmd_offset >> 20);
    return true;
}

static void d3d_doorbell(D3dptVgaState *s)
{
    uint8_t *win = memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset;
    d3dpt_shm_hdr *h = (d3dpt_shm_hdr *)win;

    if (!s->cmd_offset) {
        return;
    }
    s->batches++;
    if (d3d_load(s)) {
        /* How long the host spends in a batch, against how long the guest
         * takes to bring the next one: a frame rate that halves with the
         * host's share flat is a slow guest, and one that halves with the
         * share grown is us (3DMark slow after some reboots). */
        int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

        s->d3d_err  = s->lib->submit(s->exec, win, D3DPT_SHM_SIZE);
        s->exec_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
    } else {
        s->d3d_err = D3DPT_ERR_HOST;
        h->ret_status = D3DPT_ERR_HOST;
        h->cmd_bytes = 0;
        h->cmd_count = 0;
    }
    if (s->d3d_err && s->batches <= 8) {
        info_report("d3dpt-vga: batch %u: error %u at record %u", s->batches, s->d3d_err, h->ret_index);
    }
    fb_flip_rate(s);    /* the 5 s line, for a game that blits and never flips */
}

/* DMA_APPEND (register set v7): `count` records from guest memory at
 * DMA_ADDR, DMA_BYTES of them, to the end of the window's batch, as the
 * guest's encoder would have written them. The executor checks the
 * records themselves at the doorbell (a count that does not match the
 * bytes is its MALFORMED); here only that they fit and can be read. */
static uint32_t d3d_dma_append(D3dptVgaState *s, uint32_t count)
{
    uint8_t *win = memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset;
    d3dpt_shm_hdr *h = (d3dpt_shm_hdr *)win;
    dma_addr_t addr = ((uint64_t)s->dma_hi << 32) | s->dma_lo;
    uint32_t bytes = s->dma_bytes;

    if (!s->cmd_offset || !count || !bytes || (bytes & 7) || h->cmd_bytes > D3DPT_CMD_SIZE) {
        return D3DPT_FB_DMA_BAD;
    }
    if (bytes > D3DPT_CMD_SIZE - h->cmd_bytes) {
        return D3DPT_FB_DMA_NO_ROOM;
    }
    if (pci_dma_read(&s->dev, addr, win + D3DPT_CMD_OFFSET + h->cmd_bytes, bytes) != MEMTX_OK) {
        return D3DPT_FB_DMA_BAD;
    }
    h->cmd_bytes += bytes;
    h->cmd_count += count;
    s->dma_appends++;
    s->dma_appended += bytes;
    return D3DPT_FB_DMA_OK;
}

/* guest reset: every context and surface the driver registered is gone */
static void d3d_reset(D3dptVgaState *s)
{
    if (s->exec) {
        s->lib->attach(s->exec, 0);
        s->lib->attach(s->exec, 1);
    }
    if (s->cmd_offset) {
        memset(memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset, 0, sizeof(d3dpt_shm_hdr));
    }
    s->d3d_err = 0;
    s->batches = 0;
}

/* ------------------------------------------------------------- registers */

/* ---------------------------------------------------- the hardware cursor
 * (version 4): the shape is read out of VRAM at CURSOR_DEFINE and handed
 * to the console as a QEMUCursor; the position and visibility go through
 * dpy_mouse_set. The display clients composite it (the player: as the host
 * window's cursor); the device draws nothing into the frame. */
/* No shape: **never `dpy_cursor_define(con, NULL)`**. QEMU's console takes
 * a reference on the cursor it is handed and dereferences it (cursor_ref),
 * so a NULL is a SIGSEGV in whatever process the device lives in (the
 * player crashed this way on a Win98 restart once the reset below cleared
 * the shape). A hidden 1x1 transparent cursor is what "no cursor" is to
 * the console. */
static void fb_cursor_clear(D3dptVgaState *s)
{
    QEMUCursor *c = cursor_alloc(1, 1);

    c->data[0] = 0;
    s->cur_defined = false;
    s->cur_on = false;
    qemu_console_set_mouse(s->vga.con, s->cur_x, s->cur_y, false);
    qemu_console_set_cursor(s->vga.con, c);
    cursor_unref(c);
}

static void fb_cursor_define(D3dptVgaState *s, bool on)
{
    QEMUCursor *c;
    uint32_t w = s->cur_w, h = s->cur_h;
    uint64_t bytes = (uint64_t)w * h * 4;

    if (!on) {
        fb_cursor_clear(s);
        return;
    }
    if (!w || !h || w > D3DPT_FB_CURSOR_MAX || h > D3DPT_FB_CURSOR_MAX ||
        s->cur_addr + bytes > s->vga.vram_size ||
        s->cur_hot_x >= w || s->cur_hot_y >= h) {
        warn_report("d3dpt-vga: cursor %ux%u at %u hot %u,%u refused",
                    w, h, s->cur_addr, s->cur_hot_x, s->cur_hot_y);
        return;
    }
    c = cursor_alloc(w, h);
    memcpy(c->data, memory_region_get_ram_ptr(&s->vga.vram) + s->cur_addr, bytes);
    c->hot_x = s->cur_hot_x;
    c->hot_y = s->cur_hot_y;
    qemu_console_set_cursor(s->vga.con, c);
    cursor_unref(c);
    s->cur_defined = true;
    if (s->cur_defines++ < 4) {
        info_report("d3dpt-vga: cursor %ux%u hot %u,%u defined", w, h, s->cur_hot_x, s->cur_hot_y);
    }
}

/* While a DirectDraw flip chain is scanning out, the sprite is hidden too,
 * from the first flip until the chain gives the screen back: a mode set
 * (ENABLE written), or the flips stopping on the desktop's own page (see
 * fb_cursor_flip_idle). Windows' pointer stays enabled behind an
 * exclusive-mode game that never hides it (on a card without a hardware
 * cursor GDI's pointer lives in the front buffer and the first flip wipes
 * it, so a page-flipping game draws its own), and this device's sprite
 * put Windows' arrow beside Moto Racer's in its menus. No
 * guest driver learns of exclusive mode on 9x, so the flip is the signal.
 *
 * A VGA screen has no hardware cursor. While ENABLE is off (a full-screen
 * DOS box, a blue screen, the moments of a mode switch) the sprite stays
 * hidden whatever CURSOR_ENABLE says. The guest's driver is not running the
 * screen then and nothing of its will turn the sprite off. The player
 * composites the sprite into the frame when the pointer is grabbed, and
 * over Blood's 640x480 VGA frame the desktop's arrow came out at the
 * desktop's coordinates, scaled with the frame. */
/* How long without a page flip before a flip chain counts as gone. Guest
 * time, so a slow TCG frame or a paused VM does not count; long enough that
 * a game at a few frames a second keeps the sprite hidden. */
#define D3DPT_FB_FLIP_IDLE_MS 2000

static void fb_cursor_move(D3dptVgaState *s);

/* The other way a flip chain gives the screen back: it stops flipping, on
 * the page the desktop was on before its first flip, which is where
 * DirectDraw leaves the scanout when the chain is released. A game that
 * plays at the desktop's own mode never sets a mode on the way out (the
 * runtime only calls the driver's SetMode when the mode changes), so "until
 * the next mode set" hid the pointer for good after 3DMark 99 at 800x600x16.
 * A game still on its other page stays hidden; one idle on
 * the desktop's page (a loading screen) gets the pointer until its next
 * flip, which is what a card with no hardware cursor would show too. */
static void fb_cursor_flip_idle(void *opaque)
{
    D3dptVgaState *s = opaque;

    if (!s->cur_flip_hidden || !s->r_enable || s->r_offset != s->cur_flip_desktop) {
        return;
    }
    s->cur_flip_hidden = false;
    if (s->cur_flip_idles++ < 64) {
        info_report("d3dpt-vga: no page flip for %d ms on the desktop's page (offset %u): "
                    "the flip chain is gone", D3DPT_FB_FLIP_IDLE_MS, s->r_offset);
    }
    if (s->cur_defined && s->cur_on) {
        fb_cursor_move(s);
    }
}

static void fb_cursor_move(D3dptVgaState *s)
{
    bool on = s->cur_on && s->r_enable && !s->cur_flip_hidden;

    /* the first moves, and then every show / hide: a pointer that should
     * not be on the screen (a game drawing its own) is a question of when
     * the guest turned it off, which the first four lines never reach */
    if (s->cur_moves++ < 4) {
        info_report("d3dpt-vga: cursor %s at %d,%d", on ? "shown" : "hidden", s->cur_x, s->cur_y);
    } else if (on != s->cur_shown && s->cur_flips++ < 256) {
        info_report("d3dpt-vga: cursor %s at %d,%d (%ux%u mode)", on ? "shown" : "hidden",
                    s->cur_x, s->cur_y, s->r_w, s->r_h);
    }
    s->cur_shown = on;
    qemu_console_set_mouse(s->vga.con, s->cur_x, s->cur_y, on);
}

static uint64_t d3dpt_vga_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    D3dptVgaState *s = opaque;
    uint32_t w, h, bpp, hz;

    switch (addr) {
    case D3DPT_FB_REG_MAGIC:
        return D3DPT_FB_MAGIC;
    case D3DPT_FB_REG_VERSION:
        return s->fb_version;
    case D3DPT_FB_REG_VRAM_SIZE:
        return s->vga.vram_size;
    case D3DPT_FB_REG_CAPS:
        return D3DPT_FB_CAP_BPP8 | D3DPT_FB_CAP_BPP16 | D3DPT_FB_CAP_BPP32 |
               D3DPT_FB_CAP_CURSOR | D3DPT_FB_CAP_GAMMA | (s->cmd_offset ? D3DPT_FB_CAP_D3D : 0) |
               (s->irq ? D3DPT_FB_CAP_IRQ : 0) | (s->cmd_offset ? D3DPT_FB_CAP_DMA : 0);
    case D3DPT_FB_REG_IRQ_ENABLE:
        return s->irq_enable;
    case D3DPT_FB_REG_IRQ_STATUS:
        return s->irq_status;
    case D3DPT_FB_REG_DMA_ADDR_LO:
        return s->dma_lo;
    case D3DPT_FB_REG_DMA_ADDR_HI:
        return s->dma_hi;
    case D3DPT_FB_REG_DMA_BYTES:
        return s->dma_bytes;
    case D3DPT_FB_REG_DMA_APPEND:
        return s->dma_status;
    case D3DPT_FB_REG_FENCE_DONE:
        return s->fence_done;
    case D3DPT_FB_REG_CURSOR_ADDR:
        return s->cur_addr;
    case D3DPT_FB_REG_CURSOR_W:
        return s->cur_w;
    case D3DPT_FB_REG_CURSOR_H:
        return s->cur_h;
    case D3DPT_FB_REG_CURSOR_HOT_X:
        return s->cur_hot_x;
    case D3DPT_FB_REG_CURSOR_HOT_Y:
        return s->cur_hot_y;
    case D3DPT_FB_REG_CURSOR_X:
        return (uint32_t)s->cur_x;
    case D3DPT_FB_REG_CURSOR_Y:
        return (uint32_t)s->cur_y;
    case D3DPT_FB_REG_CURSOR_ENABLE:
        return s->cur_on;
    case D3DPT_FB_REG_GAMMA_ENABLE:
        return s->gamma_on;
    case D3DPT_FB_REG_MODE_COUNT:
        return FB_MODE_COUNT;
    case D3DPT_FB_REG_MODE_SEL:
        return s->r_sel;
    case D3DPT_FB_REG_MODE_W:
        return fb_mode_entry(s->r_sel, &w, &h, &bpp, &hz) ? w : 0;
    case D3DPT_FB_REG_MODE_H:
        return fb_mode_entry(s->r_sel, &w, &h, &bpp, &hz) ? h : 0;
    case D3DPT_FB_REG_MODE_BPP:
        return fb_mode_entry(s->r_sel, &w, &h, &bpp, &hz) ? bpp : 0;
    case D3DPT_FB_REG_MODE_HZ:
        return fb_mode_entry(s->r_sel, &w, &h, &bpp, &hz) ? hz : 0;
    case D3DPT_FB_REG_ENABLE:
        return s->r_enable;
    case D3DPT_FB_REG_WIDTH:
        return s->r_w;
    case D3DPT_FB_REG_HEIGHT:
        return s->r_h;
    case D3DPT_FB_REG_BPP:
        return s->r_bpp;
    case D3DPT_FB_REG_PITCH:
        return s->r_pitch;
    case D3DPT_FB_REG_OFFSET:
        return s->r_offset;
    case D3DPT_FB_REG_HZ:
        return s->r_hz;
    case D3DPT_FB_REG_FRAMES:
        return fb_vblank_count(s);
    case D3DPT_FB_REG_DDFLAGS:
        return s->ddflags;
    case D3DPT_FB_REG_CMD_OFFSET:
        return s->cmd_offset;
    case D3DPT_FB_REG_DOORBELL:
        return s->d3d_err;
    case D3DPT_FB_REG_D3D_STATUS:
        /* probing loads the executor: the answer must be honest */
        return d3d_load(s) ? D3DPT_STATUS_READY : D3DPT_STATUS_NO_EXEC;
    default:
        if (addr >= D3DPT_FB_REG_PALETTE &&
            addr < D3DPT_FB_REG_PALETTE + 4 * D3DPT_FB_PALETTE_SIZE) {
            return s->pal[(addr - D3DPT_FB_REG_PALETTE) / 4];
        }
        if (addr >= D3DPT_FB_REG_GAMMA &&
            addr < D3DPT_FB_REG_GAMMA + 4 * D3DPT_FB_GAMMA_SIZE) {
            return s->gamma[(addr - D3DPT_FB_REG_GAMMA) / 4];
        }
        return 0;
    }
}

static void d3dpt_vga_regs_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    D3dptVgaState *s = opaque;

    switch (addr) {
    case D3DPT_FB_REG_MODE_SEL:
        s->r_sel = val;
        break;
    case D3DPT_FB_REG_ENABLE:
        if ((val != 0) != (s->r_enable != 0)) {
            info_report("d3dpt-vga: linear mode %s (%ux%ux%u pitch %u offset %u, %u Hz)",
                        val ? "on" : "off", s->r_w, s->r_h, s->r_bpp,
                        s->r_pitch, s->r_offset, s->r_hz);
        }
        if (!val && s->r_enable) {
            s->vga_grace_until = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                                 D3DPT_FB_VGA_GRACE_MS;
        }
        s->r_enable = val != 0;
        s->vbl_ns = s->r_enable ? qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : 0;
        s->cur_flip_hidden = false;     /* a mode set: the flip chain is gone */
        timer_del(s->cur_flip_timer);
        if (s->cur_defined && s->cur_on) {
            fb_cursor_move(s);      /* the sprite follows the linear mode */
        }
        qemu_console_hw_invalidate(s->vga.con);
        break;
    case D3DPT_FB_REG_WIDTH:
        s->r_w = val;
        break;
    case D3DPT_FB_REG_HEIGHT:
        s->r_h = val;
        break;
    case D3DPT_FB_REG_BPP:
        s->r_bpp = val;
        break;
    case D3DPT_FB_REG_PITCH:
        s->r_pitch = val;
        break;
    case D3DPT_FB_REG_OFFSET:
        if (val != s->r_offset && s->flips < 4) {
            /* DirectDraw page flips: the first few, so the log shows them */
            info_report("d3dpt-vga: scanout offset %u -> %" PRIu64, s->r_offset, val);
        }
        if (val != s->r_offset) {
            s->flips++;
            fb_flip_rate(s);
            if (s->r_enable) {
                if (!s->cur_flip_hidden) {
                    s->cur_flip_hidden = true;
                    s->cur_flip_desktop = s->r_offset;  /* the page GDI was on */
                    s->r_offset = val;
                    if (s->cur_defined && s->cur_on) {
                        fb_cursor_move(s);  /* a flip chain has the screen */
                    }
                }
                timer_mod(s->cur_flip_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + D3DPT_FB_FLIP_IDLE_MS);
            }
        }
        s->r_offset = val;
        break;
    case D3DPT_FB_REG_HZ:
        s->r_hz = val;
        break;
    case D3DPT_FB_REG_DEBUG: {
        char c = (char)val;
        if (c == '\n' || s->dbg_len == sizeof(s->dbg) - 1) {
            s->dbg[s->dbg_len] = 0;
            info_report("d3dpt-vga: guest: %s", s->dbg);
            s->dbg_len = 0;
            if (c == '\n') {
                break;
            }
        }
        if (c != '\r') {
            s->dbg[s->dbg_len++] = c;
        }
        break;
    }
    case D3DPT_FB_REG_DOORBELL:
        d3d_doorbell(s);
        break;
    case D3DPT_FB_REG_CURSOR_ADDR:
        s->cur_addr = val;
        break;
    case D3DPT_FB_REG_CURSOR_W:
        s->cur_w = val;
        break;
    case D3DPT_FB_REG_CURSOR_H:
        s->cur_h = val;
        break;
    case D3DPT_FB_REG_CURSOR_HOT_X:
        s->cur_hot_x = val;
        break;
    case D3DPT_FB_REG_CURSOR_HOT_Y:
        s->cur_hot_y = val;
        break;
    case D3DPT_FB_REG_CURSOR_DEFINE:
        fb_cursor_define(s, val != 0);
        break;
    case D3DPT_FB_REG_CURSOR_X:
        s->cur_x = (int32_t)val;
        break;
    case D3DPT_FB_REG_CURSOR_Y:
        s->cur_y = (int32_t)val;
        fb_cursor_move(s);
        break;
    case D3DPT_FB_REG_CURSOR_ENABLE:
        s->cur_on = val != 0;
        fb_cursor_move(s);
        break;
    case D3DPT_FB_REG_GAMMA_ENABLE:
        s->gamma_on = val != 0;
        fb_gamma_apply(s);
        break;
    case D3DPT_FB_REG_IRQ_ENABLE:
        fb_irq_enable(s, val);
        break;
    case D3DPT_FB_REG_IRQ_STATUS:
        s->irq_status &= ~(uint32_t)val;
        fb_irq_update(s);
        break;
    case D3DPT_FB_REG_DMA_ADDR_LO:
        s->dma_lo = val;
        break;
    case D3DPT_FB_REG_DMA_ADDR_HI:
        s->dma_hi = val;
        break;
    case D3DPT_FB_REG_DMA_BYTES:
        s->dma_bytes = val;
        break;
    case D3DPT_FB_REG_DMA_APPEND:
        s->dma_status = d3d_dma_append(s, val);
        break;
    case D3DPT_FB_REG_FENCE:
        s->fence_done = val;
        if (s->irq_enable & D3DPT_FB_IRQ_DMA) {
            s->irq_status |= D3DPT_FB_IRQ_DMA;
            fb_irq_update(s);
        }
        break;
    default:
        if (addr >= D3DPT_FB_REG_PALETTE &&
            addr < D3DPT_FB_REG_PALETTE + 4 * D3DPT_FB_PALETTE_SIZE) {
            s->pal[(addr - D3DPT_FB_REG_PALETTE) / 4] = val & 0xffffffu;
            s->pal_dirty = true;
        }
        if (addr >= D3DPT_FB_REG_GAMMA &&
            addr < D3DPT_FB_REG_GAMMA + 4 * D3DPT_FB_GAMMA_SIZE) {
            s->gamma[(addr - D3DPT_FB_REG_GAMMA) / 4] = val & 0xffffffu;   /* applied at GAMMA_ENABLE */
        }
        break;
    }
}

static const MemoryRegionOps d3dpt_vga_regs_ops = {
    .read = d3dpt_vga_regs_read,
    .write = d3dpt_vga_regs_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

/* ---------------------------------------------------------------- device */

static void d3dpt_vga_realize(PCIDevice *dev, Error **errp)
{
    D3dptVgaState *s = D3DPT_VGA(dev);
    VGACommonState *vga = &s->vga;

    /* no-exec=on, d3d9= and exec= before the library is chosen, and the
     * library chosen here rather than at the first D3D_STATUS read: the
     * out-of-process executor (M15) wants VRAM as a region of its shared
     * file, and VRAM is made below. The command window is made either
     * way: a host below the Vulkan 1.3 floor has the adapter too. */
    if (s->no_exec) {
        d3dpt_exec_refuse();
    }
    d3dpt_exec_prefer(s->d3d9);
    d3dpt_exec_prefer_backend(s->exec_pick);
    s->lib = d3dpt_exec_lib();
#ifdef CONFIG_POSIX
    /* Only the out-of-process executor shares VRAM, and that is Wine on a
     * Linux or macOS host (ADR-018): the fd and the memory API that maps
     * one as guest RAM are both POSIX. A Windows host below the Vulkan
     * floor runs the executor in process on the system's own Direct3D 9
     * (ADR-007's second amendment), so there is no library with
     * shared_alloc there and the branch is compiled out. */
    if (s->lib && s->lib->shared_alloc) {
        uint64_t size = (uint64_t)vga->vram_size_mb << 20, off;
        int fd = s->lib->shared_alloc("d3dpt-vga.vram", size, &off);
        if (fd >= 0) {
            /* the VGA core takes a region its owner backed already (patch 73) */
            if (!memory_region_init_ram_from_fd(&vga->vram, OBJECT(dev), "vga.vram", size,
                                                RAM_SHARED, fd, off, errp)) {
                return;
            }
            s->lib->shared_map(off, memory_region_get_ram_ptr(&vga->vram));
        }
    }
#endif
    if (!vga_common_init(vga, OBJECT(dev), errp)) {
        return;
    }
    vga_init(vga, OBJECT(dev), pci_address_space(dev),
             pci_address_space_io(dev), true);
    /* one console; the VGA core's ops run through ours while ENABLE is 0 */
    vga->con = qemu_graphic_console_create(DEVICE(dev), 0, &d3dpt_vga_gfx_ops, s);
    s->cur_flip_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, fb_cursor_flip_idle, s);

    memory_region_init_io(&s->regs, OBJECT(dev), &d3dpt_vga_regs_ops, s,
                          "d3dpt-vga.regs", D3DPT_FB_REGS_SIZE);
    s->indexed = g_new0(pixman_indexed_t, 1);
    s->indexed->color = true;

    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &vga->vram);
    pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->regs);
    if (s->irq) {
        dev->config[PCI_INTERRUPT_PIN] = 1;
    }
    s->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fb_vbl_tick, s);

    /* the command window takes the top 64 MiB when at least as much is
     * left below it for the frame buffer and the DirectDraw heap */
    if (vga->vram_size >= 2 * D3DPT_SHM_SIZE) {
        s->cmd_offset = vga->vram_size - D3DPT_SHM_SIZE;
    } else {
        warn_report("d3dpt-vga: vgamem_mb=%u leaves no room for the Direct3D window (needs 128)",
                    vga->vram_size_mb);
    }
}

static void d3dpt_vga_reset(DeviceState *dev)
{
    D3dptVgaState *s = D3DPT_VGA(PCI_DEVICE(dev));

    /* the VGA core registers its own reset; ours goes back to VGA text */
    s->r_enable = s->r_w = s->r_h = s->r_bpp = s->r_pitch = 0;
    s->r_offset = s->r_hz = s->r_sel = 0;
    s->vbl_ns = 0;
    s->cur_flip_hidden = false;
    timer_del(s->cur_flip_timer);
    s->flips = s->flips_last = 0;
    s->flips_ns = 0;
    s->vga_grace_until = 0;
    s->dbg_len = 0;
    if (s->cur_on || s->cur_defined) {
        /* a rebooted guest has no pointer until its driver defines one */
        fb_cursor_clear(s);
    }
    s->gamma_on = false;
    fb_gamma_apply(s);     /* no ramp after a reset: a RAMDAC comes up linear */
    s->irq_status = 0;
    fb_irq_enable(s, 0);   /* no interrupt until the driver asks again */
    s->dma_lo = s->dma_hi = s->dma_bytes = s->dma_status = 0;
    s->fence_done = 0;
    d3d_reset(s);
}

static const Property d3dpt_vga_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", D3dptVgaState, vga.vram_size_mb, D3DPT_FB_VRAM_MB),
    DEFINE_PROP_BOOL("global-vmstate", D3dptVgaState, vga.global_vmstate, false),
    DEFINE_PROP_UINT32("ddflags", D3dptVgaState, ddflags, 0),
    /* no-exec=on: this host is below ADR-013's Vulkan 1.3 floor. Every
     * d3dpt device then reports D3DPT_STATUS_NO_EXEC, the display driver
     * keeps its DirectDraw half and offers no Direct3D, and a game falls
     * back the way it does on such a host (the runtime's software
     * device). Testing knob only: it is how a host
     * we cannot borrow is met from one that has Vulkan. It refuses before
     * the executor library is opened, so `d3d9=` below is not read and no
     * backend is picked: a *Windows* host below the floor
     * is `d3d9=system`, and this is a host with no pass-through at all. */
    DEFINE_PROP_BOOL("no-exec", D3dptVgaState, no_exec, false),
    /* d3d9=auto|dxvk|system: which Direct3D 9 the executor runs on
     * (d3dpt_exec_load.h). `system` is Windows' own, the fallback for a
     * host below the Vulkan 1.3 floor and, on a host that has both, the
     * A/B between the two rasterisers. Nothing here refuses `system` on
     * Linux or macOS: the executor answers that, and says so. */
    DEFINE_PROP_STRING("d3d9", D3dptVgaState, d3d9),
    /* exec=auto|dxvk|wine|none: which executor library (d3dpt_exec_load.h).
     * `wine` is the executor in another process on Wine's own Direct3D 9,
     * a Linux or macOS host's fallback below the Vulkan 1.3 floor (ADR-018,
     * docs/tracks/m15-wine-executor.md) and, on a host that has both, the
     * A/B; `auto` takes it only when DXVK finds no device. */
    DEFINE_PROP_STRING("exec", D3dptVgaState, exec_pick),
    /* the register set version the adapter reports: a newer one than the
     * device implements is the check that an installed driver accepts a
     * QEMU update (d3dpt_fb.h, "Versions only add"). Nothing else reads it. */
    DEFINE_PROP_UINT32("fb-version", D3dptVgaState, fb_version, D3DPT_FB_VERSION),
    /* convert the whole frame every refresh instead of the dirty spans:
     * the A/B for a picture that comes out in stale bands (a
     * 3DMark 99 loading screen on the PC). On means the pixels in VRAM are
     * right and this device's incremental path is what lost them. */
    DEFINE_PROP_BOOL("full-frames", D3dptVgaState, full_frames, false),
    /* an interrupt pin for the WDDM driver (M18, Windows 7) and the IRQ
     * registers behind it (register set v6: the vertical blank) */
    DEFINE_PROP_BOOL("irq", D3dptVgaState, irq, false),
};

static void d3dpt_vga_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = d3dpt_vga_realize;
    k->romfile = "vgabios-stdvga.bin";
    k->vendor_id = D3DPT_FB_PCI_VENDOR;
    k->device_id = D3DPT_FB_PCI_DEVICE;
    k->subsystem_vendor_id = D3DPT_FB_PCI_VENDOR;
    k->subsystem_id = D3DPT_FB_PCI_DEVICE;
    k->class_id = PCI_CLASS_DISPLAY_VGA;
    dc->desc = "paravirtual framebuffer VGA (2ksbox, doc 15)";
    dc->hotpluggable = false;
    device_class_set_props(dc, d3dpt_vga_properties);
    device_class_set_legacy_reset(dc, d3dpt_vga_reset);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo d3dpt_vga_info = {
    .name = TYPE_D3DPT_VGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(D3dptVgaState),
    .class_init = d3dpt_vga_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void d3dpt_vga_register_types(void)
{
    type_register_static(&d3dpt_vga_info);
}

type_init(d3dpt_vga_register_types)
