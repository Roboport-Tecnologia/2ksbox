/*
 * libqemu_embed.c: run QEMU in-process behind a small C API.
 * See libqemu_embed.h for the thread contract. Design: docs/11-m1-embed-api.md
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "block/aio.h"
#include "sysemu/sysemu.h"
#include "sysemu/runstate.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/input.h"
#include "qemu-main.h"
#include "libqemu_embed.h"
#include "embedfx.h"
/* The two pad devices' host-side entry points (M13 paths A and B).
 * Copied in beside this file by scripts/prepare-qemu.sh, from
 * gamepad/qemu/. A machine has one device or the other and this file
 * does not have to know which: each call is a no-op when its device is
 * absent. */
#include "usb-gamepad.h"
#include "gameport.h"
/* CONFIG_GAMEPORT: the gameport is an ISA device, which the aarch64
 * target (Windows 11 on Arm) does not build. */
#include CONFIG_DEVICES

#ifdef _WIN32
#include <io.h>                 /* _open_osfhandle() for qemu_embed_socket_to_fd */
#endif

/*
 * system/main.c is not part of the library, but ui/cocoa.m (macOS) still
 * references qemu_default_main()/qemu_main: provide upstream's definitions.
 * Unused in the embed flow (-display none); the caller drives the loop via
 * qemu_embed_run().
 */
int qemu_default_main(void)
{
    int status = qemu_main_loop();
    qemu_cleanup(status);
    return status;
}
int (*qemu_main)(void) = qemu_default_main;

enum { EV_KEY, EV_REL, EV_ABS, EV_BTN, EV_PAD };

typedef struct {
    uint8_t kind;
    bool down;
    int32_t a, b, c, d;
    int64_t t;              /* enqueue time (g_get_monotonic_time) */
} in_event;

/* input path statistics (stderr, "qemu-embed: input: …"): how late the main
 * loop drains the queue, key presses whose down and up landed in the same
 * drain (a zero-length press: a game polling the keyboard state never sees
 * it), events dropped on a full queue. Found with FIFA 2000 under TCG. */
typedef struct {
    unsigned drains, events, dropped, coalesced, coalesced_logged;
    int64_t max_lat, last_report;
} in_stats;

#define IN_QUEUE_LEN 512

struct qemu_embed {
    DisplayChangeListener dcl;
    qemu_embed_display_cb cb;
    void *ud;
    QemuConsole *con;
    bool follow_pending;    /* bh_follow_console scheduled */
    uint32_t win_w, win_h, win_dpi;     /* qemu_embed_set_window_size, 0 = none */
    bool win_seen;          /* the console was told a size at least once */

    QemuMutex in_lock;
    in_event in_q[IN_QUEUE_LEN];
    unsigned in_n;
    bool in_bh_pending;
    in_stats in_st;

    int argc;
    char **argv;
    uint32_t refresh_ms;
    uint32_t *flip;         /* row-flipped copy of a bottom-up 3D frame */
    size_t flip_len;
};

/* one VM per process: the 3D backend reports through this instance */
static qemu_embed_t *fx_instance;

void embed_fx_active(bool on)
{
    qemu_embed_t *e = fx_instance;
    if (e && e->cb.on_3d_active) {
        e->cb.on_3d_active(e->ud, on);
    }
}

void embed_fx_frame(const uint32_t *px, int w, int h, int stride, int bottom_up)
{
    qemu_embed_t *e = fx_instance;
    if (!e || !e->cb.on_3d_frame) {
        return;
    }
    if (bottom_up) {
        size_t need = (size_t)w * h;
        if (e->flip_len < need) {
            e->flip = g_realloc(e->flip, need * sizeof(uint32_t));
            e->flip_len = need;
        }
        const uint8_t *src = (const uint8_t *)px;
        for (int y = 0; y < h; y++) {
            memcpy(e->flip + (size_t)y * w, src + (size_t)(h - 1 - y) * stride,
                   (size_t)w * 4);
        }
        e->cb.on_3d_frame(e->ud, (const uint8_t *)e->flip, w, h, w * 4);
    } else {
        e->cb.on_3d_frame(e->ud, (const uint8_t *)px, w, h, stride);
    }
}

