/*
 * voodoo2.c: the 3dfx Voodoo 2 as a QEMU PCI device (doc 21, M14).
 *
 * The chip is 86Box's emulation, vendored verbatim under 86box/ and built
 * against the shim of 86Box's platform headers under shim/ (voodoo_shim.c
 * is the shim's other half). This file is what QEMU sees: a PCI function
 * 121a:0002 with one 16 MiB memory BAR whose accesses go to 86Box's
 * register/LFB/texture handlers, the configuration bytes 86Box owns
 * (initEnable at 0x40) forwarded to its handlers, and the display half.
 * A Voodoo 1/2 is a pass-through card, so when the guest sets fbiInit0's
 * VGA_PASS bit the guest console stops being drawn by the VGA device and
 * every frame the Voodoo's display timer completes is copied into it.
 * Screendumps, the VNC fallback and the player's own surface path all see
 * that frame the way they see a VGA one.
 *
 *   -device voodoo2[,fbmem=2|4][,texmem=2|4][,threads=1|2|4]
 *                  [,bilinear=on|off][,dither-sub=on|off][,filter=on|off]
 *                  [,undither=on|off]
 *                  [,recompiler=on|off][,ramfifo=on|off][,lfb-order=on|off]
 *
 * lfb-order (off): also wait for the ring before an LFB or texture write.
 * The other half of that ordering (waiting for 86Box's own FIFO before the
 * ring's next packets, the one a game meets) is unconditional.
 * See "one order" below.
 *
 * ramfifo (on by default): the command-FIFO ring is plain RAM to the guest,
 * so Glide's packet stream is ordinary stores instead of one MMIO trap per
 * dword. Under TCG a trap mid-block is a cpu_io_recompile, which was
 * most of the vCPU's time in Quake II (doc 21 §9). The chip learns what was
 * written at the guest's next access to anything else on the card; see
 * voodoo2_fifo_sync().
 *
 * Threads: the guest's MMIO writes run on the vCPU thread with the BQL; the
 * display timer on the main loop with the BQL (86Box has both on its CPU
 * thread, which is the invariant that matters); the FIFO thread and the
 * render threads are 86Box's own and touch nothing of QEMU's.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/host-utils.h"
#include "qemu/memalign.h"
#include "qemu/units.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "qom/object.h"
#include "hw/core/cpu.h"
#include "exec/target_page.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "cpu.h"                /* before the shim: it #defines tsc */

#include "shim/86box/86box.h"
#include "shim/86box/device.h"
#include "shim/86box/mem.h"
#include "shim/86box/pci.h"
#include "shim/86box/plat.h"
#include "shim/86box/thread.h"
#include "shim/86box/timer.h"
#include "shim/86box/video.h"
#include "shim/86box/vid_svga.h"
#include "86box/vid_voodoo_common.h"
#include "86box/vid_voodoo_fifo.h"
#include "voodoo_shim.h"
#include "undither.h"

/* 86box/vid_voodoo_regs.h's offsets (the header itself does not compile
 * outside 86Box's own files) */
#define SST_status          0x000
#define SST_cmdFifoBaseAddr 0x1e0
#define SST_cmdFifoRdPtr    0x1e8
#define SST_cmdFifoDepth    0x1f4
#define SST_fbiInit7        0x24c

/* vid_voodoo_regs.h's FBZ_DITHER_2x2, for the undither's log line */
#define FBZ_DITHER_2x2_BIT  (1 << 11)

/* vid_voodoo.c (no upstream header declares these three) */
void   *voodoo_init(const device_t *info);
void    voodoo_close(void *priv);
uint8_t voodoo_pci_read(int func, int addr, int len, void *priv);
void    voodoo_pci_write(int func, int addr, int len, uint8_t val, void *priv);

#define TYPE_VOODOO2 "voodoo2"
OBJECT_DECLARE_SIMPLE_TYPE(Voodoo2State, VOODOO2)

#define VOODOO2_PCI_VENDOR 0x121a
#define VOODOO2_PCI_DEVICE 0x0002
#define VOODOO2_BAR_SIZE   (16 * MiB)
#define VOODOO2_STATS_MS   5000
#define VOODOO2_BLANK_MS   2000   /* a guest that never swaps still shows */

struct Voodoo2State {
    PCIDevice parent_obj;

    MemoryRegion  mmio;
    voodoo_set_t *set;
    voodoo_t     *v;

    /* display */
    QemuConsole    *con;
    DisplaySurface *surface;    /* ours, while the console shows it */
    int             override;
    bool            no_console_warned;
    uint32_t        frames;
    /* scan-outs of a buffer other than the last one: the game's frames as
     * the monitor shows them (at most the refresh rate). v->frame_count
     * cannot say this, because 86Box counts only swaps that wait for a retrace */
    uint32_t        shown;
    uint32_t        shown_front;
    /* black until the guest's first swap after the monitor changes hands or
     * size: what the frame buffer holds then is the driver's memory test
     * or the last mode's lines at the new pitch (a gray pattern), which a
     * real monitor never showed because it was re-locking to the timings */
    bool            blank;
    bool            blank_swapped;  /* seen; the next frame is all new lines */
    uint32_t        blank_front;
    int             blank_frames;
    int64_t         blank_since;
    int             shown_w, shown_h;

    /* the 5 s activity line */
    QEMUTimer *stats;
    uint32_t   last_frames;
    uint32_t   last_shown;
    int        last_tris;
    int        last_wr;
    int        last_rd;
    int        last_tex;
    unsigned   last_fatals;
    uint32_t   fifo_off_writes;   /* FIFO-window packets met with the FIFO off */
    uint32_t   last_fifo_off;
    bool       fifo_off_warned;
    /* which registers those dwords would have been, as a bitmap over
     * (addr & 0x3fc) >> 2: a stranded client's packets walk the file at
     * whatever offsets its addresses land on, and the 5 s line names them,
     * because the question a refusal raises is whether one of them was the
     * guest's own way back (fbiInit7 with bit 8) rather than a packet */
    uint32_t   fifo_off_seen[8];
    /* Who last ran sst1InitRegisters (an initEnable of exactly 1), kept for
     * the report the first such packet makes: the process that wrote the
     * packet against the one that switched the FIFO off under it. The same
     * cr3 in both is one program re-initialising under itself; a different
     * one is a second Glide client (doc 21 §11). */
    char       init_who[512];

    /* the command FIFO in RAM (ramfifo=on) */
    uint8_t     *fb_86box;       /* 86Box's own fb_mem, given back at close */
    MemoryRegion fb_ram;         /* the frame buffer as RAM, never mapped */
    MemoryRegion fifo_win;       /* an alias of the ring at BAR 0x200000 */
    bool         fifo_mapped;
    bool         fifo_broken;    /* a packet not followed: MMIO for good */
    bool         fifo_stray_warned;
    uint32_t     fifo_base;      /* frame-buffer addresses of the ring */
    uint32_t     fifo_size;
    uint32_t     fifo_parse;     /* the next header nobody has counted */
    uint32_t     fifo_poisoned;  /* consumed words poisoned up to here */
    uint32_t     fifo_words, last_fifo_words;
    uint32_t     fifo_syncs, last_fifo_syncs;
    /* Glide's device probe (cvg/init/info.c) with the FIFO off: what it
     * reads back through the LFB, a line per read, capped per window */
    uint32_t     probe_fbzcp;    /* the last fbzColorPath written directly */
    uint32_t     probe_lines;
    /* the consumer waiting inside a packet while the guest waits for idle */
    uint32_t     stall_windows;
    bool         stall_dumped;
    uint32_t     fifo_narrow;    /* packet words written narrower than a dword */
    bool         fifo_narrow_warned;
    bool         packed_alpha_seen;  /* the packet patch 71 is about */
    struct {                         /* the last packets the walk counted */
        uint32_t addr, hdr, n;
    } seen[24];
    uint32_t     seen_n;
    uint32_t     in_packet;          /* words of the current packet still to count */
    uint32_t     partial;            /* packets the guest was still writing */
    uint32_t     trig_addr;          /* the guest access this walk runs from */
    uint8_t      trig_size;
    bool         trig_write;
    /* putting the ring back in front of what came after it ("one order") */
    uint32_t     drains;             /* waits for the ring before an access */
    uint32_t     last_drains;
    uint32_t     behind;             /* publishes with LFB writes still queued */
    uint32_t     last_behind;
    /* the ring through MMIO, counted the way the chip counts it: only words
     * every predecessor of which has arrived ("the command FIFO through
     * MMIO" below) */
    uint32_t     mmio_next;          /* window offset of the next word in address order */
    uint64_t     mmio_ahead;         /* bit k: the word k past it has arrived already */
    uint32_t     mmio_holes;         /* words that arrived ahead of one before them */
    uint32_t     last_mmio_holes;
    uint32_t     mmio_flushed;       /* counted with a hole still open */
    uint32_t     last_mmio_flushed;
    uint32_t     mmio_in_packet;     /* data words left of the packet being counted */
    uint32_t     mmio_jumps;         /* JMP packets followed on the write side */
    uint32_t     last_mmio_jumps;
    bool         mmio_holes_noted;
    bool         mmio_flushed_warned;
    int64_t      idle_status_since;  /* first of a run of idle-busy polls */
    bool         idle_status_noted;
    /* the LFB writes of a window, by the buffer the guest aimed them at:
     * which one a game's HUD goes into is the question they answer */
    uint32_t     lfb_front, lfb_back, lfb_else;
    uint32_t     last_lfb_front, last_lfb_back, last_lfb_else;
    uint32_t     lfb_y_lo, lfb_y_hi;
    bool         drain_slow_warned;  /* one that did not finish in time */
    bool         fifo_left_noted;    /* words stranded by the FIFO going off */
    /* register-window accesses by register (addr & 0x3fc) since the last
     * line: a guest that spins on one register names it here */
    uint32_t   rd_hist[256];
    uint32_t   wr_hist[256];
    uint32_t   cfg_hist[64];    /* configuration-space reads, by dword */
    /* the last accesses, for a fatal(): what the guest was doing */
    struct {
        uint32_t addr;
        uint32_t val;
        uint8_t  size;
        uint8_t  write;
    } ring[64];
    uint32_t   ring_n;

    /* properties */
    uint32_t fbmem_mb;
    uint32_t texmem_mb;
    uint32_t threads;
    bool     bilinear;
    bool     dithersub;
    bool     filter;
    bool     undither;
    bool     recompiler;
    bool     ramfifo;
    bool     lfb_order;
    bool     fifo_off_regs;
    bool     count_holes;   /* count the MMIO ring contiguously (the chip's way) */

    /* the undither's one-shot note: it says once that it is on, and once
     * why it is not, because a frame it declines is an ordinary frame and
     * nothing else would show that the setting did nothing */
    bool        undither_on_noted;
    const char *undither_why_noted;
};

/* ------------------------------------------------------------------ MMIO */

static bool     voodoo2_trace;          /* VOODOO2_TRACE=1 in the environment */
static bool     voodoo2_trace_where_armed; /* name the module at the next write */
static uint32_t voodoo2_trace_addr;     /* the address of the held-back read run */
static uint32_t voodoo2_trace_val;      /* what the last of them answered */
static uint32_t voodoo2_trace_n;        /* how many of them there have been */

/* A spin is millions of reads of one register a second (the status poll,
 * or cmdFifoRdPtr with the FIFO in RAM), so a run of reads of one address
 * is held back and printed as a count when anything else happens. What the
 * trace is for is the writes between the spins. */
static void
voodoo2_trace_flush(void)
{
    if (!voodoo2_trace_n) {
        return;
    }
    if (voodoo2_trace_n == 1) {
        fprintf(stderr, "voodoo2: rd %06x %08x\n", voodoo2_trace_addr, voodoo2_trace_val);
    } else {
        fprintf(stderr, "voodoo2: rd %06x %08x x%u\n", voodoo2_trace_addr,
                voodoo2_trace_val, voodoo2_trace_n);
    }
    voodoo2_trace_n = 0;
}

static void voodoo2_trace_where(const char *why);

