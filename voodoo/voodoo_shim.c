/*
 * voodoo_shim.c: 86Box's platform, as the vendored Voodoo sources see it,
 * implemented over QEMU: threads and events (qemu-thread), timers
 * (QEMUTimer on the virtual clock, see shim/86box/timer.h for the units),
 * executable memory for the recompiler, the device configuration table, the
 * monitor bitmap the display timer paints into, the "SVGA override" that
 * hands the monitor to the Voodoo, and stubs for the four Banshee entry
 * points a Voodoo 1/2 never reaches.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/thread.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qemu/rcu.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#ifndef _WIN32
#include <sys/mman.h>
#endif

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
#include "voodoo_shim.h"

/* ------------------------------------------------------------ cpu.h, pci.h */

int voodoo_shim_cycles;
int pci_burst_time    = 1;
int pci_nonburst_time = 4;

void
pci_add_card(uint8_t add_type,
             uint8_t (*read)(int func, int addr, int len, void *priv),
             void (*write)(int func, int addr, int len, uint8_t val, void *priv),
             void *priv, uint8_t *slot)
{
    /* The QEMU PCI device calls voodoo_pci_read/write by name; nothing to
     * register. The slot number is what the card reads back nowhere. */
    (void) add_type;
    (void) read;
    (void) write;
    (void) priv;
    *slot = 0;
}

/* ------------------------------------------------------------------ mem.h */

void
mem_mapping_add(mem_mapping_t *map, uint32_t base, uint32_t size,
                uint8_t (*read_b)(uint32_t addr, void *priv),
                uint16_t (*read_w)(uint32_t addr, void *priv),
                uint32_t (*read_l)(uint32_t addr, void *priv),
                void (*write_b)(uint32_t addr, uint8_t val, void *priv),
                void (*write_w)(uint32_t addr, uint16_t val, void *priv),
                void (*write_l)(uint32_t addr, uint32_t val, void *priv),
                uint8_t *exec, uint32_t flags, void *priv)
{
    memset(map, 0, sizeof(*map));
    map->base    = base;
    map->size    = size;
    map->read_b  = read_b;
    map->read_w  = read_w;
    map->read_l  = read_l;
    map->write_b = write_b;
    map->write_w = write_w;
    map->write_l = write_l;
    map->exec    = exec;
    map->flags   = flags;
    map->priv    = priv;
    map->enable  = (size != 0);
}

void
mem_mapping_set_addr(mem_mapping_t *map, uint32_t base, uint32_t size)
{
    map->base   = base;
    map->size   = size;
    map->enable = 1;
}

void
mem_mapping_disable(mem_mapping_t *map)
{
    map->enable = 0;
}

void
mem_mapping_enable(mem_mapping_t *map)
{
    map->enable = 1;
}

/* Only the command FIFO's AGP mode reads guest RAM, and only a Banshee has
 * one; kept real rather than stubbed (the FIFO thread is RCU-registered for
 * it) so a future card type does not fault here. */
uint32_t
mem_readl_phys(uint32_t addr)
{
    return address_space_ldl_le(&address_space_memory, addr,
                                MEMTXATTRS_UNSPECIFIED, NULL);
}

/* --------------------------------------------------------------- 86box.h */