int embed_fx_dmabuf(int slot, int fd, int w, int h, int stride,
                    uint32_t fourcc, uint64_t modifier)
{
    qemu_embed_t *e = fx_instance;
    if (!e || !e->cb.on_3d_dmabuf || !e->cb.on_3d_frame_ready) {
        return 0;
    }
    int dupfd = dup(fd);
    if (dupfd < 0) {
        return 0;
    }
    if (!e->cb.on_3d_dmabuf(e->ud, slot, dupfd, w, h, stride, fourcc, modifier)) {
        close(dupfd);
        return 0;
    }
    return 1;
}

int embed_fx_iosurface(int slot, void *iosurface, int w, int h)
{
    qemu_embed_t *e = fx_instance;
    if (!e || !e->cb.on_3d_iosurface || !e->cb.on_3d_frame_ready) {
        return 0;
    }
    return e->cb.on_3d_iosurface(e->ud, slot, iosurface, w, h) ? 1 : 0;
}

void embed_fx_frame_ready(int slot)
{
    qemu_embed_t *e = fx_instance;
    if (e && e->cb.on_3d_frame_ready) {
        e->cb.on_3d_frame_ready(e->ud, slot);
    }
}

/* ---------------------------------------------------------------- display */

/*
 * The screen a machine with two adapters shows (M20, Windows 11 on Arm):
 * the last graphic console whose device has a picture, else the default
 * one. The `virt` board has `ramfb`, which the firmware and Windows setup
 * draw on, and then `virtio-gpu-pci`, which Windows draws on once
 * viogpudo is installed. virtio-gpu holds a placeholder surface whenever
 * no driver scans out (reset, the firmware's hand-over, a Windows without
 * the driver, its recovery), so following it needs no state of our own.
 * A machine with one adapter always gets that one.
 */
static void embed_tell_window_size(qemu_embed_t *e, bool delay);

static QemuConsole *embed_live_console(void)
{
    QemuConsole *live = NULL;
    for (unsigned i = 0;; i++) {
        QemuConsole *con = qemu_console_lookup_by_index(i);
        if (!con) {
            break;
        }
        if (qemu_console_is_graphic(con) &&
            !surface_is_placeholder(qemu_console_surface(con))) {
            live = con;
        }
    }
    return live ? live : qemu_console_lookup_default();
}

/* Out of the listener's own refresh: re-registering inside it would edit
 * the display state's listener list while QEMU walks it. Registering fires
 * gfx_switch with the new console's surface, so the player sees a mode
 * change and nothing else. */
static void bh_follow_console(void *opaque)
{
    qemu_embed_t *e = opaque;
    QemuConsole *con = embed_live_console();
    e->follow_pending = false;
    if (con == e->con) {
        return;
    }
    g_autofree char *label = qemu_console_get_label(con);
    fprintf(stderr, "qemu-embed: showing console %d (%s)\n", qemu_console_get_index(con), label);
    unregister_displaychangelistener(&e->dcl);
    qatomic_set(&e->con, con);
    e->dcl.con = con;
    register_displaychangelistener(&e->dcl);
    e->win_seen = false;
    embed_tell_window_size(e, false);
}

/* The window's size to the console on show. `delay` is QEMU's own
 * one-second settle, for a window being dragged; the first size a console
 * hears goes at once, so a guest that starts drawing gets the window's size
 * as its first mode. Main loop. */