static inline void
voodoo2_note(Voodoo2State *s, hwaddr addr, uint64_t val, unsigned size, bool write)
{
    unsigned i = s->ring_n++ % ARRAY_SIZE(s->ring);

    s->ring[i].addr  = (uint32_t) addr;
    s->ring[i].val   = (uint32_t) val;
    s->ring[i].size  = size;
    s->ring[i].write = write;
    if (voodoo2_trace && addr < 0x400000) {
        /* every register- and command-FIFO-window access, a spin on one
         * register (the status poll, cmdFifoRdPtr) as one count */
        if (!write) {
            if (voodoo2_trace_n && (uint32_t) addr == voodoo2_trace_addr) {
                voodoo2_trace_val = (uint32_t) val;
                voodoo2_trace_n++;
                return;
            }
            voodoo2_trace_flush();
            voodoo2_trace_addr = (uint32_t) addr;
            voodoo2_trace_val  = (uint32_t) val;
            voodoo2_trace_n    = 1;
            return;
        }
        voodoo2_trace_flush();
        if (voodoo2_trace_where_armed) {
            /* the first write after an initEnable: whose init this is. The
             * first packet met with the FIFO off names its own writer
             * without the trace (voodoo2_mmio_write). */
            voodoo2_trace_where_armed = false;
            voodoo2_trace_where("writer");
        }
        fprintf(stderr, "voodoo2: wr %06x %08x (initEnable %08x%s)\n",
                (unsigned) addr, (unsigned) val, s->v->initEnable,
                s->v->cmdfifo_enabled ? ", fifo on" : "");
    }
}

/* Which guest module is touching the card: the PE image around a linear
 * address, found by walking back page by page to its MZ header and reading
 * its export directory's name. Trace mode only. It exists because a
 * Win9x Glide process can hold more than one copy of 3dfx's init library
 * (GLIDE2X.DLL, the OEM DLL, the splash DLL), each with its own idea of
 * what state the card is in, and a register trace alone cannot say whose
 * write is whose. */
static bool
voodoo2_guest_read(CPUState *cs, vaddr a, void *buf, int len)
{
    /* RAM only: a linear address can map the card's own BAR (or any other
     * device), and a debug read of it is an MMIO access from inside this
     * device's handler. QEMU blocks it as re-entrant, and a read of the
     * status register is not free anyway. Every page the read touches is
     * checked. */
    for (vaddr p = a & TARGET_PAGE_MASK; p < a + len; p += TARGET_PAGE_SIZE) {
        TranslateForDebugResult t;
        hwaddr        phys;
        MemoryRegion *mr;
        hwaddr        xlat, plen = 1;
        bool          ram;

        if (!cpu_translate_for_debug(cs, p, &t)) {
            return false;
        }
        phys = t.physaddr;
        RCU_READ_LOCK_GUARD();
        mr  = address_space_translate(&address_space_memory, phys, &xlat, &plen,
                                      false, MEMTXATTRS_UNSPECIFIED);
        ram = memory_region_is_ram(mr) && !memory_region_is_ram_device(mr);
        if (!ram) {
            return false;
        }
    }
    return cpu_memory_rw_debug(cs, a, buf, len, false) == 0;
}

static bool
voodoo2_guest_module(CPUState *cs, uint32_t a, uint32_t *base, char *name, size_t len)
{
    uint32_t page = a & ~0xfffu;

    for (int k = 0; k < 4096 && page >= 0x1000; k++, page -= 0x1000) {
        uint8_t  mz[2];
        uint32_t lfanew, pe, ednames[2] = { 0, 0 }, edir;

        if (!voodoo2_guest_read(cs, page, mz, 2) || mz[0] != 'M' || mz[1] != 'Z' ||
            !voodoo2_guest_read(cs, page + 0x3c, &lfanew, 4) || lfanew > 0x1000 ||
            !voodoo2_guest_read(cs, page + lfanew, &pe, 4) || pe != 0x00004550) {
            continue;
        }
        *base = page;
        {
            uint32_t soi = 0;

            voodoo2_guest_read(cs, page + lfanew + 24 + 56, &soi, 4);
            snprintf(name, len, "[img %08x size %x]", page, soi);
        }
        /* PE32 optional header at +24, export data directory at +96 of it */
        if (voodoo2_guest_read(cs, page + lfanew + 24 + 96, &edir, 4) && edir &&
            voodoo2_guest_read(cs, page + edir + 12, ednames, 4)) {
            char buf[40] = "";

            if (voodoo2_guest_read(cs, page + ednames[0], buf, sizeof(buf) - 1)) {
                buf[sizeof(buf) - 1] = 0;
                snprintf(name, len, "%s", buf);
            }
        }
        return true;
    }
    return false;
}

/* Where the guest is, as one line: the module and offset the program
 * counter is in, the callers above it and the code around it. Empty when
 * there is no CPU to ask (a timer, the FIFO thread). */
static void
voodoo2_where(char *line, size_t len)
{
    CPUState    *cs = current_cpu;
    CPUX86State *env;
    uint32_t     pc, esp, base = 0, stack[96];
    char         name[40];
    size_t       n = 0;

    line[0] = 0;
    if (!cs) {
        return;
    }
    env = cpu_env(cs);
    /* eip is the start of the current translation block or later, which is
     * inside the same function, which is all this needs */
    pc  = (uint32_t) (env->segs[R_CS].base + env->eip);
    esp = (uint32_t) (env->segs[R_SS].base + env->regs[R_ESP]);
    n += snprintf(line + n, len - n, "cr3 %08x pc %08x: ",
                  (uint32_t) env->cr[3], pc);
    if (voodoo2_guest_module(cs, pc, &base, name, sizeof(name))) {
        n += snprintf(line + n, len - n, "%s+%x", name, pc - base);
    } else {
        n += snprintf(line + n, len - n, "%08x", pc);
    }
    /* callers: every stack word that lands in a module other than the last
     * one named, innermost first */
    if (voodoo2_guest_read(cs, esp, stack, sizeof(stack))) {
        uint32_t last = base;

        for (unsigned i = 0; i < ARRAY_SIZE(stack) && n < len - 48; i++) {
            uint32_t b;
            char     nm[40];

            if (stack[i] < 0x00400000 || (stack[i] >= last && stack[i] < last + 0x100000) ||
                !voodoo2_guest_module(cs, stack[i], &b, nm, sizeof(nm)) || b == last) {
                continue;
            }
            n += snprintf(line + n, len - n, " <- %s+%x", nm, stack[i] - b);
            last = b;
        }
    }
    {
        uint8_t code[24];

        if (voodoo2_guest_read(cs, pc & ~0xfu, code, sizeof(code))) {
            n += snprintf(line + n, len - n, " [code@%08x", pc & ~0xfu);
            for (unsigned i = 0; i < sizeof(code) && n < len - 4; i++) {
                n += snprintf(line + n, len - n, " %02x", code[i]);
            }
            n += snprintf(line + n, len - n, "]");
        }
    }
}

static void
voodoo2_trace_where(const char *why)
{
    char line[512];

    voodoo2_where(line, sizeof(line));
    if (line[0]) {
        fprintf(stderr, "voodoo2: %s: %s\n", why, line);
    }
}

/* the last accesses the guest made, oldest first */
static void
voodoo2_dump_accesses(Voodoo2State *s)
{
    unsigned n = MIN(s->ring_n, ARRAY_SIZE(s->ring));

    fprintf(stderr, "voodoo2: the last %u accesses, oldest first:\n", n);
    for (unsigned k = 0; k < n; k++) {
        unsigned i = (s->ring_n - n + k) % ARRAY_SIZE(s->ring);

        fprintf(stderr, "voodoo2:   %s %u %06x %08x\n",
                s->ring[i].write ? "wr" : "rd", s->ring[i].size,
                s->ring[i].addr, s->ring[i].val);
    }
}

/* 86Box's fatal(): the state a bug report needs, before the abort */
static void
voodoo2_on_fatal(void *opaque)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;

    voodoo2_trace_flush();
    fprintf(stderr, "voodoo2: initEnable %08x fbiInit0 %08x fbiInit7 %08x "
            "cmdfifo %s base %08x end %08x rp %08x depth wr %u rd %u; "
            "%dx%d, %s\n",
            v->initEnable, v->fbiInit0, v->fbiInit7,
            v->cmdfifo_enabled ? "on" : "off", v->cmdfifo_base, v->cmdfifo_end,
            v->cmdfifo_rp, v->cmdfifo_depth_wr, v->cmdfifo_depth_rd,
            v->h_disp, v->v_disp, s->override ? "monitor" : "no monitor");
    voodoo2_dump_accesses(s);
}

/* -------------------------------------------------- the command FIFO in RAM
 *
 * With the ring as RAM the device sees none of the guest's packet writes,
 * and 3dfx's Glide tells the chip nothing either: it leaves hole counting
 * on (cvg/init/util.c), i.e. the chip executes whatever it has seen written
 * contiguously from its read pointer. So the device finds out itself, at
 * the guest's next access to anything else on the card (a status poll, a
 * register, the LFB), and what it counts is **words, not packets**: as
 * many as the guest has written from the last one counted on, which go to
 * 86Box's consumer as the depth the per-dword writes used to add. The
 * consumer then parses them and blocks inside a packet whose rest has not
 * arrived, which is what the chip does.
 *
 * It counted whole packets once, on the header's word count. Glide does
 * write each packet whole before it touches the card again (the 5 s line
 * counts the ones met half-written), so counting words changes nothing in
 * practice. It is what the chip does, and one assumption fewer between a
 * guest and a hang.
 *
 * **The read pointer never passes what the guest has written.** That is the
 * one invariant this reconstruction owes the guest, because Glide's free
 * space is `rp - wp - 1` and a pointer past `wp` turns the whole ring into
 * a few words of room: the guest then waits for space on a ring that is
 * empty, for ever. It is also a state the chip cannot reach, which is the
 * short way to say the same thing. So a word that reads as poison is never
 * taken on a guess about what the guest must have meant. Waiting is what
 * the chip does, and a stall where the pointer is honest can at least be
 * read (FIFA 2000, doc 21 §9).
 *
 * Where the guest has not written yet is told by poison: a word the
 * consumer has taken is set to VOODOO2_FIFO_POISON before the guest can
 * learn that its slot is free. The only way it learns that is reading
 * cmdFifoRdPtr, and that read is answered here, after the poisoning, with
 * the pointer it poisoned up to. The value is one no guest writes (see the
 * define), because a poison word that a guest could also mean as data stops
 * the FIFO on the guest's own bytes; a word that still reads as poison is
 * taken as written anyway when a word close after it is, since the guest
 * fills the ring in address order. A packet this cannot follow (a JSR, AGP,
 * a Banshee type) is warned about once and the window goes back to MMIO;
 * ramfifo=off is the A/B.
 */
#define VOODOO2_FIFO_WIN     0x200000
#define VOODOO2_FIFO_WIN_MAX 0x40000    /* the window decodes addr & 0x3fffc */
/* The mark for "the guest has not written here". It only has to be a word
 * the chip could never run (the low three bits are the packet type, and
 * type 7 does not exist), so the rest of it is chosen to be a word no guest
 * would write: as a float it is -2.5e18, as a pair of 16-bit texels an odd
 * dark blue beside a dirty pink, and in a hex dump it says what it is. It
 * was 0xffffffff once, which is a white texel: a texture with white in it
 * read as unwritten ring and the FIFO stopped on the guest's own data
 * (3DMark 99's loading screen). */
#define VOODOO2_FIFO_POISON  0xdeadbee7u
#define VOODOO2_FIFO_AHEAD   8          /* words looked past a poison-looking
                                         * data word for one the guest wrote */

static inline uint32_t *
voodoo2_fifo_word(voodoo_t *v, uint32_t a)
{
    return (uint32_t *) &v->fb_mem[a & v->fb_mask];
}

/* the words a packet takes, header included, exactly as 86Box's consumer
 * takes them (vid_voodoo_fifo.c); 0 for one this does not follow */