void
pclog(const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "voodoo2: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void
pclog_ex(const char *fmt, va_list ap)
{
    fprintf(stderr, "voodoo2: ");
    vfprintf(stderr, fmt, ap);
}

static VoodooShimHooks shim_hooks;
unsigned               voodoo_shim_fatals;

/* 86Box's fatal() ends 86Box; here it must not end QEMU, because every
 * caller is a guest write the chip would have swallowed: an intrCtrl the
 * emulation has no interrupts for, a command-FIFO packet the decoder does
 * not know, an LFB format it cannot convert. Each site `break`s or falls
 * through after the call, so returning is safe: the write is refused, the
 * stream goes on. The first is reported with the device's dump (the last
 * accesses and the FIFO state), the rest are counted for the 5 s line. */
void
fatal_ex(const char *fmt, va_list ap)
{
    voodoo_shim_fatals++;
    if (voodoo_shim_fatals > 1) {
        return;
    }
    fprintf(stderr, "voodoo2: refused (86Box fatal): ");
    vfprintf(stderr, fmt, ap);
    if (shim_hooks.on_fatal) {
        shim_hooks.on_fatal(shim_hooks.opaque);
    }
    fflush(stderr);
}

void
fatal(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    fatal_ex(fmt, ap);
    va_end(ap);
}

/* --------------------------------------------------- guarded allocations
 * (shim/86box/86box.h: calloc/free of the vendored files come here) */

#define SHIM_GUARD_MIN   (1024 * 1024)      /* allocations this big are mapped */
#define SHIM_GUARD_BYTES (64 * 1024 * 1024) /* zero pages after them */

static struct {
    void  *p;
    size_t len;
} shim_guarded[8];

#undef calloc
#undef free

void *
voodoo_shim_calloc(size_t n, size_t size)
{
    size_t bytes = n * size;

    if (bytes >= SHIM_GUARD_MIN) {
        for (int i = 0; i < (int) ARRAY_SIZE(shim_guarded); i++) {
            if (!shim_guarded[i].p) {
                void *p = plat_mmap(bytes + SHIM_GUARD_BYTES, 0, NULL);

                if (!p) {
                    return NULL;
                }
                shim_guarded[i].p   = p;
                shim_guarded[i].len = bytes + SHIM_GUARD_BYTES;
                return p;   /* anonymous pages: zero, like calloc */
            }
        }
        /* the table is full: an ordinary allocation, no guard */
    }
    return calloc(n, size);
}

void
voodoo_shim_free(void *p)
{
    if (!p) {
        return;
    }
    for (int i = 0; i < (int) ARRAY_SIZE(shim_guarded); i++) {
        if (shim_guarded[i].p == p) {
            plat_munmap(p, shim_guarded[i].len);
            shim_guarded[i].p = NULL;
            return;
        }
    }
    free(p);
}

/* ---------------------------------------------------------------- plat.h */

uint64_t timer_freq = 1000000000ULL;

void *
plat_mmap(size_t size, uint8_t executable, uint8_t *large)
{
    if (large) {
        *large = 0;
    }
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                        executable ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
#else
    int prot  = PROT_READ | PROT_WRITE | (executable ? PROT_EXEC : 0);
    int flags = MAP_ANON | MAP_PRIVATE;
#if defined(__APPLE__) && defined(MAP_JIT)
    /* Apple Silicon: W^X per thread, toggled by the arm64 recompiler itself
     * with pthread_jit_write_protect_np; the entitlement is the player's
     * (com.apple.security.cs.allow-jit, the same one TCG needs). */
    if (executable) {
        flags |= MAP_JIT;
    }
#endif
    void *p = mmap(NULL, size, prot, flags, -1, 0);
    return (p == MAP_FAILED) ? NULL : p;
#endif
}

void
plat_munmap(void *ptr, size_t size)
{
    if (!ptr) {
        return;
    }
#ifdef _WIN32
    (void) size;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, size);
#endif
}

uint64_t
plat_timer_read(void)
{
    return get_clock();
}

void
plat_delay_ms(uint32_t count)
{
    g_usleep((gulong) count * 1000);
}

/* -------------------------------------------------------------- thread.h */

typedef struct shim_thread_t {
    QemuThread th;
    void (*fn)(void *param);
    void *param;
} shim_thread_t;

static void *
shim_thread_tramp(void *arg)
{
    shim_thread_t *t = arg;

    rcu_register_thread();
    t->fn(t->param);
    rcu_unregister_thread();
    return NULL;
}

thread_t *
thread_create_named(void (*thread_func)(void *param), void *param, const char *name)
{
    shim_thread_t *t = g_new0(shim_thread_t, 1);

    t->fn    = thread_func;
    t->param = param;
    qemu_thread_create(&t->th, name, shim_thread_tramp, t, QEMU_THREAD_JOINABLE);
    return t;
}

int
thread_wait(thread_t *arg)
{
    shim_thread_t *t = arg;

    if (!t) {
        return 0;
    }
    qemu_thread_join(&t->th);
    g_free(t);
    return 0;
}

typedef struct shim_event_t {
    QemuMutex mutex;
    QemuCond  cond;
    bool      state;
} shim_event_t;

event_t *
thread_create_event(void)
{
    shim_event_t *ev = g_new0(shim_event_t, 1);

    qemu_mutex_init(&ev->mutex);
    qemu_cond_init(&ev->cond);
    return ev;
}

void
thread_set_event(event_t *arg)
{
    shim_event_t *ev = arg;

    qemu_mutex_lock(&ev->mutex);
    ev->state = true;
    qemu_mutex_unlock(&ev->mutex);
    qemu_cond_broadcast(&ev->cond);
}

void
thread_reset_event(event_t *arg)
{
    shim_event_t *ev = arg;

    qemu_mutex_lock(&ev->mutex);
    ev->state = false;
    qemu_mutex_unlock(&ev->mutex);
}