static void embed_tell_window_size(qemu_embed_t *e, bool delay)
{
    uint32_t w = qatomic_read(&e->win_w), h = qatomic_read(&e->win_h);
    uint32_t dpi = qatomic_read(&e->win_dpi);
    if (!w || !h || !dpy_ui_info_supported(e->con)) {
        return;
    }
    QemuUIInfo info = *dpy_get_ui_info(e->con);
    info.width = w;
    info.height = h;
    if (dpi) {
        /* the size a monitor of this DPI would have: the EDID Windows picks
         * its recommended scaling from */
        info.width_mm = (uint16_t)MIN(w * 254u / (dpi * 10u), UINT16_MAX);
        info.height_mm = (uint16_t)MIN(h * 254u / (dpi * 10u), UINT16_MAX);
    }
    dpy_set_ui_info(e->con, &info, delay && e->win_seen);
    e->win_seen = true;
}

static void bh_window_size(void *opaque)
{
    embed_tell_window_size(opaque, true);
}

void qemu_embed_set_window_size(qemu_embed_t *e, uint32_t w, uint32_t h, uint32_t dpi)
{
    qatomic_set(&e->win_w, w);
    qatomic_set(&e->win_h, h);
    qatomic_set(&e->win_dpi, dpi);
    aio_bh_schedule_oneshot(qemu_get_aio_context(), bh_window_size, e);
}

bool qemu_embed_display_follows_window(qemu_embed_t *e)
{
    QemuConsole *con = qatomic_read(&e->con);
    return con && dpy_ui_info_supported(con);
}

static void embed_dpy_refresh(DisplayChangeListener *dcl)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    if (!e->follow_pending && embed_live_console() != e->con) {
        e->follow_pending = true;
        aio_bh_schedule_oneshot(qemu_get_aio_context(), bh_follow_console, e);
    }
    graphic_hw_update(dcl->con);
    if (e->cb.on_refresh_done) {
        e->cb.on_refresh_done(e->ud);
    }
}

static void embed_dpy_gfx_update(DisplayChangeListener *dcl,
                                 int x, int y, int w, int h)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    if (e->cb.on_update) {
        e->cb.on_update(e->ud, x, y, w, h);
    }
}

static void embed_dpy_gfx_switch(DisplayChangeListener *dcl,
                                 DisplaySurface *surface)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    if (e->cb.on_switch) {
        e->cb.on_switch(e->ud, surface_data(surface),
                        surface_width(surface), surface_height(surface),
                        surface_stride(surface), QEMU_EMBED_FMT_XRGB8888);
    }
}

static bool embed_dpy_gfx_check_format(DisplayChangeListener *dcl,
                                       pixman_format_code_t format)
{
    /* v1: one format for the host side; QEMU shadows everything else. */
    return format == PIXMAN_x8r8g8b8;
}

static void embed_dpy_mouse_set(DisplayChangeListener *dcl,
                                int x, int y, bool on)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    if (e->cb.on_mouse_set) {
        e->cb.on_mouse_set(e->ud, x, y, on);
    }
}

static void embed_dpy_cursor_define(DisplayChangeListener *dcl,
                                    QEMUCursor *c)
{
    qemu_embed_t *e = container_of(dcl, qemu_embed_t, dcl);
    if (e->cb.on_cursor) {
        if (c) {
            e->cb.on_cursor(e->ud, c->data, c->width, c->height,
                            c->hot_x, c->hot_y);
        } else {
            e->cb.on_cursor(e->ud, NULL, 0, 0, 0, 0);
        }
    }
}

static const DisplayChangeListenerOps embed_dcl_ops = {
    .dpy_name             = "embed",
    .dpy_refresh          = embed_dpy_refresh,
    .dpy_gfx_update       = embed_dpy_gfx_update,
    .dpy_gfx_switch       = embed_dpy_gfx_switch,
    .dpy_gfx_check_format = embed_dpy_gfx_check_format,
    .dpy_mouse_set        = embed_dpy_mouse_set,
    .dpy_cursor_define    = embed_dpy_cursor_define,
};

/* -------------------------------------------------------------- lifecycle */

uint32_t qemu_embed_api_version(void)
{
    return QEMU_EMBED_API_VERSION;
}