static uint32_t
voodoo2_packet_words(uint32_t h)
{
    uint32_t pv;
    uint32_t n;

    switch (h & 7) {
    case 0:
        switch ((h >> 3) & 7) {
        case 0:     /* NOP */
        case 3:     /* JMP to the frame buffer: the caller follows it */
            return 1;
        default:    /* JSR/RET (a subroutine elsewhere), JMP AGP, reserved */
            return 0;
        }
    case 1:
        return 1 + (h >> 16);
    case 2:
        return 1 + ctpop32(h >> 3);
    case 3:
        pv = 2;                                         /* x, y */
        if (h & (1u << 28)) {
            /* one packed ARGB word, there whenever either parameter is
             * named. Glide sends iterated alpha over a constant colour
             * with the packed bit set and the RGB bit clear (patch 71) */
            if (h & ((1 << 10) | (1 << 11))) {
                pv++;
            }
        } else {
            if (h & (1 << 10)) {
                pv += 3;                                /* RGB */
            }
            if (h & (1 << 11)) {
                pv++;                                   /* alpha */
            }
        }
        pv += !!(h & (1 << 12)) + !!(h & (1 << 13)) + !!(h & (1 << 14));
        pv += 2 * !!(h & (1 << 15));                    /* s0, t0 */
        pv += !!(h & (1 << 16));                        /* w1 */
        pv += 2 * !!(h & (1 << 17));                    /* s1, t1 */
        return 1 + ((h >> 6) & 0xf) * pv + ((h >> 29) & 7);
    case 4:
        return 1 + ctpop32((h >> 15) & 0x3fff) + ((h >> 29) & 7);
    case 5:
        n = (h >> 3) & 0x7ffff;
        return 2 + (n ? n : 1);
    default:
        return 0;
    }
}

/* Has the guest written the word at `a`? Poison says no, unless the guest
 * has written a word soon after it, which it can only have done by writing
 * this one first (it fills the ring in address order), so the poison there
 * is its own data. */
static bool
voodoo2_fifo_written(Voodoo2State *s, uint32_t a)
{
    voodoo_t *v   = s->v;
    uint32_t  end = s->fifo_base + s->fifo_size;

    if (*voodoo2_fifo_word(v, a) != VOODOO2_FIFO_POISON) {
        return true;
    }
    for (uint32_t k = 1; k <= VOODOO2_FIFO_AHEAD; k++) {
        uint32_t b = a + 4 * k;

        if (b >= end) {
            break;          /* the ring's end: the guest jumps, not runs on */
        }
        if (*voodoo2_fifo_word(v, b) != VOODOO2_FIFO_POISON) {
            return true;
        }
    }
    return false;
}

static void voodoo2_fifo_map(Voodoo2State *s);
static void voodoo2_mmio_drain(Voodoo2State *s);
static void voodoo2_mmio_ring_reset(Voodoo2State *s);

static void
voodoo2_fifo_break(Voodoo2State *s, uint32_t a, uint32_t h, const char *why)
{
    s->fifo_broken = true;
    warn_report("voodoo2: command FIFO back to MMIO: %s (header %08x at %08x, "
                "ring %08x+%x); ramfifo=off is the A/B", why, h, a,
                s->fifo_base, s->fifo_size);
    voodoo2_fifo_map(s);
}

/* count every word the guest has written since the last call, packet by
 * packet so a jump is followed, and word by word inside one so a packet
 * still being written goes to the consumer as far as it has got */
static void
voodoo2_fifo_sync(Voodoo2State *s)
{
    voodoo_t *v     = s->v;
    uint32_t  end   = s->fifo_base + s->fifo_size;
    uint32_t  a     = s->fifo_parse;
    uint32_t  words = 0;

    if (a < s->fifo_base || a >= end) {
        return;     /* the read pointer is not in the ring yet */
    }
    s->fifo_syncs++;
    while (words < s->fifo_size / 4) {
        uint32_t h;
        uint32_t n;

        /* inside a packet whose rest was not there last time: take what has
         * arrived since, and leave the consumer waiting for the remainder
         * exactly where the chip would wait */
        if (s->in_packet) {
            uint32_t take = 0;

            while (take < s->in_packet && voodoo2_fifo_written(s, a + 4 * take)) {
                take++;
            }
            /* Nothing is ever taken that the guest has not written, however
             * long the wait looks. There used to be a last
             * resort here: after 64 rdptr polls with the chip caught up, the
             * rest of the packet was taken as data on the argument that a
             * guest polling an empty ring cannot be waiting for room. The
             * argument eats itself. Taking words the guest has not written
             * puts the read pointer *past* its write pointer, which is a
             * state no chip can be in, and the free space Glide computes
             * from that pointer is then a few words instead of the whole
             * ring, so the guest waits for room for ever and the ring it is
             * waiting on stays empty. FIFA 2000's loading screen died on
             * exactly that: a 66-word LFB packet with 19 words written, 47
             * taken, and a guest with 46 words of room asking for 66. The
             * case the last resort was for (real data that reads as poison
             * for longer than the look ahead) was closed at its root when
             * the poison word stopped being 0xffffffff. A run
             * of eight dwords of a guest's own data all reading 0xdeadbee7
             * is not a trade worth a deadlock. `ramfifo=off` is the A/B, and
             * it is the transport that needs no guessing at all. */
            if (!take) {
                break;
            }
            words       += take;
            a           += 4 * take;
            s->in_packet -= take;
            continue;
        }
        h = *voodoo2_fifo_word(v, a);
        if (h == VOODOO2_FIFO_POISON) {
            /* A header is the first word the guest writes of its packet, so
             * poison there is a gap and never data waiting to be recognised:
             * no look ahead here (it took a poison header for a packet and
             * dropped the window back to MMIO mid-stream). */
            break;
        }
        if ((h & 7) == 3 && (h & (1u << 28)) && !(h & (1 << 10)) &&
            (h & (1 << 11)) && !s->packed_alpha_seen) {
            /* the packet patch 71 is about, in this guest's own stream */
            s->packed_alpha_seen = true;
            info_report("voodoo2: packet 3 with a packed colour word and no "
                        "RGB bit (header %08x): iterated alpha over a constant "
                        "colour, one word a vertex (patch 71)", h);
        }
        n = voodoo2_packet_words(h);
        if (!n) {
            voodoo2_fifo_break(s, a, h, "a packet it does not follow");
            break;
        }
        if ((h & 0x3f) == 0x18) {       /* JMP */
            uint32_t to = (h >> 4) & 0xfffffc;

            if (to < s->fifo_base || to >= end) {
                voodoo2_fifo_break(s, a, h, "a jump out of the ring");
                break;
            }
            words++;
            a = to;
            continue;
        }
        if (a + 4 * n > end) {
            voodoo2_fifo_break(s, a, h, "a packet across the ring's end");
            break;
        }
        /* the last packets this walk counted, for the stall dump: a stream
         * that stops is read backwards from here */
        s->seen[s->seen_n % ARRAY_SIZE(s->seen)].addr = a;
        s->seen[s->seen_n % ARRAY_SIZE(s->seen)].hdr  = h;
        s->seen[s->seen_n % ARRAY_SIZE(s->seen)].n    = n;
        s->seen_n++;
        /* the header is written; its words are counted as they arrive, this
         * call or a later one (the guest writes a long packet in pieces and
         * waits for the chip in between: 3DMark 99 on the PC) */
        if (n > 1 && !voodoo2_fifo_written(s, a + 4 * (n - 1))) {
            s->partial++;
        }
        s->in_packet = n;
    }
    s->fifo_parse = a;
    if (words) {
        /* Everything the guest wrote *before* these packets has to be on the
         * card first, and an LFB or texture write it made is sitting in
         * 86Box's other queue ("one order" below). Publishing without
         * emptying that queue is what lets a swap in the ring overtake a
         * HUD written through the LFB. Not while the consumer may be parked
         * inside a packet of ours, though: then it is waiting for exactly
         * these words and nothing else will empty that queue. */
        if (!s->in_packet) {
            voodoo2_mmio_drain(s);
        }
        smp_wmb();      /* the words before the depth that says they are there */
        v->cmdfifo_depth_wr += words;
        s->fifo_words       += words;
        voodoo_wake_fifo_thread_now(v);
    }
}

/* the guest asks where the chip is: poison what it has taken since the last
 * time, then tell. The guest never writes past what it was told */
static uint32_t
voodoo2_fifo_rdptr(Voodoo2State *s)
{
    voodoo_t *v   = s->v;
    uint32_t  rp  = qatomic_read(&v->cmdfifo_rp);
    uint32_t  end = s->fifo_base + s->fifo_size;
    uint32_t  a   = s->fifo_poisoned;

    if (rp < s->fifo_base || rp >= end || a < s->fifo_base || a >= end) {
        return rp;
    }
    for (uint32_t k = 0; a != rp && k < s->fifo_size / 4; k++) {
        *voodoo2_fifo_word(v, a) = VOODOO2_FIFO_POISON;
        a += 4;
        if (a >= end) {
            a = s->fifo_base;
        }
    }
    s->fifo_poisoned = rp;
    return rp;
}

/* a fresh ring: all poison, counted and poisoned from the read pointer */
static void
voodoo2_fifo_restart(Voodoo2State *s)
{
    voodoo_t *v = s->v;

    /* Every word of the ring goes back to poison, so anything the guest has
     * already written and the chip has not run yet is gone with it. That is
     * right at an init (Glide sets the pointers and starts afresh) and
     * wrong at any other moment, which is why every one is named here
     * (a ring restart under a live Glide would look exactly like
     * the hang being chased). */
    info_report("voodoo2: the command ring is poisoned afresh (ring %08x+%x, "
                "rp %08x, %u words counted so far)", s->fifo_base, s->fifo_size,
                (uint32_t) v->cmdfifo_rp, s->fifo_words);
    for (uint32_t a = s->fifo_base; a < s->fifo_base + s->fifo_size; a += 4) {
        *voodoo2_fifo_word(v, a) = VOODOO2_FIFO_POISON;
    }
    s->fifo_parse    = v->cmdfifo_rp;
    s->fifo_poisoned = v->cmdfifo_rp;
    s->in_packet     = 0;
}

/* map the ring as RAM when the FIFO is on and the ring fits the window, and
 * back to MMIO when either stops being so */
static void
voodoo2_fifo_map(Voodoo2State *s)
{
    voodoo_t *v    = s->v;
    uint32_t  base = v->cmdfifo_base;
    uint32_t  size = v->cmdfifo_end + 0x1000 - base;
    bool      want = s->ramfifo && s->fb_86box && !s->fifo_broken &&
                     v->cmdfifo_enabled && !v->cmdfifo_in_agp &&
                     v->cmdfifo_end >= base && size <= VOODOO2_FIFO_WIN_MAX &&
                     base + size <= v->fb_mask + 1;

    if (want == s->fifo_mapped &&
        (!want || (base == s->fifo_base && size == s->fifo_size))) {
        return;
    }
    memory_region_transaction_begin();
    if (want) {
        s->fifo_base = base;
        s->fifo_size = size;
        voodoo2_fifo_restart(s);
        memory_region_set_alias_offset(&s->fifo_win, base);
        memory_region_set_size(&s->fifo_win, size);
    }
    memory_region_set_enabled(&s->fifo_win, want);
    memory_region_transaction_commit();
    s->fifo_mapped = want;
    voodoo2_mmio_ring_reset(s);
    info_report("voodoo2: command FIFO %s (ring %08x+%x)",
                want ? "in RAM" : "through MMIO", base, size);
}

/* after a trapped write: the FIFO's own registers may have moved the ring */
static void
voodoo2_fifo_after_write(Voodoo2State *s, hwaddr addr)
{
    if (addr >= 0x400000 || !s->ramfifo || !s->fb_86box) {
        return;
    }
    if (addr & VOODOO2_FIFO_WIN) {
        if (s->fifo_mapped && !s->fifo_stray_warned) {
            s->fifo_stray_warned = true;
            warn_report("voodoo2: a command-FIFO write at %06x, outside the "
                        "ring mapped as RAM (%08x+%x)", (unsigned) addr,
                        s->fifo_base, s->fifo_size);
        }
        return;
    }
    switch (addr & 0x3fc) {
    case SST_cmdFifoRdPtr:
    case SST_cmdFifoDepth:
        if (s->fifo_mapped) {
            voodoo2_fifo_restart(s);
        }
        /* fall through */
    case SST_cmdFifoBaseAddr:
    case SST_fbiInit7:
        voodoo2_fifo_map(s);
        break;
    default:
        break;
    }
}

