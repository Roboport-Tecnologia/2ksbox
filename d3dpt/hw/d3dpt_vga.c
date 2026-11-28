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
#include "qemu/thread.h"
#include "qemu/main-loop.h"
#include "qemu/rcu.h"
#include "qemu/atomic.h"
#include "system/tcg.h"
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
    uint32_t rgb;               /* 32 bpp, x8b8g8r8 (SCAN_RGB, register set v17) */
} D3dptLinearMode;

struct D3dptVgaState {
    PCIDevice dev;
    VGACommonState vga;
    MemoryRegion regs;
    MemoryRegion dma1;          /* DMA_APPEND1 alone, BQL-free (register set v14, d3dpt_dma1_write) */

    /* guest-programmed registers */
    uint32_t r_enable, r_w, r_h, r_bpp, r_pitch, r_offset, r_hz, r_sel;
    uint32_t r_blank;           /* BLANK (register set v13): the source is off */
    uint32_t r_scan_rgb;        /* SCAN_RGB (register set v17): 32 bpp pixels are x8b8g8r8 */
    bool blank_shown;           /* the black surface is on the console */
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
    /* register set v12 (CAP_FOLLOW, track M20): the desktop follows the
     * window. follow is the property; follow_on what the driver wrote to
     * FOLLOW, while which the console takes UI info (d3dpt_vga_follow_ops);
     * pref_w / pref_h the window's size from it */
    bool follow;
    bool follow_on;
    uint32_t pref_w, pref_h;
    QEMUTimer *vbl_timer;       /* the vertical blank interrupt's, while IRQ_VBLANK is enabled */
    int64_t vbl_next_ns;        /* its next deadline (QEMU_CLOCK_VIRTUAL) */
    bool host_vblank;           /* property: IRQ_VBLANK on the host screen's blank */
    bool bar64;                 /* property: VRAM a 64-bit BAR 0, the registers BAR 2 */
    Notifier host_vbl;          /* qemu_host_vblank's, with host_vblank */
    uint32_t host_vbls;         /* host blanks raised since IRQ_VBLANK went on */
    uint32_t watchdog_vbls;     /* and the timer's, while host blanks had come */
    uint32_t dma_lo, dma_hi, dma_bytes, dma_status; /* DMA_* (register set v7) */
    uint32_t fence_done;        /* FENCE_DONE */
    uint64_t dma_appends, dma_appended; /* appends and their bytes, for the rate line */
    uint64_t dma_ns, dma_copy_ns;       /* ... their time in the handler, and in the copy alone */
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
    /* the VRAM fill (version 9): its registers and the last one's status */
    uint32_t fill_addr, fill_bytes, fill_pattern, fill_status;
    /* the VRAM <-> guest pages copy (version 9) */
    uint32_t copy_vram, copy_bytes, copy_list_off, copy_status;
    uint32_t copy_rows, copy_vpitch, copy_ppitch, copy_src, copy_count;
    uint64_t copy_list;

    /* the hardware cursor (version 4): the guest's registers, and what was
     * defined / shown so far for the log */
    uint32_t cur_addr, cur_w, cur_h, cur_hot_x, cur_hot_y;
    int32_t cur_x, cur_y;
    bool cur_on, cur_defined;
    uint32_t cur_defines, cur_moves;
    bool cur_shown;             /* the visibility last published */
    bool cur_flip_hidden;       /* a flip chain has the screen: see fb_cursor_move */
    uint32_t cur_flags;         /* CURSOR_FLAGS (v8): CURSOR_OWNED turns the flip hiding off */
    uint32_t cur_flip_desktop;  /* the scanout offset before the chain's first flip */
    QEMUTimer *cur_flip_timer;  /* no flip for D3DPT_FB_FLIP_IDLE_MS: see fb_cursor_flip_idle */
    QEMUTimer *settle_timer;    /* a flip-push waiting for the executor's readback (fb_settled) */
    int push_tries;             /* its retries so far */
    int gfx_skips;              /* refreshes skipped in a row for the same */
    uint32_t cur_flip_idles;    /* chains ended that way, for the log */
    uint32_t cur_flips;         /* show / hide changes logged so far */

    /* the linear mode currently shown (lin_on) */
    bool lin_on;
    bool full_update;
    bool full_frames;           /* property: every refresh a whole frame */
    bool flip_push;             /* property: a flip copies the page it shows and pushes it */
    uint32_t flip_pushes;       /* flips pushed so far, for the log */
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
    int64_t dbg_second;         /* the realtime second the lines below count */
    unsigned dbg_lines, dbg_dropped;

