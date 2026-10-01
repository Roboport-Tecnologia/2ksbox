/*
 * embedaudio.c: QEMU audio backend that clips the mixed guest output
 * straight into a caller-owned SPSC ring buffer (libqemu_embed.h:
 * qemu_embed_set_audio_ring). Launch with:
 *   -audiodev embed,id=snd0,out.frequency=48000,out.channels=2,out.format=f32
 *                   [,out.buffer-length=40000]
 * fixed_settings (default) yields exactly one HWVoiceOut in that format. The
 * backend moves bytes and does not care which; the player asks for f32
 * because QEMU's s16 conversion saturates a mix that sums past full scale
 * and its float conversion does not (player/src/audio.rs has the limiter).
 *
 * Producer (QEMU main loop, BQL held, one mixer tick every timer-period =
 * 10 ms) advances wr; consumer (host audio thread, the DAC's clock) advances
 * rd. Indices are byte positions in [0, size) with one slot kept free; size
 * must be a power of two.
 *
 * Pacing. The guest is drained at the pace of its own clock and never in a
 * burst, because a sound card's DMA moves exactly as far as it is drained:
 * QEMU's sb16 and AC97 copy what the mixer asks for out of the guest's
 * buffer and move the guest's play cursor by as much, and a driver that
 * writes a margin ahead of that cursor (DirectSound's scheme; a DOS game's
 * double buffer is the same with a bigger margin) plays what it has not
 * written yet as soon as the cursor jumps further than its margin. That was
 * the crackle (tools/audio-glitch-test.py): this backend used to top the
 * ring up to its target on every tick, so a host device that drains the
 * ring a whole period at a time (1024 frames from PipeWire, 2048 when
 * another client asks for it, 4096 from plain ALSA hardware) pulled the
 * guest ahead by up to that much at once, and the surplus that followed was
 * trimmed by dropping whole ticks, each drop a click of its own.
 *
 * So each tick takes what the guest's clock says has played since the last
 * one, times a small correction. A main loop that was held up (a 3D
 * title's swap finishing under the big lock, tools/audio-glitch-test.py's
 * STALL=) is paid back over the ticks that follow, never in one. A device
 * may deliver at most three ticks' worth between two ticks (the player runs
 * this audiodev at timer-period=5000, so 15 ms), and up to 100 ms stays
 * owed meanwhile. Both halves were measured wrong first: handing a 10-14 ms
 * stall's backlog out at once moved the card's cursor ~25 ms past what the
 * guest (held up too) had written, and forgetting a stall instead drained
 * the ring under a loop that is held up every frame. The correction holds
 * the ring's *minimum* over a quarter second at out.buffer-length (the
 * cushion under the consumer's own pull, whatever its period) by draining
 * the guest up to 25 % fast or 10 % slow. The host DAC's drift against the
 * guest's clock is parts per million, so in steady state the correction is
 * nil; after a stall it refills the cushion within about a second, a
 * fraction of a millisecond per tick, which no guest driver can tell from
 * its own clock. Starting a stream is the consumer's side: it waits until
 * the ring holds the cushion plus one of its own periods before it plays
 * (player/src/audio.rs), because this side will not hurry the guest to fill
 * it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/host-utils.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/audio.h"
#include "qom/object.h"
#include "libqemu_embed.h"

#define AUDIO_CAP "embed"
#include "audio/audio_int.h"

#define TYPE_AUDIO_EMBED "audio-embed"
OBJECT_DECLARE_SIMPLE_TYPE(AudioEmbed, AUDIO_EMBED)

struct AudioEmbed {
    AudioMixengBackend parent_obj;
};

static uint8_t *ring_base;
static size_t ring_size;               /* power of two */
static uint32_t *ring_wr;              /* producer index (us) */
static const uint32_t *ring_rd;        /* consumer index (host) */

void qemu_embed_set_audio_ring(void *base, size_t bytes,
                               uint32_t *wr_idx, const uint32_t *rd_idx)
{
    ring_base = base;
    ring_size = bytes;
    ring_wr = wr_idx;
    ring_rd = rd_idx;
}

/* the correction's range */
#define ADJ_MAX     0.25
#define ADJ_MIN    -0.10
/* the ring's minimum is judged over this, and an error corrected over about
 * twice it */
#define WINDOW_NS   250000000LL
#define SETTLE_S    0.5

