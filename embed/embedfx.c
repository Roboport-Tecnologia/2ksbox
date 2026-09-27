/*
 * embedfx.c: qemu-3dfx UI provider for the embed library (patch 30's
 * QemuFxUiOps). No window: the backend's pbuffer is the drawable, so the
 * Mesa entry points just acknowledge, report activation to the frontend
 * and answer size queries. The table's glide_* entries are left to
 * patch 30's defaults: the Glide device is not built (patch 74, ADR-020).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "ui/console.h"
#include "embedfx.h"

static int fx_active;

/* One "the frontend is showing 3D" edge: while it is up the VGA device is
 * not rendered and frames come from swaps. */
static void fx_set_active(int on)
{
    if (on == fx_active) {
        return;
    }
    fx_active = on;
    graphic_hw_passthrough(qemu_console_lookup_by_index(0), on);
    embed_fx_active(on != 0);
}

static void fx_mesa_prepare_window(int msaa, int alpha, int scale_x, void *cwnd_fn)
{
    void (*cwnd)(void *, void *, void *) = cwnd_fn;
    if (embed_gl_available()) {
        /* non-NULL "native window" = drawable available */
        cwnd(NULL, (void *)(uintptr_t)1, NULL);
    } else {
        static bool warned;
        if (!warned) {
            warned = true;
            error_report("mesapt: no EGL on this host; refusing GL context");
        }
        cwnd(NULL, NULL, NULL);
    }
}

static void fx_mesa_release_window(void)
{
    fx_set_active(0);
}

static void fx_mesa_renderer_stat(const int activate)
{
    fx_set_active(activate);
}

static int fx_mesa_gui_fullscreen(int *sizev)
{
    if (sizev) {
        QemuConsole *con = qemu_console_lookup_by_index(0);
        DisplaySurface *s = con ? qemu_console_surface(con) : NULL;
        sizev[0] = s ? surface_width(s) : 640;
        sizev[1] = s ? surface_height(s) : 480;
        /* drawable == guest surface: the in-QEMU scaler stays inert */
        embed_gl_drawable_size(&sizev[2], &sizev[3]);
    }
    return 0;
}

static const QemuFxUiOps embed_fx_ui_ops = {
    .mesa_renderer_stat   = fx_mesa_renderer_stat,
    .mesa_prepare_window  = fx_mesa_prepare_window,
    .mesa_release_window  = fx_mesa_release_window,
    .mesa_cursor_define   = fx_ui_default_cursor_define,
    .mesa_mouse_warp      = fx_ui_default_mouse_warp,
    .mesa_gui_fullscreen  = fx_mesa_gui_fullscreen,
};

void embed_fx_register(void)
{
    qemu_fx_ui_register(&embed_fx_ui_ops);
}