    /* M7c: the command window at the top of VRAM and its executor */
    uint32_t cmd_offset;        /* 0 = VRAM too small for a window */
    const D3dptExecLib *lib;
    d3dpt_exec_t *exec;
    bool d3d11;                 /* the executor runs Direct3D 11 (CAP_D3D11, M25) */
    uint32_t msaa;              /* register set v16: the host's multisample counts (D3DPT_EXEC_FEAT_MSAA), and the
                                 * property's mask of them (msaa=, all by default) */
    uint32_t msaa_mask;
    bool exec_tried;
    uint32_t d3d_err;           /* last D3DPT_ERR_* of the doorbell */
    QemuMutex exec_lock;        /* the executor's calls: a batch runs without the BQL (d3d_doorbell) */
    /* register set v15 (async, M25): batches the WDDM driver kicked, run by
     * a worker thread in order (d3d_async_*); async_lock guards the queue */
    bool async;                 /* property: CAP_ASYNC offered */
    bool async_started;
    QemuThread async_thread;
    QemuMutex async_lock;
    QemuCond async_cond;        /* the queue changed (work in, a slot free, idle) */
    uint8_t *async_buf[3];      /* window-shaped copies of kicked batches */
    uint32_t async_fence[3];
    unsigned async_head, async_n;   /* the oldest queued slot, how many are queued */
    bool async_busy;            /* the worker is running the head */
    uint64_t async_kicks, async_waits, async_wait_ns; /* for the rate line: kicks, those that found no free slot */
    QEMUBH *async_bh;           /* raises IRQ_DMA under the BQL after a completion */
    /* DMA_APPEND1's appends not copied yet (pend_lock): where in the
     * window's records each goes, and the guest memory it is in. Copied
     * by the worker for a kicked batch, into the window at a doorbell */
    QemuMutex pend_lock;
    struct D3dChunk { uint32_t at, bytes, count; uint64_t gpa; } pend[64], slot_chunk[3][64];
    unsigned pend_n, slot_nchunk[3];
    uint32_t pend_bytes, pend_count;
    uint32_t slot_win_bytes[3], slot_win_count[3];
    uint64_t async_flip_next;   /* ASYNC_FLIP for the next kick, + 1 (0: none) */
    uint64_t slot_flip[3];      /* each queued batch's flip, + 1 */
    uint64_t bh_flip;           /* a ran batch's flip for the bottom half, + 1 (pend_lock) */
    uint32_t bh_fence;          /* ... and its fence */
    /* the scanout ring (track M26): the front end's images on the host's
     * GPU, which the executor copies a flip's frame into instead of
     * reading it back, and which the console shows by number */
    bool zero_copy;             /* property */
    int ring_n;                 /* images in the ring (0: none) */
    uint32_t ring_slots;        /* property: how many to ask for (2 to 8) */
    uint32_t ring_w, ring_h;
    bool ring_refused;          /* this size could not have one: not asked again */
    bool shadow_stale;          /* a slot had the screen since the shadow last did */
    uint32_t ring_shows;        /* slots shown, for the log */
    uint32_t ring_declines;     /* slots passed over for what the CPU drew, for the log */
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
    m->rgb = s->r_bpp == 32 && s->r_scan_rgb;
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

/* The VRAM view the shadow is converted from: the page at m->offset. */
static void fb_make_src(D3dptVgaState *s, const D3dptLinearMode *m)
{
    uint8_t *ptr = memory_region_get_ram_ptr(&s->vga.vram) + m->offset;

    if (s->src) {
        qemu_pixman_image_unref(s->src);
    }
    if (m->bpp == 32) {
        /* a gamma ramp on (version 5), or flip-push: a copy; an x8b8g8r8
         * page (SCAN_RGB) converted into the shadow's x8r8g8b8 */
        s->src = pixman_image_create_bits(m->rgb ? PIXMAN_x8b8g8r8 : PIXMAN_x8r8g8b8, m->w, m->h,
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
}

/* A 32 bpp mode is shown straight from VRAM only with flip-push off, no
 * gamma ramp and the console's own channel order; everything else goes
 * through the shadow. */
static bool fb_wants_shadow(D3dptVgaState *s, uint32_t bpp)
{
    return bpp != 32 || s->gamma_active || s->flip_push || s->r_scan_rgb;
}

static void fb_switch(D3dptVgaState *s, const D3dptLinearMode *m)
{
    uint8_t *ptr = memory_region_get_ram_ptr(&s->vga.vram) + m->offset;
    DisplaySurface *ds;

    fb_drop_shadow(s);
    if (!fb_wants_shadow(s, m->bpp)) {
        ds = qemu_create_displaysurface_from(m->w, m->h, PIXMAN_x8r8g8b8,
                                             m->pitch, ptr);
    } else {
        fb_make_src(s, m);
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

/* A VRAM fill (version 9, FILL_GO): the host's memset of the range, which
 * the vCPU makes at uncached speed through the BAR under WHPX (track M20). */
static uint32_t fb_fill(D3dptVgaState *s)
{
    uint64_t end = (uint64_t)s->fill_addr + s->fill_bytes;
    uint32_t *p, i, n = s->fill_bytes / 4;

    if ((s->fill_addr | s->fill_bytes) & 3 || end > s->vga.vram_size) {
        return D3DPT_FB_FILL_BAD;
    }
    p = (uint32_t *)(memory_region_get_ram_ptr(&s->vga.vram) + s->fill_addr);
    if ((s->fill_pattern & 0xff) * 0x01010101u == s->fill_pattern) {
        memset(p, s->fill_pattern & 0xff, s->fill_bytes);
    } else {
        for (i = 0; i < n; i++) {
            p[i] = s->fill_pattern;
        }
    }
    memory_region_set_dirty(&s->vga.vram, s->fill_addr, s->fill_bytes);
    return D3DPT_FB_FILL_OK;
}

/* A copy between VRAM and guest memory (version 9, COPY_GO): the pages
 * are the list's, one 64-bit guest-physical address each, the first entered
 * at LIST_OFF; the device reads and writes them itself, the vCPU's copy
 * through the BAR being uncached under WHPX (the WDDM driver's paging
 * transfers, track M20). */
/* a surface's rows at most (16384 is Direct3D's largest), with room: a
 * row costs a page-list read and a DMA under the BQL */
#define D3DPT_FB_COPY_ROWS_MAX 65536
static uint32_t fb_copy(D3dptVgaState *s, uint32_t dir)
{
    uint8_t *vram = memory_region_get_ram_ptr(&s->vga.vram);
    uint32_t rows = s->copy_rows ? s->copy_rows : 1, row;
    uint64_t span = (uint64_t)(rows - 1) * s->copy_vpitch + s->copy_bytes;

    if (dir < D3DPT_FB_COPY_TO_PAGES || dir > D3DPT_FB_COPY_VRAM_VRAM ||
        (uint64_t)s->copy_vram + span > s->vga.vram_size || s->copy_list_off >= 4096 ||
        (uint64_t)rows * s->copy_bytes > s->vga.vram_size || rows > D3DPT_FB_COPY_ROWS_MAX) {
        return D3DPT_FB_FILL_BAD;
    }
    if (!s->copy_bytes) {
        return D3DPT_FB_FILL_OK;    /* nothing to move: no 4-billion-row walk under the BQL */
    }
    if (dir == D3DPT_FB_COPY_VRAM_VRAM) {
        if ((uint64_t)s->copy_src + (uint64_t)(rows - 1) * s->copy_ppitch + s->copy_bytes > s->vga.vram_size) {
            return D3DPT_FB_FILL_BAD;
        }
        for (row = 0; row < rows; row++) {
            memmove(vram + s->copy_vram + (uint64_t)row * s->copy_vpitch,
                    vram + s->copy_src + (uint64_t)row * s->copy_ppitch, s->copy_bytes);
        }
    }
    /* the pages: every row's bytes inside the list the guest filled, or
     * nothing is copied (a list entry is a DMA address) */
    if (dir != D3DPT_FB_COPY_VRAM_VRAM &&
        ((uint64_t)s->copy_list_off + (uint64_t)(rows - 1) * s->copy_ppitch + s->copy_bytes + 4095) / 4096 >
            s->copy_count) {
        return D3DPT_FB_FILL_BAD;
    }
    for (row = 0; dir != D3DPT_FB_COPY_VRAM_VRAM && row < rows; row++) {
        /* this row's place in the pages: list_off + row * page pitch */
        uint64_t lin = s->copy_list_off + (uint64_t)row * s->copy_ppitch;
        uint8_t *v = vram + s->copy_vram + (uint64_t)row * s->copy_vpitch;
        uint32_t done = 0;

        while (done < s->copy_bytes) {
            uint64_t page, at = lin + done;
            uint32_t off = at & 4095, n = MIN(4096 - off, s->copy_bytes - done);
            MemTxResult r;

            if (pci_dma_read(&s->dev, s->copy_list + 8ull * (at >> 12), &page, 8) != MEMTX_OK) {
                return D3DPT_FB_FILL_BAD;
            }
            page = le64_to_cpu(page);
            if (dir == D3DPT_FB_COPY_TO_VRAM) {
                r = pci_dma_read(&s->dev, page + off, v + done, n);
            } else {
                r = pci_dma_write(&s->dev, page + off, v + done, n);
            }
            if (r != MEMTX_OK) {
                return D3DPT_FB_FILL_BAD;
            }
            done += n;
        }
    }
    if (dir != D3DPT_FB_COPY_TO_PAGES) {
        memory_region_set_dirty(&s->vga.vram, s->copy_vram, span);
    }
    return D3DPT_FB_FILL_OK;
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
    if (s->host_vbls) {
        s->watchdog_vbls++;
    }
    s->irq_status |= D3DPT_FB_IRQ_VBLANK;
    fb_irq_update(s);
    fb_vbl_arm(s);
}

/* The host screen's vertical blank (QEMU patch 90's qemu_host_vblank: the
 * player calls it on each host refresh that starts a guest frame, at the
 * rate it gave the guest, tracks M24 and M20). While they come, each one
 * raises IRQ_VBLANK, so the guest composes in step with the screen the
 * frame is shown on instead of sliding across it, and the timer above is
 * only a watchdog: pushed two periods out at every host blank, it takes
 * over when they stop (a minimized window, a headless run, a host screen
 * with no blank to wait on), as dxgkrnl must not miss blanks for long. */
static void fb_host_vblank(Notifier *n, void *data)
{
    D3dptVgaState *s = container_of(n, D3dptVgaState, host_vbl);
    int64_t now;

    if (!(s->irq_enable & D3DPT_FB_IRQ_VBLANK)) {
        return;
    }
    if (!s->host_vbls++) {
        info_report("d3dpt-vga: the vertical blank follows the host screen's");
    }
    s->irq_status |= D3DPT_FB_IRQ_VBLANK;
    fb_irq_update(s);
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->vbl_next_ns = now + 2 * (NANOSECONDS_PER_SECOND / fb_refresh_hz(s));
    timer_mod(s->vbl_timer, s->vbl_next_ns);
}

static void fb_irq_enable(D3dptVgaState *s, uint32_t val)
{
    uint32_t was = s->irq_enable;

    s->irq_enable = s->irq ? val & (D3DPT_FB_IRQ_VBLANK | D3DPT_FB_IRQ_DMA | (s->follow ? D3DPT_FB_IRQ_DISPLAY : 0)) : 0;
    s->irq_status &= s->irq_enable;         /* nothing stays pending that is off */
    if ((s->irq_enable & D3DPT_FB_IRQ_VBLANK) && !(was & D3DPT_FB_IRQ_VBLANK)) {
        s->vbl_next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        fb_vbl_arm(s);
    } else if (!(s->irq_enable & D3DPT_FB_IRQ_VBLANK)) {
        timer_del(s->vbl_timer);
        if (was & D3DPT_FB_IRQ_VBLANK && s->host_vbls) {
            info_report("d3dpt-vga: vertical blanks: %u the host's, %u the timer's",
                        s->host_vbls, s->watchdog_vbls);
        }
        s->host_vbls = s->watchdog_vbls = 0;
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
        info_report("d3dpt-vga: %" PRIu64 " DMA appends, %" PRIu64 " KiB of records, fence %u; %.1f ms in the handler, %.1f copying",
                    s->dma_appends, s->dma_appended >> 10, s->fence_done, s->dma_ns / 1e6, s->dma_copy_ns / 1e6);
        s->dma_appends = s->dma_appended = 0;
        s->dma_ns = s->dma_copy_ns = 0;
    }
    if (s->async_kicks) {
        info_report("d3dpt-vga: %" PRIu64 " batches kicked, %" PRIu64 " waited for a slot (%.1f ms)",
                    s->async_kicks, s->async_waits, s->async_wait_ns / 1e6);
        s->async_kicks = s->async_waits = s->async_wait_ns = 0;
    }
    s->batches_last = s->batches;
    s->exec_ns_last = s->exec_ns;
    s->flips_ns = now;
    s->flips_last = s->flips;
}

/* A flip's readback the executor may still owe VRAM (protocol v28,
 * RES_SCANOUT) goes there before anything shows VRAM. Waited for, not
 * tried: a page shown before it was paid was that buffer's frame from two
 * or three flips ago, everything that moved flickering with its own past
 * (track M25). A batch holding the lock gave up the BQL, so a wait waits
 * for the batch and not the other way round, but it is a vCPU or the main
 * loop held under the BQL: waited for at every DWM flip, 2-4 ms each (the
 * GPU finishing the composition) stalled the whole guest ~20% of the time.
 * So it is tried first (`wait` false: a busy lock or a readback the GPU is
 * not done with returns false) and the caller comes back. */
#define D3DPT_SETTLE_TRIES 20   /* 1 ms apart, then waited for */

static bool fb_settled(D3dptVgaState *s, bool wait)
{
    int owed;

    if (!s->exec || !s->lib->settle) {
        return true;
    }
    if (wait) {
        qemu_mutex_lock(&s->exec_lock);
    } else if (qemu_mutex_trylock(&s->exec_lock) != 0) {
        return false;
    }
    owed = s->lib->settle(s->exec, wait);
    qemu_mutex_unlock(&s->exec_lock);
    return owed == 0;
}

/* ----------------------------------------------------- the scanout ring
 *
 * Track M26. A 32 bpp page a flip shows is usually a frame the executor
 * rendered on the host's GPU (DWM's composition, a game's flip chain),
 * and reading it back into VRAM so the front end could copy it and upload
 * it again was the whole cost of a big screen (33 MB a frame at
 * 3840x2160). With a ring the executor copies it into one of the front
 * end's images on the GPU (d3dpt_exec_ring) and the console shows that
 * image (QEMU patch 101's qemu_scanout_ring); VRAM and the shadow keep
 * an older picture, `shadow_stale`, until something must show or read
 * them (the CPU drew on the page, a screendump), which reads the page
 * back first (d3dpt_exec_scanout_vram). */
#define D3DPT_RING_SLOTS_MAX 8

static void fb_ring_drop(D3dptVgaState *s)
{
    const QemuScanoutRingOps *ops = qemu_scanout_ring();

    if (!s->ring_n) {
        return;
    }
    qemu_mutex_lock(&s->exec_lock);
    s->lib->ring(s->exec, NULL, 0, 0, 0);
    qemu_mutex_unlock(&s->exec_lock);
    if (ops) {
        ops->alloc(s->vga.con, 0, 0, 0, NULL);
    }
    s->ring_n = 0;
}

/* A ring of the mode's size, made once the executor is there: true when
 * flips can go through it */
static bool fb_ring_ready(D3dptVgaState *s, const D3dptLinearMode *m)
{
    const QemuScanoutRingOps *ops = qemu_scanout_ring();
    void *handles[D3DPT_RING_SLOTS_MAX];
    int want = MAX(2, MIN((int)s->ring_slots, D3DPT_RING_SLOTS_MAX));
    int n, took;

    if (!s->zero_copy || !s->exec || !s->lib->ring || !s->lib->ring_slot ||
        !ops || m->bpp != 32 || s->gamma_active || s->full_frames) {
        fb_ring_drop(s);
        return false;
    }
    if (s->ring_n && s->ring_w == m->w && s->ring_h == m->h) {
        return true;
    }
    if (s->ring_refused && s->ring_w == m->w && s->ring_h == m->h) {
        return false;
    }
    fb_ring_drop(s);
    s->ring_w = m->w;
    s->ring_h = m->h;
    n = ops->alloc(s->vga.con, m->w, m->h, want, handles);
    took = 0;
    if (n > 0) {
        qemu_mutex_lock(&s->exec_lock);
        took = s->lib->ring(s->exec, handles, n, m->w, m->h);
        qemu_mutex_unlock(&s->exec_lock);
    }
    if (took <= 0 || took != n) {
        if (n > 0) {
            ops->alloc(s->vga.con, 0, 0, 0, NULL);
        }
        s->ring_refused = true;
        info_report("d3dpt-vga: no scanout ring at %ux%u (%s): flips are read back",
                    m->w, m->h, n > 0 ? "the executor could not take it" : "the front end made none");
        return false;
    }
    s->ring_refused = false;
    s->ring_n = n;
    info_report("d3dpt-vga: scanout ring of %d at %ux%u: flips stay on the GPU", n, m->w, m->h);
    return true;
}

/* The flip's page from a ring slot: 1 shown, 0 not in one (VRAM has it),
 * -1 its copy not finished (and `wait` 0) */
static int fb_ring_show(D3dptVgaState *s, const D3dptLinearMode *m, bool wait)
{
    int slot;

    if (!fb_ring_ready(s, m)) {
        return 0;
    }
    if (wait) {
        qemu_mutex_lock(&s->exec_lock);
    } else if (qemu_mutex_trylock(&s->exec_lock) != 0) {
        return -1;
    }
    slot = s->lib->ring_slot(s->exec, m->offset, wait);
    qemu_mutex_unlock(&s->exec_lock);
    if (slot == -2) {
        return -1;
    }
    if (slot < 0) {
        return 0;
    }
    /* The guest's CPU wrote the page since the copy (a title's text through
     * GDI over its Direct3D 9 frame, which no READBACK precedes): the slot
     * lacks it. The frame comes into VRAM with those pixels kept over it
     * (the executor's readback merges them) and shows from there. */
    {
        DirtyBitmapSnapshot *snap =
            memory_region_snapshot_and_clear_dirty(&s->vga.vram, m->offset,
                                                   (uint64_t)m->pitch * m->h,
                                                   DIRTY_MEMORY_VGA);
        bool drew = memory_region_snapshot_get_dirty(&s->vga.vram, snap, m->offset,
                                                     (uint64_t)m->pitch * m->h);
        g_free(snap);
        if (drew) {
            if (s->ring_declines++ < 8) {
                info_report("d3dpt-vga: the guest drew on a flipped page (offset %u): "
                            "shown from VRAM, not its ring slot", m->offset);
            }
            if (s->lib->scanout_vram) {
                qemu_mutex_lock(&s->exec_lock);
                s->lib->scanout_vram(s->exec, m->offset);
                qemu_mutex_unlock(&s->exec_lock);
            }
            return 0;
        }
    }
    /* Shown under the executor's lock, so the front end holds the slot
     * (embed v18) before the next batch reads what it holds and no frame
     * goes into it while it may be sampled; the slot asked again there, as
     * a batch since the first ask may have put another frame into it.
     * Only the ask and the show: the dirty snapshot above may wait for the
     * vCPU (with the BQL let go), whose next flip waits for this lock. */
    if (wait) {
        qemu_mutex_lock(&s->exec_lock);
    } else if (qemu_mutex_trylock(&s->exec_lock) != 0) {
        return -1;
    }
    if (s->lib->ring_slot(s->exec, m->offset, false) != slot) {
        qemu_mutex_unlock(&s->exec_lock);
        return -1;
    }
    s->shadow_stale = true;
    qemu_scanout_ring()->show(s->vga.con, slot);
    qemu_mutex_unlock(&s->exec_lock);
    if (!s->ring_shows++) {
        info_report("d3dpt-vga: flips show their ring slot (zero-copy)");
    }
    return 1;
}

/* The shadow has the screen again: the page's frame into VRAM first if a
 * slot holds it */
static void fb_ring_unstale(D3dptVgaState *s, uint32_t offset)
{
    if (!s->shadow_stale) {
        return;
    }
    s->shadow_stale = false;
    if (s->ring_n && s->lib->scanout_vram) {
        qemu_mutex_lock(&s->exec_lock);
        s->lib->scanout_vram(s->exec, offset);
        qemu_mutex_unlock(&s->exec_lock);
    }
}

/* The source the driver turned off (BLANK, register set v13): the last
 * frame held for the grace, then a black surface of the mode's size, made
 * once; the linear mode is put back whole when BLANK goes to 0 */
static void fb_show_blank(D3dptVgaState *s)
{
    DisplaySurface *ds;
    int w = s->r_w >= 64 && s->r_w <= 8192 ? s->r_w : 640;
    int h = s->r_h >= 64 && s->r_h <= 8192 ? s->r_h : 480;

    if (s->blank_shown && !s->resurface) {
        return;
    }
    if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) < s->vga_grace_until) {
        return;
    }
    s->lin_on = false;
    fb_drop_shadow(s);
    ds = qemu_create_displaysurface(w, h);
    memset(surface_data(ds), 0, (size_t)surface_stride(ds) * h);
    qemu_console_set_surface(s->vga.con, ds);
    qemu_console_update_full(s->vga.con);
    s->blank_shown = true;
    s->resurface = false;
}

static bool d3dpt_vga_gfx_update(void *opaque)
{
    D3dptVgaState *s = opaque;
    D3dptLinearMode m;
    DirtyBitmapSnapshot *snap;
    int y, ys;

    if (s->r_blank && s->r_enable) {
        fb_show_blank(s);
        return true;
    }
    s->blank_shown = false;
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

    /* VRAM not current yet: this refresh shows nothing new (the flip-push
     * or the next tick will), a few in a row and then it waits */
    if (!fb_settled(s, s->gfx_skips >= 3)) {
        s->gfx_skips++;
        return true;
    }
    s->gfx_skips = 0;
    if (s->gamma_dirty) {
        /* a new ramp recolours every pixel; a 32 bpp mode moves between
         * VRAM itself and the shadow the ramp is applied in */
        s->gamma_dirty = false;
        if (s->lin_on && m.bpp == 32 && (s->shadow != NULL) != fb_wants_shadow(s, 32)) {
            s->lin_on = false;
        }
        s->full_update = true;
    }
    if (s->lin_on && !s->resurface && s->shadow && m.offset != s->lin.offset &&
        m.w == s->lin.w && m.h == s->lin.h && m.bpp == s->lin.bpp && m.pitch == s->lin.pitch &&
        m.rgb == s->lin.rgb) {
        /* a page flip the push did not take: the same shadow, the new page;
         * from its ring slot if one holds it (M26), its copy not finished
         * yet left to the next refresh */
        int shown = fb_ring_show(s, &m, false);
        if (shown != 0) {
            if (shown == 1) {
                fb_make_src(s, &m);
                s->lin = m;
            }
            return true;
        }
        fb_make_src(s, &m);
        s->lin = m;
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
    if (s->shadow_stale) {
        /* a ring slot has the screen (M26): the surface takes it back only
         * for something to show (the CPU drew on the page, a repaint of the
         * whole, a screendump: qemu_console_pixels_wanted), the page's
         * frame read back into VRAM first */
        if (!s->full_update && !qemu_console_pixels_wanted()) {
            snap = memory_region_snapshot_and_clear_dirty(&s->vga.vram, m.offset,
                                                          (uint64_t)m.pitch * m.h,
                                                          DIRTY_MEMORY_VGA);
            bool drew = memory_region_snapshot_get_dirty(&s->vga.vram, snap, m.offset,
                                                         (uint64_t)m.pitch * m.h);
            g_free(snap);
            if (!drew) {
                return true;
            }
        }
        fb_ring_unstale(s, m.offset);
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

/* The window's size, while the driver follows it (FOLLOW, register set
 * v12): QEMU's console calls this once a resize has settled (a second
 * after the last), and the driver hears IRQ_DISPLAY. The refresh rate in
 * the same info is HOST_MHZ's business (patch 96), not a change here. */
static void d3dpt_vga_ui_info(void *opaque, uint32_t head, QemuUIInfo *info)
{
    D3dptVgaState *s = opaque;

    if (!info->width || !info->height || (info->width == s->pref_w && info->height == s->pref_h)) {
        return;
    }
    s->pref_w = info->width;
    s->pref_h = info->height;
    info_report("d3dpt-vga: the window is %ux%u", s->pref_w, s->pref_h);
    if (s->follow_on && (s->irq_enable & D3DPT_FB_IRQ_DISPLAY)) {
        s->irq_status |= D3DPT_FB_IRQ_DISPLAY;
        fb_irq_update(s);
    }
}

/* The same with UI info: the console a front end makes follow its window
 * (qemu_console_ui_info_supported). Only while the driver says it applies
 * the size, so a 98 / XP / 7 guest's screen keeps its own */
static const GraphicHwOps d3dpt_vga_follow_ops = {
    .invalidate  = d3dpt_vga_invalidate,
    .gfx_update  = d3dpt_vga_gfx_update,
    .text_update = d3dpt_vga_text_update,
    .ui_info     = d3dpt_vga_ui_info,
};

static void fb_follow(D3dptVgaState *s, bool on)
{
    on = on && s->follow && s->irq;
    if (on != s->follow_on) {
        s->follow_on = on;
        qemu_graphic_console_set_hwops(s->vga.con, on ? &d3dpt_vga_follow_ops : &d3dpt_vga_gfx_ops, s);
        info_report("d3dpt-vga: the desktop %s the window", on ? "follows" : "no longer follows");
        /* a size heard before (a rebooted guest): the console's UI info is
         * the same, so QEMU calls d3dpt_vga_ui_info no more; say it now */
        if (on && s->pref_w && (s->irq_enable & D3DPT_FB_IRQ_DISPLAY)) {
            s->irq_status |= D3DPT_FB_IRQ_DISPLAY;
            fb_irq_update(s);
        }
    }
}

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
    /* register set v10: Direct3D 11 when the executor opens it (M25) */
    uint32_t feat = s->lib->features ? s->lib->features(s->exec) : 0;
    s->d3d11 = (feat & D3DPT_EXEC_FEAT_D3D11) != 0;
    s->msaa = s->d3d11 ? ((feat & D3DPT_EXEC_FEAT_MSAA_MASK) >> D3DPT_EXEC_FEAT_MSAA_SHIFT) & s->msaa_mask : 0;
    info_report("d3dpt-vga: Direct3D executor ready, window at %u MiB%s (multisampling 0x%02x)", s->cmd_offset >> 20,
                s->d3d11 ? ", Direct3D 11 too" : "", s->msaa);
    return true;
}

/* ------------------------------------------------- async (register set v15)
 *
 * ASYNC_KICK takes the window's batch with the submission's fence and
 * returns at once: the batch is copied into one of three host buffers,
 * the window starts empty again for the guest's next submission, and a
 * worker thread runs the batches in order under exec_lock, then makes
 * FENCE_DONE the fence (never lower) and raises IRQ_DMA through a bottom
 * half. The guest's GPU scheduler no longer waits for the host inside
 * every submission (Heaven: the doorbell was ~35% of that thread, track
 * M25). Anything that must see the host done first drains: a synchronous
 * doorbell, ASYNC_DRAIN (the driver writes it before every submission it
 * does not kick, and at a preemption), a reset. */
/* the records of a batch put together in `out` (D3DPT_CMD_SIZE at most):
 * the window's own records `win` (bytes, count) with the appended chunks
 * at their places, each read from guest RAM now. Returns the bytes, and
 * the count in *count; a chunk that is no longer RAM ends it (the
 * executor then refuses the count) */
static uint32_t d3d_assemble(D3dptVgaState *s, uint8_t *out, const uint8_t *win, uint32_t win_bytes,
                             const struct D3dChunk *c, unsigned n, uint32_t *count)
{
    uint32_t o = 0, w = 0;
    unsigned i;

    for (i = 0; i <= n; i++) {
        uint32_t upto = i < n ? MIN(c[i].at, win_bytes) : win_bytes;

        if (upto > w) {
            if (o + (upto - w) > D3DPT_CMD_SIZE) {
                break;
            }
            memcpy(out + o, win + w, upto - w);
            o += upto - w;
            w = upto;
        }
        if (i == n) {
            break;
        }
        {
            RCU_READ_LOCK_GUARD();
            hwaddr xlat, len = c[i].bytes;
            MemoryRegion *mr = address_space_translate(pci_get_address_space(&s->dev), c[i].gpa, &xlat, &len, false,
                                                       MEMTXATTRS_UNSPECIFIED);

            if (!memory_region_is_ram(mr) || memory_region_is_ram_device(mr) || len < c[i].bytes ||
                o + c[i].bytes > D3DPT_CMD_SIZE) {
                break;
            }
            memcpy(out + o, qemu_map_ram_ptr(mr->ram_block, xlat), c[i].bytes);
            o += c[i].bytes;
            *count += c[i].count;
        }
    }
    return o;
}

/* a doorbell's batch: the pending appends copied into the window at their
 * places (the window ends up as DMA_APPEND would have left it) */
static void d3d_materialize(D3dptVgaState *s)
{
    uint8_t *win = memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset;
    d3dpt_shm_hdr *h = (d3dpt_shm_hdr *)win;
    struct D3dChunk c[64];
    unsigned n;
    uint32_t wb, count, bytes;
    uint8_t *tmp;

    qemu_mutex_lock(&s->pend_lock);
    n = s->pend_n;
    memcpy(c, s->pend, n * sizeof(c[0]));
    s->pend_n = 0;
    s->pend_bytes = s->pend_count = 0;
    qemu_mutex_unlock(&s->pend_lock);
    if (!n) {
        return;
    }
    wb = MIN(qatomic_read(&h->cmd_bytes), D3DPT_CMD_SIZE);
    count = qatomic_read(&h->cmd_count);
    tmp = g_malloc(D3DPT_CMD_SIZE);
    bytes = d3d_assemble(s, tmp, win + D3DPT_CMD_OFFSET, wb, c, n, &count);
    memcpy(win + D3DPT_CMD_OFFSET, tmp, bytes);
    g_free(tmp);
    h->cmd_bytes = bytes;
    h->cmd_count = count;
}

static void d3dpt_vga_regs_write(void *opaque, hwaddr addr, uint64_t val, unsigned size);

/* a fence done by the device: FENCE_DONE only grows */
static void d3d_async_fence(D3dptVgaState *s, uint32_t fence)
{
    uint32_t cur = qatomic_read(&s->fence_done);

    while ((int32_t)(fence - cur) > 0) {
        uint32_t was = qatomic_cmpxchg(&s->fence_done, cur, fence);
        if (was == cur) {
            break;
        }
        cur = was;
    }
}

static void d3d_async_bh(void *opaque)
{
    D3dptVgaState *s = opaque;
    uint64_t flip;
    uint32_t fence;

    /* a kicked flip (ASYNC_FLIP): OFFSET as the driver's write would set
     * it, under the BQL, and only then its fence */
    qemu_mutex_lock(&s->pend_lock);
    flip = s->bh_flip;
    fence = s->bh_fence;
    s->bh_flip = 0;
    qemu_mutex_unlock(&s->pend_lock);
    if (flip) {
        d3dpt_vga_regs_write(s, D3DPT_FB_REG_OFFSET, flip - 1, 4);
        d3d_async_fence(s, fence);
    }
    /* latched even while the driver has IRQ_DMA off (its first batches
     * run before it enables the interrupt): the line rises once it does */
    s->irq_status |= D3DPT_FB_IRQ_DMA;
    fb_irq_update(s);
}

static void *d3d_async_worker(void *opaque)
{
    D3dptVgaState *s = opaque;

    qemu_mutex_lock(&s->async_lock);
    for (;;) {
        unsigned slot;
        uint32_t fence, err;
        int64_t t0;

        while (!s->async_n) {
            qemu_cond_wait(&s->async_cond, &s->async_lock);
        }
        slot = s->async_head;
        fence = s->async_fence[slot];
        s->async_busy = true;
        qemu_mutex_unlock(&s->async_lock);

        if (s->slot_nchunk[slot]) {
            /* the appends, read from guest RAM here: their DMA buffers stay
             * the submission's until its fence is done. The window's own
             * records were kept in the slot's return area */
            uint8_t *b = s->async_buf[slot];
            d3dpt_shm_hdr *bh = (d3dpt_shm_hdr *)b;
            uint32_t count = s->slot_win_count[slot];

            bh->cmd_bytes = d3d_assemble(s, b + D3DPT_CMD_OFFSET, b + D3DPT_RET_OFFSET, s->slot_win_bytes[slot],
                                         s->slot_chunk[slot], s->slot_nchunk[slot], &count);
            bh->cmd_count = count;
        }
        t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        qemu_mutex_lock(&s->exec_lock);
        if (s->exec) {
            if (s->ring_n && s->lib->ring_held && qemu_scanout_ring() && qemu_scanout_ring()->held) {
                s->lib->ring_held(s->exec, qemu_scanout_ring()->held(s->vga.con));
            }
            err = s->lib->submit(s->exec, s->async_buf[slot], D3DPT_SHM_SIZE);
            if (err && s->batches <= 8) {
                info_report("d3dpt-vga: async batch: error %u", err);
            }
        }
        qemu_mutex_unlock(&s->exec_lock);
        qatomic_add(&s->exec_ns, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0);

        /* FENCE_DONE only grows (a fence written meanwhile by the driver
         * is later: it drains before writing one); a flipping batch's is
         * the bottom half's, after the flip (a newer flip there replaces
         * an older one not yet shown: only the newest page matters) */
        if (s->slot_flip[slot]) {
            qemu_mutex_lock(&s->pend_lock);
            s->bh_flip = s->slot_flip[slot];
            s->bh_fence = fence;
            qemu_mutex_unlock(&s->pend_lock);
        } else {
            d3d_async_fence(s, fence);
        }
        qemu_bh_schedule(s->async_bh);

        qemu_mutex_lock(&s->async_lock);
        s->async_head = (s->async_head + 1) % 3;
        s->async_n--;
        s->async_busy = false;
        qemu_cond_broadcast(&s->async_cond);
    }
    return NULL;
}

/* wait until every kicked batch has run (the BQL away meanwhile, as a
 * batch's: the worker's completions need the main loop) */
static void d3d_async_drain(D3dptVgaState *s)
{
    bool unlock;
    bool guard;

    if (!s->async_started) {
        return;
    }
    qemu_mutex_lock(&s->async_lock);
    if (!s->async_n && !s->async_busy) {
        qemu_mutex_unlock(&s->async_lock);
        return;
    }
    qemu_mutex_unlock(&s->async_lock);
    unlock = bql_locked();
    guard = DEVICE(s)->mem_reentrancy_guard.engaged_in_io;
    if (unlock) {
        DEVICE(s)->mem_reentrancy_guard.engaged_in_io = false;
        bql_unlock();
    }
    qemu_mutex_lock(&s->async_lock);
    while (s->async_n || s->async_busy) {
        qemu_cond_wait(&s->async_cond, &s->async_lock);
    }
    qemu_mutex_unlock(&s->async_lock);
    if (unlock) {
        bql_lock();
        DEVICE(s)->mem_reentrancy_guard.engaged_in_io = guard;
    }
}

static bool d3d_load(D3dptVgaState *s);

/* ASYNC_KICK: the window's batch to the worker, the fence done after it */
static void d3d_async_kick(D3dptVgaState *s, uint32_t fence)
{
    uint8_t *win = memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset;
    d3dpt_shm_hdr *h = (d3dpt_shm_hdr *)win;
    uint32_t bytes = qatomic_read(&h->cmd_bytes), count = qatomic_read(&h->cmd_count);
    d3dpt_shm_hdr *bh;
    unsigned slot;

    if (!s->cmd_offset || !s->async || !d3d_load(s)) {
        /* no executor: done as a doorbell would leave it */
        qemu_mutex_lock(&s->pend_lock);
        s->pend_n = 0;
        s->pend_bytes = s->pend_count = 0;
        qemu_mutex_unlock(&s->pend_lock);
        h->cmd_bytes = h->cmd_count = 0;
        if (s->async_flip_next) {
            uint64_t f = s->async_flip_next;

            s->async_flip_next = 0;
            d3dpt_vga_regs_write(s, D3DPT_FB_REG_OFFSET, f - 1, 4);
        }
        s->fence_done = fence;
        d3d_async_bh(s);
        return;
    }
    if (!s->async_started) {
        int i;

        qemu_mutex_init(&s->async_lock);
        qemu_cond_init(&s->async_cond);
        for (i = 0; i < 3; i++) {
            s->async_buf[i] = g_malloc0(D3DPT_SHM_SIZE);
        }
        s->async_bh = qemu_bh_new(d3d_async_bh, s);
        qemu_thread_create(&s->async_thread, "d3dpt-async", d3d_async_worker, s, QEMU_THREAD_DETACHED);
        s->async_started = true;
    }
    if (bytes > D3DPT_CMD_SIZE || (bytes & 7)) {
        bytes = count = 0;          /* the executor would refuse it: an empty batch */
    }
    /* a free slot (all three queued: wait, the BQL away) */
    qemu_mutex_lock(&s->async_lock);
    s->async_kicks++;
    if (s->async_n == 3) {
        int64_t w0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        bool unlock = bql_locked();
        bool guard = DEVICE(s)->mem_reentrancy_guard.engaged_in_io;

        if (unlock) {
            qemu_mutex_unlock(&s->async_lock);
            DEVICE(s)->mem_reentrancy_guard.engaged_in_io = false;
            bql_unlock();
            qemu_mutex_lock(&s->async_lock);
        }
        while (s->async_n == 3) {
            qemu_cond_wait(&s->async_cond, &s->async_lock);
        }
        s->async_waits++;
        s->async_wait_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - w0;
        if (unlock) {
            qemu_mutex_unlock(&s->async_lock);
            bql_lock();
            DEVICE(s)->mem_reentrancy_guard.engaged_in_io = guard;
            qemu_mutex_lock(&s->async_lock);
        }
    }
    slot = (s->async_head + s->async_n) % 3;
    qemu_mutex_unlock(&s->async_lock);

    /* the slot is the kicker's alone until it is queued */
    bh = (d3dpt_shm_hdr *)s->async_buf[slot];
    memset(bh, 0, sizeof(*bh));
    qemu_mutex_lock(&s->pend_lock);
    if (s->pend_n && bytes <= D3DPT_RET_SIZE) {
        /* appends still in guest memory: the worker copies them; the
         * window's own records wait in the slot's return area */
        memcpy(s->async_buf[slot] + D3DPT_RET_OFFSET, win + D3DPT_CMD_OFFSET, bytes);
        memcpy(s->slot_chunk[slot], s->pend, s->pend_n * sizeof(s->pend[0]));
        s->slot_nchunk[slot] = s->pend_n;
        s->slot_win_bytes[slot] = bytes;
        s->slot_win_count[slot] = count;
        s->pend_n = 0;
        s->pend_bytes = s->pend_count = 0;
        qemu_mutex_unlock(&s->pend_lock);
    } else {
        qemu_mutex_unlock(&s->pend_lock);
        d3d_materialize(s);         /* (none pending, or a window too big to keep) */
        bytes = MIN(qatomic_read(&h->cmd_bytes), D3DPT_CMD_SIZE);
        count = qatomic_read(&h->cmd_count);
        s->slot_nchunk[slot] = 0;
        bh->cmd_bytes = bytes;
        bh->cmd_count = count;
        memcpy(s->async_buf[slot] + D3DPT_CMD_OFFSET, win + D3DPT_CMD_OFFSET, bytes);
    }
    h->cmd_bytes = 0;
    h->cmd_count = 0;
    h->ret_status = 0;
    s->batches++;

    s->slot_flip[slot] = s->async_flip_next;
    s->async_flip_next = 0;
    qemu_mutex_lock(&s->async_lock);
    s->async_fence[slot] = fence;
    s->async_n++;
    qemu_cond_broadcast(&s->async_cond);
    qemu_mutex_unlock(&s->async_lock);
}

static void d3d_doorbell(D3dptVgaState *s)
{
    uint8_t *win = memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset;
    d3dpt_shm_hdr *h = (d3dpt_shm_hdr *)win;

    if (!s->cmd_offset) {
        return;
    }
    d3d_async_drain(s);         /* kicked batches first (register set v15) */
    d3d_materialize(s);         /* and appends not copied yet, into the window */
    s->batches++;
    if (d3d_load(s)) {
        /* How long the host spends in a batch, against how long the guest
         * takes to bring the next one: a frame rate that halves with the
         * host's share flat is a slow guest, and one that halves with the
         * share grown is us (3DMark slow after some reboots). */
        int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        /* The batch runs without the BQL under a hardware accelerator
         * (track M25): a DWM frame keeps the executor 1-3 ms, and with the
         * BQL held the main loop (the vertical blank's timer, the host's
         * blanks) and every other vCPU's exits waited for it. The executor
         * reads the window and VRAM, which are RAM, and calls back only
         * d3d_log and d3d_vram_dirty, neither of which needs the BQL; its
         * own calls are serialised by exec_lock. A reset waits: it pauses
         * the vCPUs, and this one stops only once it holds the BQL again.
         * Under TCG the BQL stays held (a vCPU inside a translation block's
         * I/O, and round-robin vCPUs gain nothing). */
        bool unlock = !tcg_enabled() && bql_locked();
        uint32_t err;

        /* QEMU's re-entrancy guard is the device's, not the thread's: left
         * engaged, another vCPU's register access during the batch was
         * refused as re-entrant ("Blocked re-entrant IO"), its read gave
         * 0, and the interrupt service reported fence 0 (bugcheck 0x119,
         * subtype 1). The executor makes no DMA, so the guard is lowered
         * while the BQL is away and raised again under it. */
        bool guard = DEVICE(s)->mem_reentrancy_guard.engaged_in_io;

        if (unlock) {
            DEVICE(s)->mem_reentrancy_guard.engaged_in_io = false;
            bql_unlock();
        }
        qemu_mutex_lock(&s->exec_lock);
        if (s->ring_n && s->lib->ring_held && qemu_scanout_ring() && qemu_scanout_ring()->held) {
            /* the ring's slots the front end may still sample (M26): the
             * batch's flips go into the others */
            s->lib->ring_held(s->exec, qemu_scanout_ring()->held(s->vga.con));
        }
        err = s->lib->submit(s->exec, win, D3DPT_SHM_SIZE);
        qemu_mutex_unlock(&s->exec_lock);
        if (unlock) {
            bql_lock();
            DEVICE(s)->mem_reentrancy_guard.engaged_in_io = guard;
        }
        s->d3d_err = err;
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
    uint32_t bytes = s->dma_bytes, cur;

    if (!s->cmd_offset) {
        return D3DPT_FB_DMA_BAD;
    }
    /* the header is guest memory another vCPU may write meanwhile: read it once */
    cur = qatomic_read(&h->cmd_bytes);
    if (!count || !bytes || (bytes & 7) || cur > D3DPT_CMD_SIZE) {
        return D3DPT_FB_DMA_BAD;
    }
    if (bytes > D3DPT_CMD_SIZE - cur) {
        return D3DPT_FB_DMA_NO_ROOM;
    }
    /* The copy runs without the BQL under a hardware accelerator, as the
     * doorbell's batch does (track M25): a game's frame appends ~4 MiB in
     * ~15 submissions, and with the BQL held each waited for the main
     * loop's display work and held up every other vCPU's exits. The
     * source is mapped first, with the BQL held and the re-entrancy guard
     * engaged, so whatever it names (an address that is MMIO, this BAR
     * included, comes back as a bounce buffer read here) is read under
     * both. Only a memcpy out of the mapping runs unlocked, which
     * dispatches nothing; the guard is lowered for it as the doorbell's
     * is, since another vCPU's register access meanwhile (the interrupt
     * service's) must not read 0 as re-entrant. A mapping shorter than
     * the records (a bounce buffer's limit, a range that crosses regions)
     * is copied the old way, locked. */
    {
        dma_addr_t len = bytes;
        void *src = tcg_enabled() ? NULL : pci_dma_map(&s->dev, addr, &len, DMA_DIRECTION_TO_DEVICE);
        bool ok;

        if (src && len == bytes) {
            bool unlock = bql_locked();
            bool guard = DEVICE(s)->mem_reentrancy_guard.engaged_in_io;
            int64_t c0;

            if (unlock) {
                DEVICE(s)->mem_reentrancy_guard.engaged_in_io = false;
                bql_unlock();
            }
            c0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            memcpy(win + D3DPT_CMD_OFFSET + cur, src, bytes);
            s->dma_copy_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - c0;
            if (unlock) {
                bql_lock();
                DEVICE(s)->mem_reentrancy_guard.engaged_in_io = guard;
            }
            ok = true;
        } else {
            ok = pci_dma_read(&s->dev, addr, win + D3DPT_CMD_OFFSET + cur, bytes) == MEMTX_OK;
        }
        if (src) {
            pci_dma_unmap(&s->dev, src, len, DMA_DIRECTION_TO_DEVICE, 0);
        }
        if (!ok) {
            return D3DPT_FB_DMA_BAD;
        }
        /* another vCPU may have appended meanwhile: the window is the
         * guest's own memory and the executor checks every record at the
         * doorbell, so only the count is re-read, not trusted */
        if (qatomic_read(&h->cmd_bytes) != cur) {
            return D3DPT_FB_DMA_BAD;
        }
    }
    qatomic_set(&h->cmd_bytes, cur + bytes);
    h->cmd_count += count;
    s->dma_appends++;
    s->dma_appended += bytes;
    return D3DPT_FB_DMA_OK;
}

/* guest reset: every context and surface the driver registered is gone */
static void d3d_reset(D3dptVgaState *s)
{
    if (s->exec) {
        qemu_mutex_lock(&s->exec_lock);
        s->lib->attach(s->exec, 0);
        s->lib->attach(s->exec, 1);
        qemu_mutex_unlock(&s->exec_lock);
    }
    if (s->cmd_offset) {
        memset(memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset, 0, sizeof(d3dpt_shm_hdr));
    }
    s->d3d_err = 0;
    s->batches = 0;
    s->batches_last = 0;
    s->exec_ns_last = s->exec_ns;
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
 * desktop's coordinates, scaled with the frame.
 *
 * A driver that writes CURSOR_OWNED (register set v8; the WDDM driver)
 * gets none of the flip hiding: dxgkrnl tells it when the pointer hides,
 * and Windows 7's DWM flips the desktop at every composed frame, so the
 * pointer was hidden whenever anything on the screen moved. */
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

/* A flipped page into the shadow, only the rows that differ from what the
 * shadow holds (the page shown before). DWM flips a whole page for a caret
 * that blinked; copied and pushed whole, every flip was the mode's size
 * all the way to the player's texture (33 MB at 3840x2160, and so the
 * desktop grew choppier with the window). A row that differs is copied
 * once memcmp finds the difference, so a page that changed everywhere
 * costs about what the whole copy did. Only a 32 bpp page with no gamma
 * ramp is the shadow's own bytes; anything converted is converted whole. */
static void fb_push_changed(D3dptVgaState *s)
{
    const uint8_t *src = memory_region_get_ram_ptr(&s->vga.vram) + s->lin.offset;
    uint8_t *dst = (uint8_t *)pixman_image_get_data(s->shadow);
    int dstride = pixman_image_get_stride(s->shadow);
    size_t row = (size_t)s->lin.w * 4;
    int y, ys = -1, ye = 0;

    if (s->lin.bpp != 32 || s->lin.rgb || s->gamma_active || s->full_frames) {
        fb_update_span(s, 0, s->lin.h);
        return;
    }
    for (y = 0; y < (int)s->lin.h; y++) {
        const uint8_t *a = src + (size_t)s->lin.pitch * y;
        uint8_t *b = dst + (size_t)dstride * y;

        if (memcmp(a, b, row) != 0) {
            memcpy(b, a, row);
            if (ys < 0) {
                ys = y;
            }
            ye = y + 1;
        }
    }
    /* one update, the rows from the first change to the last: each update
     * outside the refresh is a frame the player publishes (embed v12's
     * flush), and a flip is one frame, never shown half pushed */
    if (ys >= 0) {
        qemu_console_update(s->vga.con, 0, ys, s->lin.w, ye - ys);
    }
}

/* flip-push: the page a flip shows, copied into the console's shadow and
 * pushed now, from the flip's register write (tracks M20, M24). Polled by
 * the display's tick instead (PLAYER_REFRESH_MS, ~62.5 Hz), a guest that
 * flips at another rate (72 Hz on a 144 Hz host screen) has frames shown
 * twice or never, and a surface standing on VRAM is read whenever the
 * player gets to it, while the guest may already draw into that page
 * again. The page a flip shows is the one nothing draws into until the
 * next flip, so the copy is a whole, finished frame; an update outside the
 * tick is a flush to the embed library (v12 on_flush), shown at once. */
static void fb_flip_push(D3dptVgaState *s)
{
    D3dptLinearMode m;

    if (!s->flip_push || !s->lin_on || !s->shadow || s->resurface ||
        !fb_get_mode(s, &m) || m.w != s->lin.w || m.h != s->lin.h ||
        m.bpp != s->lin.bpp || m.pitch != s->lin.pitch || m.rgb != s->lin.rgb) {
        s->push_tries = 0;
        return;     /* a mode change: the tick switches surfaces */
    }
    if (s->pal_dirty && m.bpp == 8) {
        fb_apply_palette(s);
    }
    /* the page's frame on the GPU (M26): shown from its ring slot, once
     * the executor's copy there is done (tried again as below) */
    switch (fb_ring_show(s, &m, s->push_tries >= D3DPT_SETTLE_TRIES)) {
    case 1:
        s->push_tries = 0;
        timer_del(s->settle_timer);
        fb_make_src(s, &m);
        s->lin = m;
        return;
    case -1:
        s->push_tries++;
        timer_mod(s->settle_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1);
        return;
    }
    /* the page's pixels, if the executor still owes them: tried again by
     * the timer, the newest page then (a flip in between supersedes it) */
    if (!fb_settled(s, s->push_tries >= D3DPT_SETTLE_TRIES)) {
        s->push_tries++;
        timer_mod(s->settle_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1);
        return;
    }
    s->push_tries = 0;
    timer_del(s->settle_timer);
    fb_make_src(s, &m);
    s->lin = m;
    g_free(memory_region_snapshot_and_clear_dirty(&s->vga.vram, m.offset,
                                                  (uint64_t)m.pitch * m.h,
                                                  DIRTY_MEMORY_VGA));
    if (s->shadow_stale) {
        /* a slot had the screen: this page is all of it now, VRAM had the
         * page's latest (no slot does), so whatever rows match the shadow */
        s->shadow_stale = false;
        fb_update_span(s, 0, m.h);
    } else {
        fb_push_changed(s);
    }
    if (!s->flip_pushes++) {
        info_report("d3dpt-vga: flips push their page (flip-push)");
    }
}

/* a flip-push the executor's readback held: try it again */
static void fb_settle_tick(void *opaque)
{
    fb_flip_push(opaque);
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
               (s->irq ? D3DPT_FB_CAP_IRQ : 0) | (s->cmd_offset ? D3DPT_FB_CAP_DMA : 0) | D3DPT_FB_CAP_FILL | D3DPT_FB_CAP_COPY |
               (s->d3d11 ? D3DPT_FB_CAP_D3D11 : 0) | (s->follow && s->irq ? D3DPT_FB_CAP_FOLLOW : 0) |
               (s->cmd_offset && s->fb_version >= 14u ? D3DPT_FB_CAP_DMA1 : 0) |
               (s->cmd_offset && s->async && s->irq && s->fb_version >= 15u ? D3DPT_FB_CAP_ASYNC : 0) |
               (s->d3d11 && s->fb_version >= 16u ? (s->msaa << D3DPT_FB_CAP_MSAA_SHIFT) & D3DPT_FB_CAP_MSAA_MASK : 0) |
               (s->fb_version >= 17u ? D3DPT_FB_CAP_SCAN_RGB : 0);
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
    case D3DPT_FB_REG_CURSOR_FLAGS:
        return s->cur_flags;
    case D3DPT_FB_REG_FILL_ADDR:
        return s->fill_addr;
    case D3DPT_FB_REG_FILL_BYTES:
        return s->fill_bytes;
    case D3DPT_FB_REG_FILL_PATTERN:
        return s->fill_pattern;
    case D3DPT_FB_REG_FILL_GO:
        return s->fill_status;
    case D3DPT_FB_REG_COPY_VRAM:
        return s->copy_vram;
    case D3DPT_FB_REG_COPY_BYTES:
        return s->copy_bytes;
    case D3DPT_FB_REG_COPY_LIST_LO:
        return (uint32_t)s->copy_list;
    case D3DPT_FB_REG_COPY_LIST_HI:
        return (uint32_t)(s->copy_list >> 32);
    case D3DPT_FB_REG_COPY_LIST_OFF:
        return s->copy_list_off;
    case D3DPT_FB_REG_COPY_GO:
        return s->copy_status;
    case D3DPT_FB_REG_COPY_ROWS:
        return s->copy_rows;
    case D3DPT_FB_REG_COPY_VRAM_PITCH:
        return s->copy_vpitch;
    case D3DPT_FB_REG_COPY_PAGE_PITCH:
        return s->copy_ppitch;
    case D3DPT_FB_REG_COPY_SRC_VRAM:
        return s->copy_src;
    case D3DPT_FB_REG_COPY_LIST_COUNT:
        return s->copy_count;
    case D3DPT_FB_REG_HOST_MHZ:
        /* the rate the front end's blanks come at (QEMU patch 96): the
         * guest's modes say it, so its DXGI times frames by it */
        return qemu_host_refresh_mhz();
    case D3DPT_FB_REG_FOLLOW:
        return s->follow_on;
    case D3DPT_FB_REG_PREF_W:
        return s->follow_on ? s->pref_w : 0;
    case D3DPT_FB_REG_PREF_H:
        return s->follow_on ? s->pref_h : 0;
    case D3DPT_FB_REG_BLANK:
        return s->r_blank;
    case D3DPT_FB_REG_SCAN_RGB:
        return s->r_scan_rgb;
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
    bool changed;

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
        changed = val != s->r_offset;
        if (changed) {
            s->flips++;
            fb_flip_rate(s);
            if (s->r_enable && !(s->cur_flags & D3DPT_FB_CURSOR_OWNED)) {
                if (!s->cur_flip_hidden) {
                    s->cur_flip_hidden = true;
                    s->cur_flip_desktop = s->r_offset;  /* the page GDI was on */
                    if (s->cur_flip_idles < 64) {
                        info_report("d3dpt-vga: page flip from offset %u: a flip chain has the screen, "
                                    "the cursor hides", s->r_offset);
                    }
                    s->r_offset = val;
                    if (s->cur_defined && s->cur_on) {
                        fb_cursor_move(s);  /* a flip chain has the screen */
                    }
                }
                timer_mod(s->cur_flip_timer,
                          qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + D3DPT_FB_FLIP_IDLE_MS);
            }
        }
        if (changed) {
            s->r_offset = val;
            fb_flip_push(s);    /* also the flip that hid the cursor above */
        }
        break;
    case D3DPT_FB_REG_HZ:
        s->r_hz = val;
        break;
    case D3DPT_FB_REG_DEBUG: {
        char c = (char)val;
        if (c == '\n' || s->dbg_len == sizeof(s->dbg) - 1) {
            /* the driver's debug channel, a line a VM exit a character:
             * plenty for debugging, but a guest looping on it does not
             * fill the host's log (1000 lines a second, the rest counted) */
            int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) / 1000;
            if (now != s->dbg_second) {
                if (s->dbg_dropped) {
                    info_report("d3dpt-vga: guest: %u debug lines dropped", s->dbg_dropped);
                }
                s->dbg_second = now;
                s->dbg_lines = s->dbg_dropped = 0;
            }
            s->dbg[s->dbg_len] = 0;
            if (s->dbg_lines++ < 1000) {
                info_report("d3dpt-vga: guest: %s", s->dbg);
            } else {
                s->dbg_dropped++;
            }
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
    case D3DPT_FB_REG_CURSOR_FLAGS:
        s->cur_flags = val & D3DPT_FB_CURSOR_OWNED;
        if (s->cur_flags & D3DPT_FB_CURSOR_OWNED) {
            s->cur_flip_hidden = false;
            timer_del(s->cur_flip_timer);
            if (s->cur_defined) {
                fb_cursor_move(s);
            }
        }
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
    case D3DPT_FB_REG_ASYNC_KICK:
        d3d_async_kick(s, val);
        break;
    case D3DPT_FB_REG_ASYNC_DRAIN:
        d3d_async_drain(s);
        break;
    case D3DPT_FB_REG_ASYNC_FLIP:
        s->async_flip_next = val + 1;
        break;
    case D3DPT_FB_REG_DMA_APPEND1:
        /* register set v14: the address and bytes from the window's
         * header, the answer back there (one exit where DMA_APPEND took
         * five, track M25) */
        if (s->cmd_offset) {
            int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            d3dpt_shm_hdr *h = (d3dpt_shm_hdr *)(memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset);

            s->dma_lo = qatomic_read(&h->dma_addr_lo);
            s->dma_hi = qatomic_read(&h->dma_addr_hi);
            s->dma_bytes = qatomic_read(&h->dma_bytes);
            s->dma_status = d3d_dma_append(s, val);
            qatomic_set(&h->dma_status, s->dma_status);
            s->dma_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
        }
        break;
    case D3DPT_FB_REG_FILL_ADDR:
        s->fill_addr = val;
        break;
    case D3DPT_FB_REG_FILL_BYTES:
        s->fill_bytes = val;
        break;
    case D3DPT_FB_REG_FILL_PATTERN:
        s->fill_pattern = val;
        break;
    case D3DPT_FB_REG_FILL_GO:
        s->fill_status = fb_fill(s);
        break;
    case D3DPT_FB_REG_COPY_VRAM:
        s->copy_vram = val;
        break;
    case D3DPT_FB_REG_COPY_BYTES:
        s->copy_bytes = val;
        break;
    case D3DPT_FB_REG_COPY_LIST_LO:
        s->copy_list = (s->copy_list & ~0xffffffffull) | val;
        break;
    case D3DPT_FB_REG_COPY_LIST_HI:
        s->copy_list = (s->copy_list & 0xffffffffull) | ((uint64_t)val << 32);
        break;
    case D3DPT_FB_REG_COPY_LIST_OFF:
        s->copy_list_off = val;
        break;
    case D3DPT_FB_REG_COPY_GO:
        s->copy_status = fb_copy(s, val);
        break;
    case D3DPT_FB_REG_COPY_ROWS:
        s->copy_rows = val;
        break;
    case D3DPT_FB_REG_COPY_VRAM_PITCH:
        s->copy_vpitch = val;
        break;
    case D3DPT_FB_REG_COPY_PAGE_PITCH:
        s->copy_ppitch = val;
        break;
    case D3DPT_FB_REG_COPY_SRC_VRAM:
        s->copy_src = val;
        break;
    case D3DPT_FB_REG_COPY_LIST_COUNT:
        s->copy_count = val;
        break;
    case D3DPT_FB_REG_FOLLOW:
        fb_follow(s, val == 1);
        break;
    case D3DPT_FB_REG_BLANK:
        if ((val != 0) != (s->r_blank != 0)) {
            info_report("d3dpt-vga: source %s", val ? "off (black)" : "on");
            if (val) {
                /* a mode switch may turn the source off for a moment too */
                s->vga_grace_until = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                                     D3DPT_FB_VGA_GRACE_MS;
            }
        }
        s->r_blank = val != 0;
        qemu_console_hw_invalidate(s->vga.con);
        break;
    case D3DPT_FB_REG_SCAN_RGB:
        /* the mode's channel order (a full-screen R8G8B8A8 swap chain's
         * mode): the next refresh switches surfaces (the shadow converts) */
        if (s->fb_version >= 17u && (val != 0) != (s->r_scan_rgb != 0)) {
            s->r_scan_rgb = val != 0;
            info_report("d3dpt-vga: 32 bpp pixels are %s", s->r_scan_rgb ? "x8b8g8r8" : "x8r8g8b8");
            qemu_console_hw_invalidate(s->vga.con);
        }
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

/* DMA_APPEND1 (register set v14) in a region of its own that runs without
 * the BQL (track M25): under WHPX an exit takes the BQL only to dispatch
 * to a device, and an append waited ~0.7 ms for it behind the main
 * loop's display work, where the copy itself took ~50 us. This handler
 * touches the append's own state alone (the window's header and its
 * records, the rate line's counters): the source must translate to guest
 * RAM in one piece, read under RCU, so nothing it names is MMIO and no
 * device is re-entered (an append from anything else is refused, which
 * the driver never asks for); and it keeps no re-entrancy guard, a plain
 * flag another vCPU's access under the BQL would read as set. */
static void d3dpt_dma1_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    D3dptVgaState *s = opaque;
    uint8_t *win;
    d3dpt_shm_hdr *h;
    uint32_t bytes, cur, count = (uint32_t)val, st = D3DPT_FB_DMA_BAD;
    dma_addr_t src;
    int64_t t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (!s->cmd_offset) {
        return;
    }
    win = memory_region_get_ram_ptr(&s->vga.vram) + s->cmd_offset;
    h = (d3dpt_shm_hdr *)win;
    src = ((uint64_t)qatomic_read(&h->dma_addr_hi) << 32) | qatomic_read(&h->dma_addr_lo);
    bytes = qatomic_read(&h->dma_bytes);
    cur = qatomic_read(&h->cmd_bytes);
    if (!count || !bytes || (bytes & 7) || cur > D3DPT_CMD_SIZE) {
        st = D3DPT_FB_DMA_BAD;
    } else if (s->async) {
        /* deferred: only where it goes is kept, and the copy is the
         * worker's (a kicked batch) or the doorbell's. With the copy here
         * the exit took ~350 us from the guest's side, ~20 us of it the
         * copy (track M25) */
        qemu_mutex_lock(&s->pend_lock);
        if (s->pend_n == 64 || (uint64_t)cur + s->pend_bytes + bytes > D3DPT_CMD_SIZE) {
            st = D3DPT_FB_DMA_NO_ROOM;
        } else {
            struct D3dChunk *c = &s->pend[s->pend_n++];

            c->at = cur;
            c->bytes = bytes;
            c->count = count;
            c->gpa = src;
            s->pend_bytes += bytes;
            s->pend_count += count;
            s->dma_appends++;
            s->dma_appended += bytes;
            st = D3DPT_FB_DMA_OK;
        }
        qemu_mutex_unlock(&s->pend_lock);
    } else if (bytes > D3DPT_CMD_SIZE - cur) {
        st = D3DPT_FB_DMA_NO_ROOM;
    } else {
        RCU_READ_LOCK_GUARD();
        hwaddr xlat, len = bytes;
        MemoryRegion *mr = address_space_translate(pci_get_address_space(&s->dev), src, &xlat, &len, false,
                                                   MEMTXATTRS_UNSPECIFIED);

        if (memory_region_is_ram(mr) && !memory_region_is_ram_device(mr) && len >= bytes) {
            int64_t c0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

            memcpy(win + D3DPT_CMD_OFFSET + cur, qemu_map_ram_ptr(mr->ram_block, xlat), bytes);
            s->dma_copy_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - c0;
            /* another vCPU appending meanwhile: refused, not merged (the
             * executor checks every record at the doorbell anyway) */
            if (qatomic_cmpxchg(&h->cmd_bytes, cur, cur + bytes) == cur) {
                qatomic_add(&h->cmd_count, count);
                s->dma_appends++;
                s->dma_appended += bytes;
                st = D3DPT_FB_DMA_OK;
            }
        }
    }
    qatomic_set(&h->dma_status, st);
    s->dma_ns += qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0;
}

static uint64_t d3dpt_dma1_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static const MemoryRegionOps d3dpt_dma1_ops = {
    .read = d3dpt_dma1_read,
    .write = d3dpt_dma1_write,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

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

    qemu_mutex_init(&s->exec_lock);
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
    s->settle_timer = timer_new_ms(QEMU_CLOCK_REALTIME, fb_settle_tick, s);

    memory_region_init_io(&s->regs, OBJECT(dev), &d3dpt_vga_regs_ops, s,
                          "d3dpt-vga.regs", D3DPT_FB_REGS_SIZE);
    qemu_mutex_init(&s->pend_lock);
    /* register set v14's DMA_APPEND1 over the registers, BQL-free under a
     * hardware accelerator (d3dpt_dma1_write); under TCG it stays the
     * register (d3dpt_vga_regs_write's case) */
    if (!tcg_enabled()) {
        memory_region_init_io(&s->dma1, OBJECT(dev), &d3dpt_dma1_ops, s, "d3dpt-vga.dma1", 4);
        memory_region_enable_lockless_io(&s->dma1);
        s->dma1.disable_reentrancy_guard = true;
        memory_region_add_subregion_overlap(&s->regs, D3DPT_FB_REG_DMA_APPEND1, &s->dma1, 1);
    }
    s->indexed = g_new0(pixman_indexed_t, 1);
    s->indexed->color = true;

    if (s->bar64) {
        /* takes BAR slots 0 and 1, so the registers move to 2 */
        pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH | PCI_BASE_ADDRESS_MEM_TYPE_64,
                         &vga->vram);
        pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->regs);
    } else {
        pci_register_bar(dev, 0, PCI_BASE_ADDRESS_MEM_PREFETCH, &vga->vram);
        pci_register_bar(dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->regs);
    }
    if (s->irq) {
        dev->config[PCI_INTERRUPT_PIN] = 1;
    }
    s->vbl_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, fb_vbl_tick, s);
    if (s->irq && s->host_vblank) {
        s->host_vbl.notify = fb_host_vblank;
        qemu_host_vblank_add_notifier(&s->host_vbl);
    }

    /* the command window takes the top 64 MiB when at least as much is
     * left below it for the frame buffer and the DirectDraw heap */
    if (vga->vram_size >= 2 * D3DPT_SHM_SIZE) {
        s->cmd_offset = vga->vram_size - D3DPT_SHM_SIZE;
        /* no display scans the window, and the host writes megabytes a
         * frame into it (the appended records): an accelerator that tracks
         * VRAM's dirty pages leaves it out (QEMU patch 99; under WHPX each
         * host write there cost the hypervisor's tracking, ~0.4 ms an
         * append, track M25) */
        vga->vram.dirty_track_skip_offset = s->cmd_offset;
        vga->vram.dirty_track_skip_size = D3DPT_SHM_SIZE;
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
    s->r_blank = 0;
    s->r_scan_rgb = 0;
    s->blank_shown = false;
    s->vbl_ns = 0;
    s->cur_flip_hidden = false;
    s->cur_flags = 0;
    timer_del(s->cur_flip_timer);
    timer_del(s->settle_timer);
    s->push_tries = s->gfx_skips = 0;
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
    fb_follow(s, false);   /* a rebooted guest's screen is its own until its driver follows */
    s->dma_lo = s->dma_hi = s->dma_bytes = s->dma_status = 0;
    d3d_async_drain(s);    /* nothing kicked runs into the next boot */
    qemu_mutex_lock(&s->pend_lock);
    s->pend_n = 0;
    s->pend_bytes = s->pend_count = 0;
    s->bh_flip = 0;
    qemu_mutex_unlock(&s->pend_lock);
    s->async_flip_next = 0;
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
    /* convert the whole frame every refresh (and every pushed flip) instead
     * of the dirty spans (the changed rows):
     * the A/B for a picture that comes out in stale bands (a
     * 3DMark 99 loading screen on the PC). On means the pixels in VRAM are
     * right and this device's incremental path is what lost them. */
    DEFINE_PROP_BOOL("full-frames", D3dptVgaState, full_frames, false),
    /* a page flip copies the page it shows into the console's own surface
     * and pushes it to the display at once (fb_flip_push); off, the
     * console stands on VRAM and the display's tick polls it, the A/B */
    DEFINE_PROP_BOOL("flip-push", D3dptVgaState, flip_push, true),
    /* with flip-push: a flipped frame the executor rendered is copied into
     * the front end's scanout ring on the GPU and shown from there instead
     * of read back into VRAM (track M26, Windows, macOS and Linux hosts); off,
     * the A/B */
    DEFINE_PROP_BOOL("zero-copy", D3dptVgaState, zero_copy, true),
    /* the scanout ring's images (2 to 8). A flip that finds every one
     * the front end may sample held, or that would take another page's
     * newest frame, costs a readback: with 3, Windows 11's DWM at
     * 2560x1676 on the Mac read back half its frames, with 5 none (M26) */
    DEFINE_PROP_UINT32("ring-slots", D3dptVgaState, ring_slots, 5),
    /* an interrupt pin for the WDDM driver (M18, Windows 7) and the IRQ
     * registers behind it (register set v6: the vertical blank) */
    DEFINE_PROP_BOOL("irq", D3dptVgaState, irq, false),
    /* with irq: IRQ_VBLANK on the host screen's blank when the player gives
     * one (fb_host_vblank); off, the guest's clock alone, the A/B */
    DEFINE_PROP_BOOL("host-vblank", D3dptVgaState, host_vblank, true),
    /* with irq: CAP_FOLLOW, the desktop following the front end's window
     * once the driver writes FOLLOW (register set v12); off, the A/B */
    DEFINE_PROP_BOOL("follow", D3dptVgaState, follow, true),
    /* bar64=on: VRAM is a 64-bit BAR 0 (slots 0 and 1) and the registers
     * BAR 2, so the firmware may place VRAM above 4 GiB. Windows 11 on
     * Arm (track M25): the virt board's 32-bit window cannot hold 512 MiB
     * aligned to its size. Only the WDDM driver reads this layout; with
     * an option ROM EDK2 puts the BAR below 4 GiB again (romfile=). */
    DEFINE_PROP_BOOL("bar64", D3dptVgaState, bar64, false),
    /* with irq: CAP_ASYNC (register set v15), the WDDM driver's batches run
     * by a worker thread, their fences done by IRQ_DMA (track M25); off,
     * every batch runs inside its doorbell, the A/B */
    DEFINE_PROP_BOOL("async", D3dptVgaState, async, true),
    /* register set v16: the host's multisample counts offered to the guest, ANDed with this (bits 0-3 2x 4x 8x 16x up to
     * 64-bit formats, 4-7 the 128-bit ones): 0 offers none, 0x33 2x and 4x only (a weaker host's, the A/B) */
    DEFINE_PROP_UINT32("msaa", D3dptVgaState, msaa_mask, 0xff),
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