typedef struct EmbedVoiceOut {
    HWVoiceOut hw;
    RateCtl rate;         /* pacing with no consumer attached */
    size_t floor;         /* bytes: the least the ring should hold */
    int64_t last_ns;      /* QEMU_CLOCK_VIRTUAL at the last tick */
    double owed;          /* frames the guest's clock has played, not yet taken */
    double burst;         /* frames: the most a device may deliver ahead of a tick */
    double cap;           /* frames: the most that may be owed; beyond, forgotten */
    double adj;           /* the rate correction */
    int64_t win_start;
    size_t win_min;       /* bytes: the ring's least fill in this window */
    size_t pending;       /* bytes handed out by get_buffer_out, not yet put */
    bool pending_drop;    /* ... into the scratch buffer, not the ring */
    /* statistics, stderr every 5 s when something was off */
    bool empty;           /* the ring was empty at the last tick */
    unsigned gaps;        /* ticks that found the ring empty (host heard silence) */
    size_t dropped;       /* bytes discarded */
    size_t min_fill;      /* bytes, since the last report */
    int64_t last_report;  /* g_get_monotonic_time */
    bool started;         /* something has been written since enable */
} EmbedVoiceOut;

static uint8_t scratch[16384];

/* QEMU_EMBED_AUDIO_TRACE=1: a line per call on stderr */
static bool trace;

static inline size_t ring_free(void)
{
    uint32_t wr = qatomic_read(ring_wr);
    uint32_t rd = qatomic_load_acquire(ring_rd);
    return (rd - wr - 1) & (ring_size - 1);
}

static inline size_t ring_fill(void)
{
    return ring_size - 1 - ring_free();
}

static inline size_t bytes_to_ms(HWVoiceOut *hw, size_t b)
{
    return hw->info.bytes_per_second ? b * 1000 / hw->info.bytes_per_second : 0;
}

static void report(EmbedVoiceOut *vo, HWVoiceOut *hw, bool force)
{
    int64_t now = g_get_monotonic_time();
    if (!force && now - vo->last_report < 5 * 1000000) {
        return;
    }
    if (vo->gaps || vo->dropped) {
        fprintf(stderr, "qemu-embed: audio: %u gaps, %zu ms dropped, ring at least "
                "%zu ms (cushion %zu ms), guest drained %+.1f%% fast\n",
                vo->gaps, bytes_to_ms(hw, vo->dropped),
                vo->min_fill == SIZE_MAX ? 0 : bytes_to_ms(hw, vo->min_fill),
                bytes_to_ms(hw, vo->floor), vo->adj * 100);
    }
    vo->last_report = now;
    vo->gaps = 0;
    vo->dropped = 0;
    vo->min_fill = SIZE_MAX;
}

/* Once per mixer tick: what the guest's clock has played since the last
 * one, and the correction for the window that just closed. */
static void tick(EmbedVoiceOut *vo, HWVoiceOut *hw)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t dt = now - vo->last_ns;
    size_t fill = ring_fill();

    vo->last_ns = now;
    if (dt > 0) {
        vo->owed += dt * 1e-9 * hw->info.freq * (1.0 + vo->adj);
        if (vo->owed > vo->cap) {
            vo->owed = vo->cap;
        }
    }
    if (vo->started) {
        if (fill < vo->min_fill) {
            vo->min_fill = fill;
        }
        if (fill == 0 && !vo->empty) {
            vo->gaps++;
        }
        vo->empty = fill == 0;
    }
    if (fill < vo->win_min) {
        vo->win_min = fill;
    }
    if (trace) {
        fprintf(stderr, "embedaudio: t=%" PRId64 " ms dt=%" PRId64 " us owed=%.0f "
                "fill=%zu adj=%+.3f\n", now / SCALE_MS, dt / SCALE_US, vo->owed,
                fill / hw->info.bytes_per_frame, vo->adj);
    }
    if (now - vo->win_start >= WINDOW_NS) {
        double err = (double)vo->floor - (double)vo->win_min;
        vo->adj = err / (hw->info.bytes_per_second * SETTLE_S);
        vo->adj = MIN(MAX(vo->adj, ADJ_MIN), ADJ_MAX);
        vo->win_start = now;
        vo->win_min = SIZE_MAX;
    }
    report(vo, hw, false);
}

static size_t embed_buffer_get_free(HWVoiceOut *hw)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    size_t max = hw->samples * hw->info.bytes_per_frame;
    if (!ring_base) {
        return max;
    }
    tick(vo, hw);
    /* What a device may deliver now: what is owed, but never more than the
     * burst. This is asked by every audio_be_write, not only by the mixer tick
     * (a DMA card writes from i8257's bottom half between ticks), so it
     * bounds how far a device, and so the guest's play cursor, runs ahead
     * of the ring between two ticks. */
    return MIN((size_t)MIN(vo->owed, vo->burst) * hw->info.bytes_per_frame, max);
}