/* 0 = the event was set, 1 = timed out (milliseconds; < 0 waits for ever) */
int
thread_wait_event(event_t *arg, int timeout)
{
    shim_event_t *ev = arg;
    int           ret = 0;

    qemu_mutex_lock(&ev->mutex);
    if (timeout < 0) {
        while (!ev->state) {
            qemu_cond_wait(&ev->cond, &ev->mutex);
        }
    } else {
        int64_t deadline = get_clock() + (int64_t) timeout * 1000000;

        while (!ev->state) {
            int64_t left = deadline - get_clock();

            if (left <= 0) {
                ret = 1;
                break;
            }
            qemu_cond_timedwait(&ev->cond, &ev->mutex, MAX(1, (int) (left / 1000000)));
        }
    }
    qemu_mutex_unlock(&ev->mutex);
    return ret;
}

void
thread_destroy_event(event_t *arg)
{
    shim_event_t *ev = arg;

    if (!ev) {
        return;
    }
    qemu_cond_destroy(&ev->cond);
    qemu_mutex_destroy(&ev->mutex);
    g_free(ev);
}

mutex_t *
thread_create_mutex(void)
{
    QemuMutex *m = g_new0(QemuMutex, 1);

    qemu_mutex_init(m);
    return m;
}

void
thread_close_mutex(mutex_t *arg)
{
    QemuMutex *m = arg;

    if (!m) {
        return;
    }
    qemu_mutex_destroy(m);
    g_free(m);
}

int
thread_test_mutex(mutex_t *arg)
{
    return qemu_mutex_trylock(arg) == 0;
}

int
thread_wait_mutex(mutex_t *arg)
{
    qemu_mutex_lock(arg);
    return 1;
}

int
thread_release_mutex(mutex_t *arg)
{
    qemu_mutex_unlock(arg);
    return 1;
}

/* --------------------------------------------------------------- timer.h */

uint64_t TIMER_USEC = 1000ULL << 32;