int qemu_embed_socket_to_fd(uint64_t sock)
{
#ifdef _WIN32
    /* QEMU's socket calls are the os-win32.h wrappers, and every one of them
     * starts with _get_osfhandle(fd): the `fd=` of a chardev is an index into
     * this library's CRT descriptor table, so a raw SOCKET handed straight
     * through fails as "File descriptor 'N' is not a socket". */
    return _open_osfhandle((intptr_t)sock, _O_BINARY);
#else
    return sock > INT_MAX ? -1 : (int)sock;
#endif
}

qemu_embed_t *qemu_embed_new(int argc, char **argv,
                             const qemu_embed_display_cb *cb, void *ud)
{
    if (argc < 1 || !argv) {
        return NULL;
    }
    qemu_embed_t *e = g_new0(qemu_embed_t, 1);
    if (cb) {
        e->cb = *cb;
    }
    e->ud = ud;
    qemu_mutex_init(&e->in_lock);

    /* argv + "-S" + "-display none" (+ NULL); we own the copies. */
    e->argc = argc + 3;
    e->argv = g_new0(char *, e->argc + 1);
    for (int i = 0; i < argc; i++) {
        e->argv[i] = g_strdup(argv[i]);
    }
    e->argv[argc]     = g_strdup("-S");
    e->argv[argc + 1] = g_strdup("-display");
    e->argv[argc + 2] = g_strdup("none");

    /* Takes the BQL on this thread; exits the process on fatal errors. */
    qemu_init(e->argc, e->argv);

    e->con = qemu_console_lookup_default();
    e->dcl.ops = &embed_dcl_ops;
    e->dcl.con = e->con;
    /* Fires on_switch/on_update/on_cursor synchronously before returning. */
    register_displaychangelistener(&e->dcl);
    /* qemu-3dfx: window-less context provider (doc 12) */
    fx_instance = e;
#ifdef TARGET_I386
    /* qemu-3dfx's OpenGL pass-through, the x86 targets' alone (patch 77) */
    embed_fx_register();
#endif
    return e;
}

int qemu_embed_run(qemu_embed_t *e)
{
    (void)e;
    return qemu_main_loop();
}

void qemu_embed_destroy(qemu_embed_t *e, int status)
{
    fx_instance = NULL;
    unregister_displaychangelistener(&e->dcl);
    qemu_cleanup(status);
    g_free(e->flip);
    qemu_mutex_destroy(&e->in_lock);
    for (int i = 0; i < e->argc; i++) {
        g_free(e->argv[i]);
    }
    g_free(e->argv);
    g_free(e);
}

static void bh_set_refresh(void *opaque)
{
    qemu_embed_t *e = opaque;
    update_displaychangelistener(&e->dcl, e->refresh_ms);
}

void qemu_embed_set_refresh_ms(qemu_embed_t *e, uint32_t ms)
{
    e->refresh_ms = ms ? ms : 1;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), bh_set_refresh, e);
}

/* ------------------------------------------------------------- vm control */

static void bh_vm_start(void *opaque)
{
    (void)opaque;
    if (!runstate_is_running()) {
        vm_start();
    }
}

void qemu_embed_vm_start(qemu_embed_t *e)
{
    aio_bh_schedule_oneshot(qemu_get_aio_context(), bh_vm_start, e);
}

void qemu_embed_vm_pause(qemu_embed_t *e)
{
    (void)e;
    qemu_system_vmstop_request(RUN_STATE_PAUSED);
}

void qemu_embed_vm_reset(qemu_embed_t *e)
{
    (void)e;
    qemu_system_reset_request(SHUTDOWN_CAUSE_HOST_QMP_SYSTEM_RESET);
}

void qemu_embed_vm_powerdown(qemu_embed_t *e)
{
    (void)e;
    qemu_system_powerdown_request();
}

void qemu_embed_vm_shutdown(qemu_embed_t *e)
{
    (void)e;
    qemu_system_shutdown_request(SHUTDOWN_CAUSE_HOST_QMP_QUIT);
}