/* --------------------------------------------------------------- one order
 *
 * The chip has one way in from the PCI bus: a packet in the command FIFO, a
 * write into the LFB or texture aperture, and a register write all reach it
 * in the order the guest made them. 86Box has two queues, `voodoo->fifo`
 * (where a frame-buffer or texture write is queued) and the ring, and its
 * thread empties the whole of the first before it looks at the second
 * (`vid_voodoo_fifo.c`); a register the vCPU thread writes here does not
 * queue at all. So the ring is the stream that can be overtaken.
 *
 * **That is what hangs a guest at the end, and it is measured.** Glide's
 * grSstWinClose writes its last packets and then clears fbiInit7's
 * command-FIFO bit. The register write lands at once while the ring is
 * still being consumed, and 86Box's consumer loop ends the moment
 * `cmdfifo_enabled` goes false, so whatever it had not reached is never run.
 * `SST_status`'s busy bit is `cmdfifo_depth_rd != cmdfifo_depth_wr`,
 * so the card reads busy to every later poll. That poll is Glide's own
 * grSstIdle. Carmageddon's 3dfx build hung there with two
 * words outstanding of the 104 the last walk counted, the card otherwise
 * entirely idle: `busy: 0 cmds outstanding (wr 49668 rd 49668), fifo depth
 * 59495109/59495111`, and 27 million reads of register 0x000 in five
 * seconds. So a write that reconfigures the FIFO runs the ring out first,
 * always; the teardown phase of tools/voodoo-guest-test.py is the check.
 *
 * **And the other direction is the flashing HUD.** What the guest wrote
 * through the LFB *before* a batch of packets has to be on the card before
 * they are, and 86Box's thread empties its MMIO queue only between passes
 * over the ring, which in a race never empties: measured ~19,000 words
 * behind, every 5 s line of Carmageddon's race. So the HUD the game writes
 * with grLfbWriteRegion sits in that queue while the swap that follows it in
 * the ring is consumed, and lands in the buffer the swap has just turned
 * into the back one: a frame late, or not at all. The user's screenshots
 * showed it: the panels Carmageddon draws as geometry are there in both,
 * and the sprites it writes through the LFB are in one and gone in the next.
 * voodoo2_mmio_drain() is at the ring's publish point for that, and it is
 * the ordering that matters, so it is unconditional.
 *
 * **What `lfb-order` (off) adds** is the mirror of it: a wait for the ring
 * before an LFB or texture write, so a packet already counted is drawn
 * first. That window is only as wide as the consumer's wake, and the
 * ordering phase of the guest test measures it away on an unloaded host
 * (the block lands on top with the switch either way), so it is kept as the
 * A/B rather than turned on, because it costs a wait for the rasterizer at
 * every ring-then-LFB turn. The 5 s line counts both kinds of wait.
 */
#define VOODOO2_DRAIN_MS 250

static bool
voodoo2_fifo_caught_up(voodoo_t *v)
{
    return ATOMIC_LOAD(v->cmdfifo_depth_rd) == ATOMIC_LOAD(v->cmdfifo_depth_wr) &&
           !v->cmdfifo_in_sub;
}

/* Run out what the walk has counted, so what follows is ordered behind it.
 * The ring only: waiting for 86Box's own FIFO here as well would mean
 * holding `flush` while a swap goes past, and `flush` flips the buffer
 * where it stands instead of at the retrace. Measured, it tore the
 * teardown scene's frame 64 rows down. Patch 72's marks
 * survive the counter reset this write is about on their own: one the ring
 * can never reach is taken as due rather than waited for.
 *
 * Bounded, and the bound is not a formality: the consumer waits inside a
 * packet whose rest the guest has not written, which leaves the depths
 * equal (so that case returns at once), but a stream this walk has
 * mis-counted leaves them apart with nobody coming, and the guest must not
 * be stopped with it. */
static bool
voodoo2_fifo_drain(Voodoo2State *s, const char *why)
{
    voodoo_t *v = s->v;
    int64_t   deadline;

    if (voodoo2_fifo_caught_up(v)) {
        return true;
    }
    s->drains++;
    deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + VOODOO2_DRAIN_MS;
    /* 86Box's own escape for a thread draining from the guest's side: with
     * `flush` set, a swap in the ring completes without waiting for a
     * retrace the display timer cannot deliver while this holds the BQL */
    v->flush = 1;
    while (!voodoo2_fifo_caught_up(v)) {
        voodoo_wake_fifo_thread_now(v);
        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) >= deadline) {
            v->flush = 0;
            if (!s->drain_slow_warned) {
                s->drain_slow_warned = true;
                warn_report("voodoo2: the command FIFO did not run out in %d ms "
                            "at %s (%u word(s) left of %u counted); the guest "
                            "carries on and what follows may be drawn out of "
                            "order",
                            VOODOO2_DRAIN_MS, why,
                            (unsigned) (v->cmdfifo_depth_wr - v->cmdfifo_depth_rd),
                            s->fifo_words);
            }
            return false;
        }
        g_usleep(50);
    }
    v->flush = 0;
    return true;
}

/* The other direction, and the one that matters in a game: what the guest
 * wrote through the LFB *before* the packets about to be published has to
 * be on the card first ("one order" above, Carmageddon's flashing HUD).
 *
 * **Patch 72 does the ordering**, in the thread where it belongs: the ring
 * loop yields the moment anything appears in the other FIFO, and the pass
 * repeats. Nothing waits anywhere. The vCPU used to wait here instead, and
 * the measurement that ended that is Carmageddon's own: 3,200 waits and
 * 1.8 s of vCPU time per 5 s (~13 LFB-then-ring turns a frame, one per
 * HUD element) for an ordering the consumer can keep by itself.
 *
 * What is left here is the count. It is the number of times the ring
 * published words with the other FIFO not yet empty, which is the situation
 * patch 72 handles, and the one case neither can reach: a consumer already
 * parked inside cmdfifo_get waiting for the rest of a packet the guest has
 * not finished writing. That shows as `partial` in the same line. */
static void
voodoo2_mmio_drain(Voodoo2State *s)
{
    voodoo_t *v = s->v;

    if (ATOMIC_LOAD(v->fifo_read_idx) != ATOMIC_LOAD(v->fifo_write_idx)) {
        s->behind++;                    /* not FIFO_EMPTY */
    }
}

/* --------------------------------------- the command FIFO through MMIO
 *
 * With `ramfifo=off` every dword the guest puts in the 0x200000 window is
 * trapped here, and 86Box's handler stores it in the ring and counts it
 * (`cmdfifo_depth_wr++` per write, whatever its address), while its
 * consumer reads the ring one word at a time from the read pointer, as far
 * as that count lets it. That is one assumption too many, because **Glide
 * does not write the ring in address order**: a two-word packet (one
 * register and its value, such as `color1`, `fastfillCMD`, `swapbufferCMD`)
 * goes out value first and header second, so the chip never sees a header
 * whose value is missing. The chip is built for it: cmdFifoAMin / AMax /
 * Holes count what has been written *contiguously*, and the FIFO's depth
 * advances over that alone. 86Box counts the value's write at once, and a
 * consumer sitting on the header's slot (caught up, which at the start
 * of a race it is, the guest being the slow side) reads that slot before
 * the header lands: whatever was there from the last lap or the last
 * session, taken as a header. A stale word that says "256 values follow"
 * eats the next 256 words as data, the first word it then lands on that
 * reads as a Banshee packet is 86Box's `fatal()` (refused here), and from
 * there the read pointer stops where the guest cannot make room, so Glide
 * waits for space on a ring the card will never drain.
 *
 * Measured with Carmageddon's 3dfx build on `ramfifo=off` at a
 * race's start: 182 such pairs among 65,503 ring words, every one the
 * single-register packet; the parser off the rails inside a texture
 * download 2,300 words after the last of them (`Banshee 2D register
 * 00000020=02020202`), then 27 million cmdFifoRdPtr reads per 5 s for
 * ever. The guest's final ring content parses cleanly end to end: it was
 * the transport. The ring in RAM (`ramfifo=on`) never had the problem,
 * because its walk stops on a poison header and counts the pair when both
 * are there.
 *
 * So the word is stored here and counted for the consumer only once every
 * word before it has arrived, as the chip's hole counter does: a word
 * ahead of the next expected one is remembered in a small bitmap and
 * counted when the gap closes. A write below the expected one is the guest
 * continuing at the ring's base after its jump packet (or a ring set up
 * afresh, which also resets this), and the count starts again there; a
 * hole still open at that moment is counted rather than stranded, and
 * said once.
 */
#define VOODOO2_MMIO_UNKNOWN  0xffffffffu
#define VOODOO2_MMIO_AHEAD    64            /* words a hole may span */

/* 86Box's voodoo_writel, the two lines of it this path replaces: the count
 * the consumer reads up to, and its wake when the ring is nearly empty */
static void
voodoo2_mmio_count(voodoo_t *v)
{
    v->cmdfifo_depth_wr++;
    if ((v->cmdfifo_depth_wr - v->cmdfifo_depth_rd) < 20) {
        voodoo_wake_fifo_thread(v);
    }
}

/* The next contiguous word is counted and the expected address moves on:
 * by one word, or to wherever a JMP packet points. The write side follows
 * the packets the way the consumer will, header by header, because that is
 * the only way to know where the guest goes next. Glide writes
 * cmdFifoAMin and AMax once at init and never at a wrap (the trace of
 * Carmageddon's race: three writes to them in a minute, all at
 * grSstWinOpen), so the chip recognises its JMP itself, at the point it
 * becomes contiguous, and so does this. Without it the first packet after
 * every wrap (value first, header second, at the ring's base) was taken
 * as the guest continuing from wherever the value landed, its header then
 * as a jump back, and every word after held as ahead of a hole until the
 * bitmap ran out 64 words on (`moved from 200004 to 200104 with 63 word(s)
 * written past a hole`; five laps of the 359 in that race). Returns true
 * when a JMP moved the address. */
static bool
voodoo2_mmio_step(Voodoo2State *s)
{
    voodoo_t *v   = s->v;
    uint32_t  off = s->mmio_next;
    uint32_t  h, n;

    voodoo2_mmio_count(v);
    if (s->mmio_in_packet) {
        s->mmio_in_packet--;
        s->mmio_next += 4;
        return false;
    }
    h = *(uint32_t *) &v->fb_mem[(v->cmdfifo_base + off) & v->fb_mask];
    if ((h & 0x3f) == 0x18) {
        uint32_t to = (h >> 4) & 0xfffffc;

        if (to >= v->cmdfifo_base &&
            to - v->cmdfifo_base < VOODOO2_FIFO_WIN_MAX) {
            s->mmio_jumps++;
            s->mmio_next = to - v->cmdfifo_base;
            return true;
        }
    }
    n = voodoo2_packet_words(h);
    s->mmio_in_packet = n ? n - 1 : 0;  /* one it does not follow: the consumer refuses it */
    s->mmio_next += 4;
    return false;
}

/* words held ahead of a hole that is not going to close here: counted as
 * they are, and said once */
static void
voodoo2_mmio_flush(Voodoo2State *s, uint32_t addr, const char *why)
{
    unsigned n;

    if (!s->mmio_ahead) {
        return;
    }
    n = ctpop64(s->mmio_ahead);
    s->mmio_flushed += n;
    if (!s->mmio_flushed_warned) {
        s->mmio_flushed_warned = true;
        warn_report("voodoo2: %s at %06x with %u word(s) written past a hole "
                    "at %06x; they are counted as they are, and the consumer "
                    "may read the hole", why, (unsigned) addr, n,
                    (unsigned) (VOODOO2_FIFO_WIN + s->mmio_next));
    }
    while (n--) {
        voodoo2_mmio_count(s->v);
    }
    s->mmio_ahead = 0;
}