static uint64_t
shim_now_ns(void)
{
    return (uint64_t) qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

uint64_t
voodoo_shim_now_ts(void)
{
    return shim_now_ns() << VOODOO_SHIM_TS_SHIFT;
}

uint64_t
voodoo_shim_tsc(void)
{
    return shim_now_ns();
}

/* Upstream's contract: the timer is disabled when its callback runs and stays
 * so unless the callback re-arms it. A callback that re-arms itself less than
 * slack_ns ahead is run again at once (the per-scanline display timer:
 * ~32 us a line, coalesced to ~1 ms of lines per main-loop wakeup). */
static void
shim_timer_fire(void *opaque)
{
    pc_timer_t *t   = opaque;
    uint64_t    now = shim_now_ns();
    int         n   = 0;

    t->in_fire = 1;
    do {
        t->enabled = 0;
        t->callback(t->priv);
    } while (t->enabled && (t->ts >> VOODOO_SHIM_TS_SHIFT) <= now + t->slack_ns && ++n < 4096);
    t->in_fire = 0;
    if (t->enabled) {
        timer_mod_ns(t->qt, (int64_t) (t->ts >> VOODOO_SHIM_TS_SHIFT));
    }
}

void
timer_add(pc_timer_t *timer, void (*callback)(void *priv), void *priv, int start_timer)
{
    memset(timer, 0, sizeof(*timer));
    timer->callback = callback;
    timer->priv     = priv;
    timer->qt       = timer_new_ns(QEMU_CLOCK_VIRTUAL, shim_timer_fire, timer);
    if (start_timer) {
        timer->ts = voodoo_shim_now_ts();
        timer_enable(timer);
    }
}

void
timer_enable(pc_timer_t *timer)
{
    timer->enabled = 1;
    if (!timer->in_fire) {
        timer_mod_ns(timer->qt, (int64_t) (timer->ts >> VOODOO_SHIM_TS_SHIFT));
    }
}

void
timer_disable(pc_timer_t *timer)
{
    timer->enabled = 0;
    if (!timer->in_fire) {
        timer_del(timer->qt);
    }
}

/* -------------------------------------------------------------- device.h */

static struct {
    const char *name;
    int         value;
} shim_config[16];
static int shim_config_n;

void
voodoo_shim_set_config(const char *name, int value)
{
    for (int i = 0; i < shim_config_n; i++) {
        if (!strcmp(shim_config[i].name, name)) {
            shim_config[i].value = value;
            return;
        }
    }
    assert(shim_config_n < (int) ARRAY_SIZE(shim_config));
    shim_config[shim_config_n].name    = name;
    shim_config[shim_config_n++].value = value;
}

int
device_get_config_int(const char *name)
{
    for (int i = 0; i < shim_config_n; i++) {
        if (!strcmp(shim_config[i].name, name)) {
            return shim_config[i].value;
        }
    }
    warn_report("voodoo2: no configuration for '%s', using 0", name);
    return 0;
}

const char *
device_get_config_bios(const char *name)
{
    (void) name;
    return "voodoo_2";
}

uint32_t
device_get_bios_local(const device_t *dev, const char *internal_name)
{
    (void) dev;
    (void) internal_name;
    return VOODOO_2 | (VOODOO_GENERIC << 10);
}

uint64_t
device_get_bios_flags(const device_t *dev, const char *internal_name)
{
    (void) dev;
    (void) internal_name;
    return 0;
}

/* ------------------------------------------------------ video.h, vid_svga.h */

monitor_t monitors[1];
int       monitor_index_global;
double    cpuclock = 1e9;   /* "TSC" ticks per second: the timers count ns */

static bitmap_t        shim_bitmap;
static svga_t          shim_svga;

void
voodoo_shim_init(const VoodooShimHooks *hooks)
{
    shim_hooks = *hooks;
    if (!shim_bitmap.dat) {
        size_t size = (size_t) VOODOO_SHIM_BITMAP_W * VOODOO_SHIM_BITMAP_H * 4;

        shim_bitmap.w   = VOODOO_SHIM_BITMAP_W;
        shim_bitmap.h   = VOODOO_SHIM_BITMAP_H;
        shim_bitmap.dat = plat_mmap(size, 0, NULL);   /* lazily committed */
        if (!shim_bitmap.dat) {
            error_report("voodoo2: cannot map the %zu-byte monitor bitmap", size);
            exit(1);
        }
        for (int y = 0; y < VOODOO_SHIM_BITMAP_H; y++) {
            shim_bitmap.line[y] = shim_bitmap.dat + (size_t) y * VOODOO_SHIM_BITMAP_W;
        }
    }
    monitors[0].target_buffer = &shim_bitmap;
    shim_svga.monitor         = &monitors[0];
}

void
video_wait_for_buffer_monitor(int monitor_index)
{
    (void) monitor_index;
}

void
video_clamp_vram(uint64_t bios_flags, int *size)
{
    (void) bios_flags;
    (void) size;
}

svga_t *
svga_get_pri(void)
{
    return &shim_svga;
}

void
svga_set_override(svga_t *svga, int val)
{
    svga->override = val;
    if (shim_hooks.set_override) {
        shim_hooks.set_override(shim_hooks.opaque, val);
    }
}

void
svga_recalctimings(svga_t *svga)
{
    (void) svga;
}

/* voodoo_callback: svga_doblit(h_disp, v_disp - 1, svga) at the end of a
 * frame with dirty lines */
void
svga_doblit(int wx, int wy, svga_t *svga)
{
    (void) svga;
    if (shim_hooks.present) {
        shim_hooks.present(shim_hooks.opaque, &shim_bitmap, wx, wy + 1);
    }
}

/* ----------------------------------------------------------- Banshee stubs
 *
 * vid_voodoo_banshee*.c are not vendored (a Banshee is a 2D+3D card with
 * its own VGA core). The Voodoo 1/2 files reach these only for
 * type >= VOODOO_BANSHEE, or through FIFO entries only Banshee code queues.
 */
void banshee_cmd_write(void *priv, uint32_t addr, uint32_t val);
void banshee_set_overlay_addr(void *priv, uint32_t addr);
void voodoo_2d_reg_writel(voodoo_t *voodoo, uint32_t addr, uint32_t val);

void
banshee_cmd_write(void *priv, uint32_t addr, uint32_t val)
{
    (void) priv;
    fatal("Banshee AGP command %08x=%08x on a Voodoo 2\n", addr, val);
}

void
banshee_set_overlay_addr(void *priv, uint32_t addr)
{
    (void) priv;
    fatal("Banshee overlay address %08x on a Voodoo 2\n", addr);
}

void
voodoo_2d_reg_writel(voodoo_t *voodoo, uint32_t addr, uint32_t val)
{
    (void) voodoo;
    fatal("Banshee 2D register %08x=%08x on a Voodoo 2\n", addr, val);
}

void
voodoo_generate_vb_filters(voodoo_t *voodoo, int fcr, int fcg)
{
    (void) voodoo;
    (void) fcr;
    (void) fcg;
}