bool qemu_embed_vm_running(qemu_embed_t *e)
{
    (void)e;
    return runstate_is_running();
}

/* ------------------------------------------------------------------ input */

static const InputButton button_map[] = {
    INPUT_BUTTON_LEFT, INPUT_BUTTON_MIDDLE, INPUT_BUTTON_RIGHT,
    INPUT_BUTTON_WHEEL_UP, INPUT_BUTTON_WHEEL_DOWN,
    INPUT_BUTTON_SIDE, INPUT_BUTTON_EXTRA,
};

static void enqueue(qemu_embed_t *e, in_event ev)
{
    ev.t = g_get_monotonic_time();
    qemu_mutex_lock(&e->in_lock);
    if (e->in_n < IN_QUEUE_LEN) {
        e->in_q[e->in_n++] = ev;
    } else {
        /* drop: a stalled main loop should not grow memory unbounded */
        if (e->in_st.dropped++ == 0) {
            fprintf(stderr, "qemu-embed: input: queue full (%u events), dropping\n", IN_QUEUE_LEN);
        }
    }
    qemu_mutex_unlock(&e->in_lock);
}

void qemu_embed_key(qemu_embed_t *e, uint32_t qcode, bool down)
{
    enqueue(e, (in_event){ .kind = EV_KEY, .down = down, .a = qcode });
}

uint32_t qemu_embed_atset1_to_qcode(uint32_t atset1)
{
    if (atset1 < qemu_input_map_atset1_to_qcode_len) {
        return qemu_input_map_atset1_to_qcode[atset1];
    }
    return 0;
}

void qemu_embed_mouse_rel(qemu_embed_t *e, int dx, int dy)
{
    enqueue(e, (in_event){ .kind = EV_REL, .a = dx, .b = dy });
}

void qemu_embed_mouse_abs(qemu_embed_t *e, int x, int y, int w, int h)
{
    enqueue(e, (in_event){ .kind = EV_ABS, .a = x, .b = y, .c = w, .d = h });
}

void qemu_embed_mouse_btn(qemu_embed_t *e, uint32_t button, bool down)
{
    if (button < ARRAY_SIZE(button_map)) {
        enqueue(e, (in_event){ .kind = EV_BTN, .down = down, .a = button });
    }
}

bool qemu_embed_mouse_is_absolute(qemu_embed_t *e)
{
    return qemu_input_is_absolute(e->con);
}

/*
 * The gamepad (M13 path A). One event carries the *whole* pad, packed
 * into the fields the queue already has: `a` the four axes, `b` the hat,
 * `c` the buttons.
 *
 * Whole state rather than per-control events, for the reason
 * usb-gamepad.h gives. A dropped update is corrected by the next one,
 * where a dropped event would leave the guest holding a button. It also
 * means the queue can never fill with pad traffic the way it could with
 * one event per axis per frame.
 */
void qemu_embed_pad_state(qemu_embed_t *e, const uint8_t *axes,
                          uint32_t hat, uint32_t buttons)
{
    int32_t packed = (int32_t)((uint32_t)axes[0] | ((uint32_t)axes[1] << 8) |
                               ((uint32_t)axes[2] << 16) | ((uint32_t)axes[3] << 24));
    enqueue(e, (in_event){ .kind = EV_PAD, .a = packed,
                           .b = (int32_t)hat, .c = (int32_t)buttons });
}

bool qemu_embed_pad_present(qemu_embed_t *e)
{
    (void)e;
#ifdef CONFIG_GAMEPORT
    if (gameport_present()) {
        return true;
    }
#endif
    return usb_gamepad_present();
}