static void
voodoo2_mmio_ring_write(Voodoo2State *s, uint32_t addr, uint32_t val)
{
    voodoo_t *v   = s->v;
    uint32_t  off = addr & 0x3fffc;         /* the window decodes this much */
    uint32_t  gap;

    /* the word itself, where 86Box's handler would have put it */
    *(uint32_t *) &v->fb_mem[(v->cmdfifo_base + off) & v->fb_mask] = val;
    v->wr_count++;

    if (s->mmio_next != VOODOO2_MMIO_UNKNOWN && off > s->mmio_next &&
        (gap = (off - s->mmio_next) / 4) < VOODOO2_MMIO_AHEAD) {
        /* ahead of a word not written yet: held until the gap closes */
        s->mmio_ahead |= 1ull << gap;
        s->mmio_holes++;
        if (!s->mmio_holes_noted) {
            s->mmio_holes_noted = true;
            info_report("voodoo2: the guest writes the command FIFO out of "
                        "address order (%06x before %06x: a value before its "
                        "header); a word is counted for the consumer once "
                        "every word before it has arrived, as the chip's hole "
                        "counter does", (unsigned) addr,
                        (unsigned) (VOODOO2_FIFO_WIN + s->mmio_next));
        }
        return;
    }
    if (off != s->mmio_next) {
        /* not where the count expected the guest and no JMP said so: a
         * hole wider than the bitmap, or a ring taken up somewhere without
         * its registers written. The count starts again here. */
        voodoo2_mmio_flush(s, addr, "the command FIFO moved");
        s->mmio_next      = off;
        s->mmio_in_packet = 0;
    }
    /* this word, and every one after it that arrived early */
    for (;;) {
        if (voodoo2_mmio_step(s)) {
            /* a JMP: whatever was held past it lies beyond where the guest
             * goes on, which no stream of Glide's has produced */
            voodoo2_mmio_flush(s, addr, "the command FIFO jumped");
            break;
        }
        s->mmio_ahead >>= 1;
        if (!(s->mmio_ahead & 1)) {
            break;
        }
    }
}

/* the ring's registers written, or the ring mapped either way: the count
 * starts over where the read pointer says the guest does */
static void
voodoo2_mmio_ring_reset(Voodoo2State *s)
{
    voodoo_t *v  = s->v;
    uint32_t  rp = v->cmdfifo_rp;

    s->mmio_next = (rp >= v->cmdfifo_base &&
                    rp - v->cmdfifo_base < VOODOO2_FIFO_WIN_MAX)
                   ? rp - v->cmdfifo_base : VOODOO2_MMIO_UNKNOWN;
    s->mmio_ahead     = 0;
    s->mmio_in_packet = 0;
}

/* A card cannot be busy with nothing to do.
 *
 * SST_status's busy bit is four things or-ed together, and one of them is
 * `cmd_written + cmd_written_fifo - cmd_read`, a running difference that no
 * event ever resynchronises. On a Voodoo 2 it is not symmetric: a swap
 * arriving as a command-FIFO *packet* increments `cmd_read` and not
 * `cmd_written_fifo` (only Banshee and later count one), while the register
 * write Glide makes beside it increments `cmd_written`. The two are meant
 * to cancel, and when for any reason they do not, the difference stands for
 * ever. The card reads busy to every later poll, and Glide's grSstIdle
 * never returns. Measured on `ramfifo=off`: Carmageddon froze on
 * its first race frame with `wr 3243 rd 3242`, one command outstanding, the
 * ring caught up (`fifo depth 631660/631660`), nothing rendering, and the
 * guest reading register 0x000 26 million times in five seconds.
 *
 * So: when the guest is polling status and every *real* sign of work is
 * clear (both FIFOs empty, the ring caught up, no render thread, no swap
 * pending, the consumer not in its loop), the difference is stale and is
 * put back. The hysteresis is what makes it safe: the consumer is briefly
 * between dequeueing a command and counting it, and in that window the card
 * looks exactly like this, so it takes a long run of *consecutive* polls
 * with no guest write in between, which only a spinning guest can produce.
 */
#define VOODOO2_IDLE_MS 20

static void
voodoo2_status_unstick(Voodoo2State *s)
{
    voodoo_t *v = s->v;
    int       written = v->cmd_written + v->cmd_written_fifo + v->cmd_written_fifo_2;
    int64_t   now;

    /* Every real sign of work, and the count against them. The gate is the
     * idle state itself, held across a run of polls: counting polls since
     * the guest last *wrote* does not work, because `cmd_written_fifo` goes
     * up on every triangle packet and a game sends 300,000 of those in five
     * seconds (a run showed that rule never firing). */
    if (written == v->cmd_read ||
        ATOMIC_LOAD(v->fifo_read_idx) != ATOMIC_LOAD(v->fifo_write_idx) ||
        ATOMIC_LOAD(v->cmdfifo_depth_rd) != ATOMIC_LOAD(v->cmdfifo_depth_wr) ||
        v->cmdfifo_in_sub || v->voodoo_busy || v->swap_pending ||
        RENDER_VOODOO_BUSY(v, 0) ||
        (v->render_threads >= 2 && RENDER_VOODOO_BUSY(v, 1)) ||
        (v->render_threads == 4 && (RENDER_VOODOO_BUSY(v, 2) || RENDER_VOODOO_BUSY(v, 3)))) {
        s->idle_status_since = 0;
        return;
    }
    now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (!s->idle_status_since) {
        s->idle_status_since = now;
        return;
    }
    if (now - s->idle_status_since < VOODOO2_IDLE_MS) {
        return;         /* the consumer may be between a command and its count */
    }
    s->idle_status_since = 0;
    if (!s->idle_status_noted) {
        s->idle_status_noted = true;
        info_report("voodoo2: the card read busy on %d command(s) outstanding "
                    "(written %d, read %d) with both FIFOs empty and nothing "
                    "rendering, for %d ms of polling: the count is stale and "
                    "goes back, or a guest waiting for idle never gets out",
                    written - v->cmd_read, written, (int) v->cmd_read,
                    VOODOO2_IDLE_MS);
    }
    ATOMIC_STORE(v->cmd_read, written);
}

/* Is this write one that reconfigures the command FIFO? Only the register
 * window proper: with the FIFO on, the window at 0x200000 carries packets,
 * and with it off there is nothing to order. */
static bool
voodoo2_fifo_reconfigured_by(Voodoo2State *s, hwaddr addr, uint64_t val,
                             unsigned size)
{
    if (addr >= 0x200000 || size != 4 || !ATOMIC_LOAD(s->v->cmdfifo_enabled) ||
        !(s->v->initEnable & 1)) {
        return false;
    }
    switch (addr & 0x3fc) {
    case SST_fbiInit7:
        return !(val & 0x100);          /* the bit that turns the FIFO off */
    case SST_cmdFifoBaseAddr:
    case SST_cmdFifoRdPtr:
    case SST_cmdFifoDepth:
        return true;
    default:
        return false;
    }
}

/* After such a write: if the FIFO is off with words counted and not run,
 * nobody is ever going to run them, and the difference is what the status
 * register answers `busy` from. Close it, or the card is busy for ever. */
static void
voodoo2_fifo_settle(Voodoo2State *s, bool was_on)
{
    voodoo_t *v = s->v;
    uint32_t  rd, wr;

    if (!was_on || ATOMIC_LOAD(v->cmdfifo_enabled)) {
        return;
    }
    rd = ATOMIC_LOAD(v->cmdfifo_depth_rd);
    wr = ATOMIC_LOAD(v->cmdfifo_depth_wr);
    if (rd == wr) {
        return;
    }
    if (!s->fifo_left_noted) {
        s->fifo_left_noted = true;
        info_report("voodoo2: the command FIFO was turned off with %u word(s) "
                    "counted and not run; the card goes idle rather than "
                    "reading busy to every later poll", wr - rd);
    }
    ATOMIC_STORE(v->cmdfifo_depth_rd, wr);
    v->cmdfifo_in_sub = 0;
}

/* A read of the LFB with the command FIFO off: Glide's device probe, when
 * it is one of its three checks: fbiMemSize's 16-bit depth-buffer reads
 * away from the origin, and a 32-bit read of the 4x4 at the origin under a
 * textured fbzColorPath (the TMU configuration strap and the texture-memory
 * sense, which samples texels just written at 2, 1 and 0 MB). A wrong value
 * there makes Glide size the card smaller or refuse to open it; after a long
 * session that is where a stale device (a texture cache) would show. */
static void
voodoo2_probe_read(Voodoo2State *s, hwaddr addr, uint64_t val, unsigned size)
{
    voodoo_t *v = s->v;
    unsigned  x = (addr & 0x7fe) >> 1;
    unsigned  y = (addr >> 11) & 0x3ff;
    bool      mem  = size == 2 && (x || y);
    bool      tmu  = size == 4 && x < 4 && y < 4 && (s->probe_fbzcp & (1u << 27));

    if ((!mem && !tmu) || s->probe_lines >= 96) {
        return;
    }
    s->probe_lines++;
    if (mem) {
        info_report("voodoo2: probe: LFB %u,%u reads %04x (lfbMode %08x "
                    "fbiInit1 %08x fbiInit2 %08x, row %d, read at %06x, "
                    "aux at %06x)", x, y, (unsigned) val, v->lfbMode,
                    v->fbiInit1, v->fbiInit2, v->row_width,
                    v->fb_read_offset, v->params.aux_offset);
    } else {
        info_report("voodoo2: probe: textured LFB %u,%u reads %08x "
                    "(texBaseAddr %06x tLOD %08x textureMode %08x)", x, y,
                    (unsigned) val, v->params.texBaseAddr[0],
                    v->params.tLOD[0], v->params.textureMode[0]);
    }
}

static uint64_t
voodoo2_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;

    uint64_t      val;

    if (addr < 0x400000) {
        s->rd_hist[(addr >> 2) & 0xff]++;
    }
    if (s->fifo_mapped) {
        s->trig_addr  = (uint32_t) addr;
        s->trig_size  = size;
        s->trig_write = false;
        voodoo2_fifo_sync(s);
        if (size == 4 && addr < VOODOO2_FIFO_WIN &&
            (addr & 0x3fc) == SST_cmdFifoRdPtr && s->fifo_mapped) {
            val = voodoo2_fifo_rdptr(s);
            voodoo2_note(s, addr, val, size, false);
            return val;
        }
    }
    if (addr < 0x200000 && (addr & 0x3fc) == SST_status && size == 4) {
        voodoo2_status_unstick(s);
    }
    switch (size) {
    case 4:
        val = v->mapping.read_l((uint32_t) addr, v->mapping.priv);
        break;
    case 2:
        val = v->mapping.read_w((uint32_t) addr, v->mapping.priv);
        break;
    default:
        /* the card has no byte lane: 86Box registers no byte handler */
        val = 0xff;
        break;
    }
    voodoo2_note(s, addr, val, size, false);
    if (!v->cmdfifo_enabled && (addr & 0xc00000) == 0x400000) {
        voodoo2_probe_read(s, addr, val, size);
    }
    return val;
}