static void *embed_get_buffer_out(HWVoiceOut *hw, size_t *size)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    size_t bpf = hw->info.bytes_per_frame;

    if (!ring_base) {
        /* no consumer attached: behave like the 'none' backend */
        *size = MIN(*size, sizeof(scratch));
        *size = MIN(*size, audio_rate_peek_bytes(&vo->rate, &hw->info));
        *size -= *size % bpf;
        vo->pending = *size;
        vo->pending_drop = true;
        return scratch;
    }

    size_t n = MIN(*size, (size_t)vo->owed * bpf);
    n -= n % bpf;
    if (!n) {
        *size = 0;
        return NULL;
    }
    uint32_t wr = qatomic_read(ring_wr);
    size_t m = MIN(n, MIN(ring_free(), ring_size - wr));
    m -= m % bpf;
    if (m) {
        vo->pending = m;
        vo->pending_drop = false;
        vo->started = true;
        *size = m;
        return ring_base + wr;
    }
    /* The ring is full: the consumer has stopped taking. What the guest's
     * clock says has played goes nowhere, as it would on a card nobody
     * listened to. */
    n = MIN(n, sizeof(scratch));
    n -= n % bpf;
    vo->pending = n;
    vo->pending_drop = true;
    vo->dropped += n;
    *size = n;
    return scratch;
}

static size_t embed_put_buffer_out(HWVoiceOut *hw, void *buf, size_t size)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    assert(size <= vo->pending);
    audio_rate_add_bytes(&vo->rate, size);
    if (ring_base) {
        vo->owed -= size / hw->info.bytes_per_frame;
        if (vo->owed < 0) {
            vo->owed = 0;
        }
    }
    if (!vo->pending_drop) {
        uint32_t wr = qatomic_read(ring_wr);
        assert(buf == ring_base + wr);
        qatomic_store_release(ring_wr, (uint32_t)((wr + size) & (ring_size - 1)));
    }
    vo->pending = 0;
    return size;
}

static size_t embed_write(HWVoiceOut *hw, void *buf, size_t len)
{
    /* mixeng=off path: copy into the ring in up to two chunks */
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        void *dst = embed_get_buffer_out(hw, &n);
        if (!n) {
            break;
        }
        memcpy(dst, (uint8_t *)buf + done, n);
        embed_put_buffer_out(hw, dst, n);
        done += n;
    }
    return done;
}

static void restart(EmbedVoiceOut *vo)
{
    audio_rate_start(&vo->rate);
    vo->last_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    vo->owed = 0;
    vo->win_start = vo->last_ns;
    vo->win_min = SIZE_MAX;
    vo->started = false;
    vo->empty = false;
}

static int embed_init_out(HWVoiceOut *hw, struct audsettings *as)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    Audiodev *dev = hw->s->dev;
    int64_t period_ns = hw->s && hw->s->period_ticks > 0 ? hw->s->period_ticks
                                                          : 10 * SCALE_MS;

    audio_pcm_init_info(&hw->info, as);
    size_t bpf = hw->info.bytes_per_frame;
    /* out.buffer-length is the cushion: the least the ring holds under the
     * consumer's pull */
    size_t frames = audio_buffer_frames(dev->u.embed.out, as, 40000);
    vo->floor = frames * bpf;
    if (ring_base && vo->floor > ring_size / 4) {
        vo->floor = ring_size / 4;
        vo->floor -= vo->floor % bpf;
    }
    vo->burst = 3 * period_ns * 1e-9 * hw->info.freq;
    vo->cap = 0.1 * hw->info.freq;
    trace = getenv("QEMU_EMBED_AUDIO_TRACE") != NULL;
    /* the mixer must be able to hold one tick's take */
    hw->samples = MAX(vo->floor / bpf, (size_t)vo->burst + 1);
    vo->adj = 0;
    vo->min_fill = SIZE_MAX;
    vo->last_report = g_get_monotonic_time();
    restart(vo);
    return 0;
}

static void embed_fini_out(HWVoiceOut *hw)
{
}

static void embed_enable_out(HWVoiceOut *hw, bool enable)
{
    EmbedVoiceOut *vo = container_of(hw, EmbedVoiceOut, hw);
    if (enable) {
        restart(vo);
    } else {
        /* a short stream (a system sound) never reaches the 5 s report */
        report(vo, hw, true);
    }
}

static void audio_embed_class_init(ObjectClass *klass, const void *data)
{
    AudioMixengBackendClass *k = AUDIO_MIXENG_BACKEND_CLASS(klass);

    k->max_voices_out = 1;
    k->max_voices_in = 0;
    k->voice_size_out = sizeof(EmbedVoiceOut);
    k->voice_size_in = 0;

    k->init_out = embed_init_out;
    k->fini_out = embed_fini_out;
    k->write = embed_write;
    k->buffer_get_free = embed_buffer_get_free;
    k->get_buffer_out = embed_get_buffer_out;
    k->put_buffer_out = embed_put_buffer_out;
    k->run_buffer_out = audio_generic_run_buffer_out;
    k->enable_out = embed_enable_out;
}

/* -audiodev embed: QEMU makes an audiodev's backend as type audio-<driver> */
static const TypeInfo audio_types[] = {
    {
        .name = TYPE_AUDIO_EMBED,
        .parent = TYPE_AUDIO_MIXENG_BACKEND,
        .instance_size = sizeof(AudioEmbed),
        .class_init = audio_embed_class_init,
    },
};

DEFINE_TYPES(audio_types)