/* Runs on the main loop under BQL. */
static void bh_input_drain(void *opaque)
{
    qemu_embed_t *e = opaque;
    in_event batch[IN_QUEUE_LEN];
    unsigned n;

    qemu_mutex_lock(&e->in_lock);
    n = e->in_n;
    memcpy(batch, e->in_q, n * sizeof(in_event));
    e->in_n = 0;
    e->in_bh_pending = false;
    qemu_mutex_unlock(&e->in_lock);

    /* statistics: latency of the oldest event, down+up pairs in one batch */
    in_stats *st = &e->in_st;
    int64_t now = g_get_monotonic_time();
    st->drains++;
    st->events += n;
    if (n && now - batch[0].t > st->max_lat) {
        st->max_lat = now - batch[0].t;
    }
    for (unsigned i = 0; i < n; i++) {
        if (batch[i].kind != EV_KEY || !batch[i].down) {
            continue;
        }
        for (unsigned j = i + 1; j < n; j++) {
            if (batch[j].kind == EV_KEY && batch[j].a == batch[i].a && !batch[j].down) {
                st->coalesced++;
                if (st->coalesced_logged < 8) {
                    st->coalesced_logged++;
                    fprintf(stderr, "qemu-embed: input: key qcode %d down and up in one drain "
                            "(down queued %lld ms ago, up %lld ms after it, batch of %u)\n",
                            batch[i].a, (long long)((now - batch[i].t) / 1000),
                            (long long)((batch[j].t - batch[i].t) / 1000), n);
                }
                break;
            }
        }
    }
    if (now - st->last_report > 5 * 1000000 && (st->max_lat > 20000 || st->coalesced || st->dropped)) {
        fprintf(stderr, "qemu-embed: input: %u drains, %u events, max latency %lld ms, "
                "%u zero-length key presses, %u dropped\n",
                st->drains, st->events, (long long)(st->max_lat / 1000), st->coalesced, st->dropped);
        st->last_report = now;
        st->max_lat = 0;
    }

    bool pointer = false;
    for (unsigned i = 0; i < n; i++) {
        in_event *ev = &batch[i];
        switch (ev->kind) {
        case EV_KEY:
            qemu_input_event_send_key_qcode(e->con, (QKeyCode)ev->a, ev->down);
            break;
        case EV_REL:
            qemu_input_queue_rel(e->con, INPUT_AXIS_X, ev->a);
            qemu_input_queue_rel(e->con, INPUT_AXIS_Y, ev->b);
            pointer = true;
            break;
        case EV_ABS:
            qemu_input_queue_abs(e->con, INPUT_AXIS_X, ev->a, 0, ev->c);
            qemu_input_queue_abs(e->con, INPUT_AXIS_Y, ev->b, 0, ev->d);
            pointer = true;
            break;
        case EV_BTN:
            qemu_input_queue_btn(e->con, button_map[ev->a], ev->down);
            pointer = true;
            break;
        case EV_PAD: {
            uint8_t axes[USB_GAMEPAD_AXES] = {
                (uint8_t)(ev->a & 0xff), (uint8_t)((ev->a >> 8) & 0xff),
                (uint8_t)((ev->a >> 16) & 0xff), (uint8_t)((ev->a >> 24) & 0xff),
            };
            /* No qemu_input_event_sync(): neither pad device is on a
             * console and they do not ride the input core at all.
             * Both are called unconditionally. A machine has one or
             * the other and the absent one returns immediately, which
             * is cheaper than asking twice and keeps this file out of
             * the business of knowing which path the bundle chose. */
            usb_gamepad_set_state(axes, (uint8_t)ev->b, (uint16_t)ev->c);
#ifdef CONFIG_GAMEPORT
            gameport_set_state(axes, (uint8_t)ev->b, (uint16_t)ev->c);
#endif
            break;
        }
        }
    }
    if (pointer) {
        qemu_input_event_sync();
    }
}

void qemu_embed_input_flush(qemu_embed_t *e)
{
    bool schedule;
    qemu_mutex_lock(&e->in_lock);
    schedule = !e->in_bh_pending && e->in_n > 0;
    if (schedule) {
        e->in_bh_pending = true;
    }
    qemu_mutex_unlock(&e->in_lock);
    if (schedule) {
        aio_bh_schedule_oneshot(qemu_get_aio_context(), bh_input_drain, e);
    }
}