static void
voodoo2_mmio_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;
    bool          fifo_was_on;

    if (s->fifo_mapped) {
        /* what the guest put in the ring comes before this write */
        s->trig_addr  = (uint32_t) addr;
        s->trig_size  = size;
        s->trig_write = true;
        voodoo2_fifo_sync(s);
    }
    if (addr < 0x400000) {
        s->wr_hist[(addr >> 2) & 0xff]++;
    }
    voodoo2_note(s, addr, val, size, true);
    if (addr >= 0x400000 && addr < 0x800000) {
        /* the buffer the guest is aiming this LFB write at, and the row */
        uint32_t y = (addr >> 11) & 0x3ff;

        if (v->fb_write_offset == v->params.front_offset) {
            s->lfb_front++;
        } else if (v->fb_write_offset == v->back_offset) {
            s->lfb_back++;
        } else {
            s->lfb_else++;
        }
        s->lfb_y_lo = MIN(s->lfb_y_lo, y);
        s->lfb_y_hi = MAX(s->lfb_y_hi, y);
    }
    if ((addr & 0x200000) && addr < 0x400000 && !v->cmdfifo_enabled &&
        (addr & 0x1fffff) >= 0x100) {
        /* A command-FIFO packet written to the 0x200000 window while the
         * FIFO is off. With the FIFO off that window is the legacy register
         * map, so 86Box decodes each packet dword as the register at bits
         * 9:2 as the chip would, garbage into videoDimensions, triangleCMD,
         * fbiInit7 (a dword with bit 8 set spuriously turns the FIFO back
         * on). What puts a Glide in that state, measured (doc 21 §11): a
         * *second* Glide client ran sst1InitRegisters under a live window
         * (3dfx's login helper, `rundll32
         * 3dfxv2ps.dll,UpdateRegSettings` through GLIDE3X.DLL), which
         * switched the FIFO off behind the running program's back. It is
         * also what a game's own close can leave (FIFA 2000 on
         * `base98-br`: the window closed, another init ran, and the client
         * that still owned the ring went on streaming to it).
         *
         * The stream is **refused** by default (`fifo-off-regs=on` is the
         * A/B, the walk as 86Box decodes it). Nothing writes this window on
         * purpose with the FIFO off (Glide only writes there when it
         * believes the FIFO is on), so every one of these dwords is a
         * stranded client's packet, and letting them walk the register file
         * destroys the card for everything after: `videoDimensions` is
         * zeroed and never rewritten, so the display timer stops generating
         * retraces and `status` never reads idle again; `fbiInit7` flips the
         * FIFO on and off at random; `intrCtrl` reaches 86Box's `fatal()`.
         * On the real chip a game recovers from this (3dfx's own Glide
         * does it routinely), so permanent damage is the emulation's, not
         * the card's. Offsets below 0x100 still pass: those are the vertex
         * and triangle registers under Glide's alternate mapping, and a
         * stray triangle renders and is over, where the rest sticks. */
        s->fifo_off_writes++;
        s->fifo_off_seen[(addr & 0x3fc) >> 7] |= 1u << (((addr & 0x3fc) >> 2) & 31);
        if (!s->fifo_off_warned) {
            char line[512];

            s->fifo_off_warned = true;
            warn_report("voodoo2: command-FIFO packet %08x to the window at "
                        "%06x with the FIFO off -> %s register %03x "
                        "(Glide streaming to a reset FIFO: the teardown hang)",
                        (unsigned) val, (unsigned) addr,
                        s->fifo_off_regs ? "decoded as" : "refused; it would be",
                        (unsigned) (addr & 0x3fc));
            /* who is streaming, and who switched the FIFO off under them:
             * the same cr3 in both is one program re-initialising under
             * itself, a different one is a second Glide client */
            voodoo2_where(line, sizeof(line));
            if (line[0]) {
                info_report("voodoo2: the packet comes from %s", line);
            }
            if (s->init_who[0]) {
                info_report("voodoo2: the last sst1InitRegisters was %s",
                            s->init_who);
            }
        }
        if (!s->fifo_off_regs) {
            return;
        }
    }
    if (addr < 0x400000 && (addr & 0x3fc) == 0x214 &&
        !((addr & 0x200000) && v->cmdfifo_enabled)) {
        /* fbiInit1 bit 23, scanline interleaving: this device is one card
         * with no partner, so the bit is not writable. 86Box's display
         * timer takes SLI at its word and draws the odd lines from a second
         * card that does not exist (a NULL: 3dfx's Glide on a
         * grSstWinClose/grSstWinOpen pushes its reopen's register writes
         * through the command-FIFO transport with the FIFO off, and the
         * stream walks the register file, fbiInit1 included). A real single
         * board with the bit set shows half its lines; this one ignores it. */
        val &= ~(1u << 23);
    }
    if (addr < 0x200000 && (addr & 0x3fc) == 0x104 && size == 4) {
        s->probe_fbzcp = (uint32_t) val;     /* fbzColorPath, for the probe */
    }
    if (size != 4 && (addr & 0x200000) && addr < 0x400000 && v->cmdfifo_enabled) {
        /* A packet word written narrower than a dword. 86Box's writew takes
         * only the frame buffer and it has no byte handler at all, so the
         * word is dropped and the consumer waits inside the packet for ever
         * (the deadlock above). Name it: the guest is not at fault here. */
        s->fifo_narrow++;
        if (!s->fifo_narrow_warned) {
            s->fifo_narrow_warned = true;
            warn_report("voodoo2: a %u-byte write of %08x into the command-FIFO "
                        "window at %06x: 86Box takes dwords there, so the word "
                        "is lost and the FIFO will stall", size,
                        (unsigned) val, (unsigned) addr);
        }
    }
    if ((addr & 0x200000) && addr < 0x400000 && !s->fifo_mapped &&
        ATOMIC_LOAD(v->cmdfifo_enabled)) {
        /* ramfifo=off: this dword is a ring word, and 86Box counts it into
         * cmdfifo_depth_wr itself, so here is that transport's publish
         * point ("one order" below). Patch 72 keeps the order on both. */
        voodoo2_mmio_drain(s);
    }
    /* the ring is what the guest wrote first: run it before this ("one
     * order" above) */
    if (voodoo2_fifo_reconfigured_by(s, addr, val, size)) {
        voodoo2_fifo_drain(s, "a command-FIFO register");
    } else if (s->lfb_order && addr >= 0x400000) {
        voodoo2_fifo_drain(s, "an LFB or texture write");
    }
    fifo_was_on = ATOMIC_LOAD(v->cmdfifo_enabled) != 0;
    switch (size) {
    case 4:
        if ((addr & 0x200000) && addr < 0x400000 && !s->fifo_mapped &&
            fifo_was_on && s->count_holes) {
            /* a ring word through MMIO: counted as the chip counts it */
            voodoo2_mmio_ring_write(s, (uint32_t) addr, (uint32_t) val);
        } else {
            v->mapping.write_l((uint32_t) addr, (uint32_t) val, v->mapping.priv);
        }
        break;
    case 2:
        v->mapping.write_w((uint32_t) addr, (uint16_t) val, v->mapping.priv);
        break;
    default:
        break;
    }
    voodoo2_fifo_settle(s, fifo_was_on);
    voodoo2_fifo_after_write(s, addr);
    if (addr < 0x200000 && size == 4) {
        switch (addr & 0x3fc) {
        case SST_cmdFifoBaseAddr:
        case SST_cmdFifoRdPtr:
        case SST_cmdFifoDepth:
        case SST_fbiInit7:
            voodoo2_mmio_ring_reset(s);
            break;
        default:
            break;
        }
    }
}

static const MemoryRegionOps voodoo2_mmio_ops = {
    .read       = voodoo2_mmio_read,
    .write      = voodoo2_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* a vector store to the LFB is split into dwords; a byte is answered
     * the way the hardware answers it (nothing) */
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl  = { .min_access_size = 1, .max_access_size = 4 },
};

/* ---------------------------------------------------- configuration space
 *
 * The PCI core owns the header (vendor, class, the BAR, the command
 * register). 86Box's handlers own 0x40-0x43, initEnable, whose byte 1 also
 * carries the "I am a Voodoo 2" strap the 3dfx driver reads; the command
 * register and the BAR's top byte are echoed so the card's own notion of
 * being enabled and mapped stays in step.
 */
/* 0x54, siProcess: the Voodoo 2's silicon-process monitor. Glide's
 * sst1InitMeasureSiProcess (cvg/init/util.c) loads a PCI-clock countdown
 * into bits 27:16, sets RUN (bit 28), polls the register until that field
 * reads zero and then takes the ring-oscillator count out of bits 15:0.
 * QEMU's configuration space keeps whatever a guest writes past the 64-byte
 * header, so the loaded count read back for ever and 3dfx's glide2x.dll spun
 * there on every grSstWinOpen (GLIDETEST and Diablo II's video
 * test "froze"). 86Box answers 0 to the whole register, which ends the loop
 * with a count of 0 ("a very slow process": the shorter clock delay); here
 * the countdown is over the moment RUN is read back, and the count is a
 * production board's, well past Glide's 5000 threshold. 0x40-0x43 is
 * initEnable, 86Box's. */
#define VOODOO2_CFG_SIPROCESS  0x54
#define SIPROCESS_OSC_CNTR     0x0000ffffu
#define SIPROCESS_PCI_CNTR     0x0fff0000u
#define SIPROCESS_RUN          (1u << 28)
#define SIPROCESS_OSC_COUNT    8000u

static uint32_t
voodoo2_siprocess(PCIDevice *dev)
{
    uint32_t si = pci_get_long(dev->config + VOODOO2_CFG_SIPROCESS);

    if (si & SIPROCESS_RUN) {
        return (si & ~(SIPROCESS_PCI_CNTR | SIPROCESS_OSC_CNTR)) | SIPROCESS_OSC_COUNT;
    }
    return si & ~SIPROCESS_OSC_CNTR;   /* held in reset: nothing counted */
}

static uint32_t
voodoo2_config_read(PCIDevice *dev, uint32_t addr, int len)
{
    Voodoo2State *s   = VOODOO2(dev);
    uint32_t      val = pci_default_read_config(dev, addr, len);

    s->cfg_hist[(addr >> 2) & 63]++;
    for (int i = 0; i < len; i++) {
        uint32_t a = addr + i;
        int      b = -1;

        if (a >= 0x40 && a <= 0x43) {
            b = voodoo_pci_read(0, (int) a, 1, s->v);
        } else if (a >= VOODOO2_CFG_SIPROCESS && a < VOODOO2_CFG_SIPROCESS + 4) {
            b = (voodoo2_siprocess(dev) >> (8 * (a - VOODOO2_CFG_SIPROCESS))) & 0xff;
        }
        if (b >= 0) {
            val &= ~(0xffu << (8 * i));
            val |= (uint32_t) b << (8 * i);
        }
    }
    return val;
}

static void
voodoo2_config_write(PCIDevice *dev, uint32_t addr, uint32_t val, int len)
{
    Voodoo2State *s = VOODOO2(dev);

    pci_default_write_config(dev, addr, val, len);
    for (int i = 0; i < len; i++) {
        uint32_t a = addr + i;

        if (a == PCI_COMMAND || a == 0x13 || (a >= 0x40 && a <= 0x43)) {
            voodoo_pci_write(0, (int) a, 1, (val >> (8 * i)) & 0xff, s->v);
        }
    }
    if (addr <= 0x43 && addr + len > 0x40) {
        info_report("voodoo2: initEnable <= %08x", s->v->initEnable);
        voodoo2_trace_where_armed = voodoo2_trace;
        if (s->v->initEnable == 0x00000001) {
            /* sst1InitRegisters opens with exactly SST_INITWR_EN: remember
             * whose it is, for the FIFO-off report above */
            voodoo2_where(s->init_who, sizeof(s->init_who));
        }
        if (s->v->initEnable == 0x00000001 && s->v->cmdfifo_enabled) {
            /* A second Glide initialising the card under a live one (doc 21
             * §11). sst1InitRegisters opens with initEnable = SST_INITWR_EN,
             * exactly 1, and goes on to zero the video timing and put
             * fbiInit7 back to its default, the command FIFO off. Glide's
             * own close turns the FIFO off before any of that, so with the
             * FIFO still on this is someone else's init. The Glide
             * that owns the window goes on streaming packets into a FIFO
             * that is now off, which 86Box decodes as registers: the card
             * wedges. 3dfx's driver runs one at every login (the Run entry
             * `Voodoo2`, rundll32 3dfxv2ps.dll,UpdateRegSettings, through
             * GLIDE3X.DLL), a few seconds' work under TCG and milliseconds
             * on the chip, so a Glide program started at the desktop lands
             * in it. Named here because on its own it reads as the device
             * hanging; VOODOO2_TRACE=1 names the writing module. */
            warn_report("voodoo2: the card is being re-initialised "
                        "(sst1InitRegisters) while a Glide window has the "
                        "command FIFO on -- another Glide client is "
                        "initialising it under this one (3dfx's login helper "
                        "does, for a few seconds after the desktop appears); "
                        "the running program will wedge");
        }
    }
}

/* --------------------------------------------------------------- display */

static QemuConsole *
voodoo2_console(Voodoo2State *s)
{
    if (!s->con) {
        s->con = qemu_console_lookup_by_index(0);
    }
    if (!s->con && !s->no_console_warned) {
        s->no_console_warned = true;
        warn_report("voodoo2: no VGA console to pass through (-vga none "
                    "and no display adapter?): frames go nowhere");
    }
    return s->con;
}

/* fbiInit0 VGA_PASS: vCPU thread, BQL held */
static void
voodoo2_set_override(void *opaque, int on)
{
    Voodoo2State *s   = opaque;
    QemuConsole  *con = voodoo2_console(s);

    on = (on != 0);
    if (on == s->override) {
        return;
    }
    s->override = on;
    s->shown_w  = 0;
    s->shown_h  = 0;
    if (!con) {
        return;
    }
    graphic_hw_passthrough(con, on);
    if (!on) {
        /* the VGA draws again: make it start from a full frame, into a
         * surface of its own (ours is freed when it replaces it) */
        s->surface = NULL;
        qemu_console_hw_invalidate(con);
        qemu_console_hw_update(con);
    }
    info_report("voodoo2: display %s", on ? "on (VGA pass-through)" : "off (VGA back)");
}

/* end of a frame with dirty lines: main loop (display timer), BQL held */
static void
voodoo2_present(void *opaque, const bitmap_t *frame, int w, int h)
{
    Voodoo2State   *s   = opaque;
    QemuConsole    *con = voodoo2_console(s);
    DisplaySurface *cur;
    uint8_t        *dst;
    int             stride;

    if (!con || !s->override) {
        return;
    }
    if (w <= 0 || h <= 0 || w > frame->w || h > frame->h) {
        return;
    }
    if (w != s->shown_w || h != s->shown_h) {
        /* the card just took the monitor, or its mode changed */
        s->shown_w       = w;
        s->shown_h       = h;
        s->blank         = true;
        s->blank_swapped = false;
        s->blank_front   = s->v->front_offset;
        s->blank_frames  = s->v->frame_count;
        s->blank_since   = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    }
    if (s->blank) {
        int64_t ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - s->blank_since;

        if (s->blank_swapped || ms >= VOODOO2_BLANK_MS) {
            /* a swap marks every line dirty, so the frame after the one it
             * landed in is all the new buffer's */
            s->blank = false;
            info_report("voodoo2: %dx%d shown after %" PRId64 " ms of black (%s)",
                        w, h, ms, s->blank_swapped ? "first swap" : "no swap");
        } else if (s->v->front_offset != s->blank_front ||
                   s->v->frame_count != s->blank_frames) {
            s->blank_swapped = true;
        }
        if (s->blank) {
            /* redraw every line next frame: a frame is only presented when
             * lines are dirty, and a guest that swaps once and then draws
             * nothing would otherwise leave the console black for good */
            memset(s->v->dirty_line, 1, sizeof(s->v->dirty_line));
        }
    }
    cur = qemu_console_surface(con);
    if (cur != s->surface || !cur ||
        surface_width(cur) != w || surface_height(cur) != h) {
        s->surface = qemu_create_displaysurface(w, h);
        qemu_console_set_surface(con, s->surface);
    }
    dst    = surface_data(s->surface);
    stride = surface_stride(s->surface);
    /* The undither reads the front buffer itself. It needs the stored 565
     * codes, which is where the dither is; frame->line[] is what 86Box made
     * of them. It declines a frame it cannot answer for (doc 21 §12). */
    if (s->undither && !s->blank) {
        const char *why = NULL;

        if (voodoo_undither_frame(s->v, dst, stride, w, h, &why)) {
            if (!s->undither_on_noted) {
                s->undither_on_noted = true;
                info_report("voodoo2: undither on (%s dither)",
                            (s->v->params.fbzMode & FBZ_DITHER_2x2_BIT) ? "2x2" : "4x4");
            }
            goto done;
        }
        if (why && why != s->undither_why_noted) {
            s->undither_why_noted = why;
            info_report("voodoo2: undither off this frame: %s", why);
        }
    }
    for (int y = 0; y < h; y++) {
        if (s->blank) {
            memset(dst + (size_t) y * stride, 0, (size_t) w * 4);
        } else {
            memcpy(dst + (size_t) y * stride, frame->line[y], (size_t) w * 4);
        }
    }
done:
    qemu_console_update_full(con);
    s->frames++;
    if (s->v->front_offset != s->shown_front) {
        s->shown_front = s->v->front_offset;
        s->shown++;
    }
}

/* ----------------------------------------------------------------- stats */

/* the four most-hit registers of a histogram, as " 0x000:N ..." */
static void
voodoo2_top_regs(uint32_t *hist, int count, char *buf, size_t len)
{
    size_t n = 0;

    buf[0] = 0;
    for (int k = 0; k < 4; k++) {
        int best = -1;

        for (int i = 0; i < count; i++) {
            if (hist[i] && (best < 0 || hist[i] > hist[best])) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        n += snprintf(buf + n, len - n, " 0x%03x:%u", best << 2, hist[best]);
        hist[best] = 0;
        if (n >= len) {
            break;
        }
    }
    memset(hist, 0, count * sizeof(*hist));
}

static void
voodoo2_stats(void *opaque)
{
    Voodoo2State *s = opaque;
    voodoo_t     *v = s->v;
    uint32_t      frames = s->frames - s->last_frames;
    int           tris   = v->tri_count - s->last_tris;
    int           wr     = v->wr_count - s->last_wr;
    int           rd     = v->rd_count - s->last_rd;
    int           tex    = v->tex_count - s->last_tex;
    uint32_t      syncs  = s->fifo_syncs - s->last_fifo_syncs;
    uint32_t      fwords = s->fifo_words - s->last_fifo_words;

    /* a guest waiting on the FIFO in RAM reads only cmdFifoRdPtr, which is
     * answered here and never reaches 86Box's read count: its syncs are the
     * activity then (a whole hang went unreported without them) */
    if (frames || tris || wr || rd || voodoo_shim_fatals != s->last_fatals ||
        s->fifo_off_writes != s->last_fifo_off ||
        s->fifo_syncs != s->last_fifo_syncs) {
        char rds[64], wrs[64], cfg[64], ref[48] = "", busy[96] = "", ram[256] = "";
        char ord[96] = "", holes[128] = "", lfb[128] = "";
        int  written = v->cmd_written + v->cmd_written_fifo + v->cmd_written_fifo_2;
        int  outstanding = written - v->cmd_read;
        int  is_busy = outstanding ||
            (v->cmdfifo_depth_rd != v->cmdfifo_depth_wr) || v->cmdfifo_in_sub ||
            v->voodoo_busy ||
            RENDER_VOODOO_BUSY(v, 0) ||
            (v->render_threads >= 2 && RENDER_VOODOO_BUSY(v, 1)) ||
            (v->render_threads == 4 && (RENDER_VOODOO_BUSY(v, 2) || RENDER_VOODOO_BUSY(v, 3)));

        /* the guest is polling status and the card is busy: name what the
         * status register's busy bit is reading, so a spin says why */
        if (is_busy && rd > 100000 && s->rd_hist[0] * 4 > (uint32_t) rd) {
            snprintf(busy, sizeof(busy),
                     "; busy: %d cmds outstanding (wr %d rd %d), fifo depth %u/%u%s%s%s",
                     outstanding, written, v->cmd_read,
                     (unsigned) v->cmdfifo_depth_rd, (unsigned) v->cmdfifo_depth_wr,
                     v->voodoo_busy ? ", voodoo_busy" : "",
                     RENDER_VOODOO_BUSY(v, 0) ? ", render0" : "",
                     (v->render_threads >= 2 && RENDER_VOODOO_BUSY(v, 1)) ? ", render1" : "");
        }

        voodoo2_top_regs(s->rd_hist, 256, rds, sizeof(rds));
        voodoo2_top_regs(s->wr_hist, 256, wrs, sizeof(wrs));
        voodoo2_top_regs(s->cfg_hist, 64, cfg, sizeof(cfg));
        if (voodoo_shim_fatals != s->last_fatals || s->fifo_off_writes != s->last_fifo_off) {
            char regs[128] = "";
            size_t rn = 0;

            for (unsigned i = 0; i < 256 && rn < sizeof(regs) - 8; i++) {
                if (s->fifo_off_seen[i >> 5] & (1u << (i & 31))) {
                    rn += snprintf(regs + rn, sizeof(regs) - rn, "%s%03x",
                                   rn ? " " : " (as registers ", i * 4);
                }
            }
            if (rn) {
                snprintf(regs + rn, sizeof(regs) - rn, ")");
            }
            memset(s->fifo_off_seen, 0, sizeof(s->fifo_off_seen));
            snprintf(ref, sizeof(ref), "; %u refused, %u fifo-off%s",
                     voodoo_shim_fatals - s->last_fatals,
                     s->fifo_off_writes - s->last_fifo_off, regs);
        }
        s->last_fifo_off = s->fifo_off_writes;
        if (s->fifo_mapped && s->fifo_words == s->last_fifo_words &&
            s->fifo_syncs != s->last_fifo_syncs) {
            /* polled and nothing counted: which side is waiting */
            uint32_t rp = qatomic_read(&v->cmdfifo_rp);

            snprintf(ram, sizeof(ram), "; FIFO in RAM: 0 words in %u syncs, "
                     "ring %08x+%x rp %08x (%08x) parse %08x (%08x) "
                     "poisoned %08x depth %u/%u%s%s",
                     s->fifo_syncs - s->last_fifo_syncs,
                     s->fifo_base, s->fifo_size,
                     rp, *voodoo2_fifo_word(v, rp),
                     s->fifo_parse, *voodoo2_fifo_word(v, s->fifo_parse),
                     s->fifo_poisoned,
                     (unsigned) v->cmdfifo_depth_rd, (unsigned) v->cmdfifo_depth_wr,
                     v->cmdfifo_in_sub ? ", in sub" : "",
                     v->swap_pending ? ", swap pending" : "");
        } else if (s->fifo_mapped || s->fifo_words != s->last_fifo_words) {
            snprintf(ram, sizeof(ram), "; FIFO in RAM: %u words in %u syncs, "
                     "%u packets met part-written",
                     s->fifo_words - s->last_fifo_words,
                     s->fifo_syncs - s->last_fifo_syncs, s->partial);
        }
        /* the ordering waits ("one order"): what they cost, on whichever
         * transport the ring is on */
        if (s->drains != s->last_drains || s->behind != s->last_behind) {
            snprintf(ord, sizeof(ord), "; %u waits for the ring, %u publishes "
                     "behind the LFB queue", s->drains - s->last_drains,
                     s->behind - s->last_behind);
        }
        /* the MMIO ring's holes: words the guest wrote ahead of one it had
         * not written yet, and those counted with such a hole still open */
        if (s->mmio_holes != s->last_mmio_holes ||
            s->mmio_flushed != s->last_mmio_flushed ||
            s->mmio_jumps != s->last_mmio_jumps) {
            snprintf(holes, sizeof(holes), "; %u ring words written ahead of a "
                     "hole, %u counted with one open, %u jumps followed",
                     s->mmio_holes - s->last_mmio_holes,
                     s->mmio_flushed - s->last_mmio_flushed,
                     s->mmio_jumps - s->last_mmio_jumps);
        }
        s->last_mmio_holes   = s->mmio_holes;
        s->last_mmio_flushed = s->mmio_flushed;
        s->last_mmio_jumps   = s->mmio_jumps;
        if (s->lfb_front != s->last_lfb_front || s->lfb_back != s->last_lfb_back ||
            s->lfb_else != s->last_lfb_else) {
            snprintf(lfb, sizeof(lfb), "; LFB writes: %u to the front buffer, "
                     "%u to the back, %u elsewhere, rows %u..%u",
                     s->lfb_front - s->last_lfb_front,
                     s->lfb_back - s->last_lfb_back,
                     s->lfb_else - s->last_lfb_else,
                     s->lfb_y_lo > s->lfb_y_hi ? 0 : s->lfb_y_lo, s->lfb_y_hi);
        }
        s->last_lfb_front = s->lfb_front;
        s->last_lfb_back  = s->lfb_back;
        s->last_lfb_else  = s->lfb_else;
        s->lfb_y_lo = 0xffffffff;
        s->lfb_y_hi = 0;
        s->last_drains = s->drains;
        s->last_behind = s->behind;
        s->last_fifo_words = s->fifo_words;
        s->last_fifo_syncs = s->fifo_syncs;
        /* frames = presents (a scan-out with any dirty line); new = those
         * showing a buffer the last one did not, i.e. the game's frame rate */
        info_report("voodoo2: %dx%d %s: %u frames (%u new), %d triangles, "
                    "%d writes (%d texture), %d reads in %.1f s; regs read%s; "
                    "written%s; config read%s%s%s%s%s%s%s",
                    v->h_disp, v->v_disp, s->override ? "on" : "off",
                    frames, s->shown - s->last_shown, tris, wr, tex, rd,
                    VOODOO2_STATS_MS / 1000.0,
                    rds[0] ? rds : " none", wrs[0] ? wrs : " none",
                    cfg[0] ? cfg : " none", ref, busy, ram, ord, holes, lfb);
    }
    /* The FIFO deadlock: 86Box's consumer waits inside cmdfifo_get
     * for a word the guest never wrote (it read a packet header wanting more
     * words than Glide put there), so `voodoo_busy` stays set with the ring
     * fully consumed, and Glide (which polls the status register for idle
     * before it writes anything else) never writes again. Neither side can
     * move. Both are visible from here: the card busy, the ring empty, no
     * work done, and the guest reading one register a million times. Name
     * the packet: the words around the read pointer, and what the guest did
     * last. */
    if (v->cmdfifo_enabled && !v->cmdfifo_in_sub &&
        v->cmdfifo_depth_rd == v->cmdfifo_depth_wr &&
        !frames && !tris && wr < 16 &&
        /* the consumer inside a packet (`voodoo_busy`), or the ring in RAM
         * with nothing left to count and the guest still asking */
        (v->voodoo_busy || (s->fifo_mapped && !fwords)) &&
        /* the guest polling hard: the status register through 86Box, or
         * cmdFifoRdPtr, which is answered here and reaches no read count */
        (rd > 100000 || syncs > 100000)) {
        s->stall_windows++;
    } else {
        s->stall_windows = 0;
    }
    if (s->stall_windows >= 2 && !s->stall_dumped) {
        uint32_t rp = qatomic_read(&v->cmdfifo_rp);

        s->stall_dumped = true;
        voodoo2_trace_flush();
        fprintf(stderr, "voodoo2: the command FIFO is stuck inside a packet: "
                "the consumer wants the word at %08x, which the guest has not "
                "written, and the guest is waiting for the card to go idle "
                "(ring %08x+%x, depth %u, %s, %u narrow writes, %u packets "
                "met while the guest was still writing them)\n", rp,
                v->cmdfifo_base, v->cmdfifo_end + 0x1000 - v->cmdfifo_base,
                (unsigned) v->cmdfifo_depth_wr,
                s->fifo_mapped ? "in RAM" : "through MMIO", s->fifo_narrow,
                s->partial);
        if (s->fifo_mapped) {
            /* How this walk read the tail of the stream: a packet counted
             * longer than the guest wrote lands the next header in unwritten
             * space, and a shorter one lands it inside somebody's parameters.
             * Either way the last few here say which packet did it. */
            unsigned n = MIN(s->seen_n, ARRAY_SIZE(s->seen));

            fprintf(stderr, "voodoo2: the last %u packets counted, oldest "
                    "first (parse stopped at %08x):\n", n, s->fifo_parse);
            for (unsigned k = 0; k < n; k++) {
                unsigned i = (s->seen_n - n + k) % ARRAY_SIZE(s->seen);

                fprintf(stderr, "voodoo2:   %08x header %08x type %u, "
                        "%u words\n", s->seen[i].addr, s->seen[i].hdr,
                        s->seen[i].hdr & 7, s->seen[i].n);
            }
        }
        for (uint32_t a = rp - 0x40; a != rp + 0x20; a += 4) {
            fprintf(stderr, "voodoo2:   %08x %08x%s\n", a,
                    *voodoo2_fifo_word(v, a), a == rp ? "   <- wanted" : "");
        }
        voodoo2_dump_accesses(s);
    }
    s->probe_lines = 0;
    s->last_fatals = voodoo_shim_fatals;
    s->last_frames = s->frames;
    s->last_shown  = s->shown;
    s->last_tris   = v->tri_count;
    s->last_wr     = v->wr_count;
    s->last_rd     = v->rd_count;
    s->last_tex    = v->tex_count;
    timer_mod(s->stats, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + VOODOO2_STATS_MS);
}

/* ---------------------------------------------------------------- device */

static bool voodoo2_instantiated;

static void
voodoo2_realize(PCIDevice *dev, Error **errp)
{
    Voodoo2State   *s = VOODOO2(dev);
    VoodooShimHooks hooks = {
        .opaque       = s,
        .set_override = voodoo2_set_override,
        .on_fatal     = voodoo2_on_fatal,
        .present      = voodoo2_present,
    };

    if (voodoo2_instantiated) {
        /* 86Box's primary-SVGA hook and the shim's monitor are one per
         * process; SLI is not modelled (fbiInit7 has no second card) */
        error_setg(errp, "voodoo2: only one Voodoo 2 per machine");
        return;
    }
    if (s->fbmem_mb != 2 && s->fbmem_mb != 4) {
        error_setg(errp, "voodoo2: fbmem must be 2 or 4 (MB)");
        return;
    }
    if (s->texmem_mb != 2 && s->texmem_mb != 4) {
        error_setg(errp, "voodoo2: texmem must be 2 or 4 (MB, per TMU)");
        return;
    }
    if (s->threads != 1 && s->threads != 2 && s->threads != 4) {
        error_setg(errp, "voodoo2: threads must be 1, 2 or 4");
        return;
    }
    voodoo2_instantiated = true;

    voodoo2_trace = getenv("VOODOO2_TRACE") && *getenv("VOODOO2_TRACE") == '1';
    voodoo_shim_init(&hooks);
    voodoo_shim_set_config("bilinear", s->bilinear);
    voodoo_shim_set_config("dithersub", s->dithersub);
    voodoo_shim_set_config("dacfilter", s->filter);
    voodoo_shim_set_config("texture_memory", (int) s->texmem_mb);
    voodoo_shim_set_config("framebuffer_memory", (int) s->fbmem_mb);
    voodoo_shim_set_config("render_threads", (int) s->threads);
    voodoo_shim_set_config("recompiler", s->recompiler);
    voodoo_shim_set_config("sli", 0);

    s->set = voodoo_init(NULL);
    s->v   = s->set->voodoos[0];
    /* the per-scanline display timer, coalesced (shim/86box/timer.h) */
    s->v->timer.slack_ns = 1000000;

    memory_region_init_io(&s->mmio, OBJECT(s), &voodoo2_mmio_ops, s,
                          "voodoo2.mmio", VOODOO2_BAR_SIZE);
    if (s->ramfifo) {
        /* the frame buffer page-aligned, so a slice of it can be RAM to the
         * guest (86Box's own is a calloc, 16 bytes past a page, which a KVM
         * memory slot refuses); nothing has used 86Box's yet, and it goes
         * back before voodoo_close() frees it */
        uint8_t *fb = qemu_memalign(qemu_real_host_page_size(), 4 * MiB);

        memset(fb, 0, 4 * MiB);
        s->fb_86box  = s->v->fb_mem;
        s->v->fb_mem = fb;
        memory_region_init_ram_ptr(&s->fb_ram, OBJECT(s), "voodoo2.fb",
                                   4 * MiB, fb);
        memory_region_init_alias(&s->fifo_win, OBJECT(s), "voodoo2.cmdfifo",
                                 &s->fb_ram, 0, 0x1000);
        memory_region_set_enabled(&s->fifo_win, false);
        memory_region_add_subregion_overlap(&s->mmio, VOODOO2_FIFO_WIN,
                                            &s->fifo_win, 1);
    }
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);
    /* initEnable: writable (the read is answered by 86Box's handler) */
    for (int a = 0x40; a <= 0x43; a++) {
        dev->wmask[a] = 0xff;
    }
    /* a reference board: no subsystem IDs (QEMU's default is Red Hat's) */
    pci_set_word(dev->config + PCI_SUBSYSTEM_VENDOR_ID, 0);
    pci_set_word(dev->config + PCI_SUBSYSTEM_ID, 0);

    s->lfb_y_lo = 0xffffffff;
    voodoo2_mmio_ring_reset(s);
    s->stats = timer_new_ms(QEMU_CLOCK_VIRTUAL, voodoo2_stats, s);
    timer_mod(s->stats, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + VOODOO2_STATS_MS);

    info_report("voodoo2: %u MB frame buffer, %u MB per TMU, %u render "
                "thread%s, recompiler %s",
                s->fbmem_mb, s->texmem_mb, s->threads, s->threads > 1 ? "s" : "",
                s->recompiler ? "on" : "off");
}

static void
voodoo2_exit(PCIDevice *dev)
{
    Voodoo2State *s = VOODOO2(dev);

    if (s->stats) {
        timer_free(s->stats);
        s->stats = NULL;
    }
    if (s->set) {
        voodoo2_set_override(s, 0);
        if (s->fb_86box) {
            qemu_vfree(s->v->fb_mem);
            s->v->fb_mem = s->fb_86box;
            s->fb_86box  = NULL;
        }
        voodoo_close(s->set);
        s->set = NULL;
        s->v   = NULL;
    }
    voodoo2_instantiated = false;
}

/* A system reset: 86Box has no reset entry for the card (the driver
 * re-initialises it), but the BAR is gone and the monitor must go back to
 * the VGA, or a rebooting guest stares at the last frame. */
static void
voodoo2_reset(DeviceState *dev)
{
    Voodoo2State *s = VOODOO2(dev);
    voodoo_t     *v = s->v;

    if (!v) {
        return;
    }
    thread_wait_mutex(v->force_blit_mutex);
    v->can_blit         = 0;
    v->force_blit_count = 0;
    thread_release_mutex(v->force_blit_mutex);
    v->fbiInit0        = 0;
    v->fbiInit7        = 0;
    v->cmdfifo_enabled = 0;
    v->initEnable      = 0;
    v->pci_enable      = 0;
    v->memBaseAddr     = 0;
    voodoo2_set_override(s, 0);
    s->fifo_broken = false;
    voodoo2_fifo_map(s);
}

static const Property voodoo2_properties[] = {
    /* the 8 MB board: 4 MB of frame buffer and 2 MB per TMU. It was the
     * common Voodoo 2, and the 12 MB board's 4 MB TMUs break a game written
     * before it existed: a texture level may not span a 2 MB boundary of
     * TMU memory, Glide refuses one that does, and a 1997 allocator that
     * walks the whole of a 4 MB range reaches that line sooner or later
     * (Carmageddon's 3dfx build, every race, 20 to 50 s in, dying inside
     * its own error print, doc 21 §13). texmem=4 is the 12 MB
     * board, for a title that wants it. */
    DEFINE_PROP_UINT32("fbmem", Voodoo2State, fbmem_mb, 4),
    DEFINE_PROP_UINT32("texmem", Voodoo2State, texmem_mb, 2),
    DEFINE_PROP_UINT32("threads", Voodoo2State, threads, 2),
    DEFINE_PROP_BOOL("bilinear", Voodoo2State, bilinear, true),
    DEFINE_PROP_BOOL("dither-sub", Voodoo2State, dithersub, true),
    DEFINE_PROP_BOOL("filter", Voodoo2State, filter, false),
    DEFINE_PROP_BOOL("undither", Voodoo2State, undither, false),
    DEFINE_PROP_BOOL("recompiler", Voodoo2State, recompiler, true),
    DEFINE_PROP_BOOL("ramfifo", Voodoo2State, ramfifo, true),
    DEFINE_PROP_BOOL("lfb-order", Voodoo2State, lfb_order, false),
    /* on: a command-FIFO-window write met with the FIFO off walks the
     * register file, the way 86Box decodes it. Off (the default) refuses
     * it (see the site in voodoo2_mmio_write) */
    DEFINE_PROP_BOOL("fifo-off-regs", Voodoo2State, fifo_off_regs, false),
    /* off: 86Box's own count, a word per write whatever its address (the
     * A/B for Carmageddon's race-start freeze) */
    DEFINE_PROP_BOOL("mmio-holes", Voodoo2State, count_holes, true),
};

static void
voodoo2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k  = PCI_DEVICE_CLASS(klass);

    k->realize      = voodoo2_realize;
    k->exit         = voodoo2_exit;
    k->config_read  = voodoo2_config_read;
    k->config_write = voodoo2_config_write;
    k->vendor_id    = VOODOO2_PCI_VENDOR;
    k->device_id    = VOODOO2_PCI_DEVICE;
    k->revision     = 0x02;
    k->class_id     = PCI_CLASS_MULTIMEDIA_VIDEO;
    dc->desc        = "3dfx Voodoo 2 (86Box's emulation)";
    dc->hotpluggable = false;
    device_class_set_legacy_reset(dc, voodoo2_reset);
    device_class_set_props(dc, voodoo2_properties);
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo voodoo2_info = {
    .name          = TYPE_VOODOO2,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(Voodoo2State),
    .class_init    = voodoo2_class_init,
    .interfaces    = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void
voodoo2_register_types(void)
{
    type_register_static(&voodoo2_info);
}

type_init(voodoo2_register_types)
