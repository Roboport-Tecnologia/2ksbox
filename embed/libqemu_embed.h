/*
 * libqemu_embed.h: minimal C API to run QEMU in-process.
 *
 * Thread contract:
 *   - qemu_embed_new(), qemu_embed_run(), qemu_embed_destroy(): one dedicated
 *     caller-created thread (the "QEMU thread"). new() returns with the VM
 *     created and paused (-S is forced); run() blocks until shutdown.
 *   - display callbacks fire on the QEMU thread with the BQL held. They must
 *     not block and must not retain the surface pointer past on_switch's
 *     replacement.
 *   - vm_* and input functions are safe from any thread (async).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef LIBQEMU_EMBED_H
#define LIBQEMU_EMBED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Exported regardless of any -fvisibility setting in the host build. */
#if defined(_WIN32)
#define QEMU_EMBED_API __declspec(dllexport)
#else
#define QEMU_EMBED_API __attribute__((visibility("default")))
#endif

typedef struct qemu_embed qemu_embed_t;

/* Pixel format of the surface handed to on_switch. v1 accepts only 32bpp
 * XRGB (little-endian 0x00RRGGBB per pixel); QEMU shadows other guest depths
 * into it for us. */
#define QEMU_EMBED_FMT_XRGB8888 1

typedef struct qemu_embed_display_cb {
    /* New surface: pixels valid until the next on_switch. stride in bytes. */
    void (*on_switch)(void *ud, const uint8_t *pixels, int width, int height,
                      int stride, uint32_t fmt);
    /* Rect (already clamped) changed in the current surface. */
    void (*on_update)(void *ud, int x, int y, int w, int h);
    /* One refresh tick finished (all updates of this tick delivered). */
    void (*on_refresh_done)(void *ud);
    /* Hardware cursor image (0xAARRGGBB per pixel, hot spot); NULL to clear. */
    void (*on_cursor)(void *ud, const uint32_t *argb, int w, int h,
                      int hot_x, int hot_y);
    /* Guest-driven cursor position / visibility. */
    void (*on_mouse_set)(void *ud, int x, int y, bool visible);
    /* v4: qemu-3dfx pass-through went on/off. While on, the VGA surface is
       not updated; frames arrive through on_3d_frame instead. */
    void (*on_3d_active)(void *ud, bool active);
    /* v4: a 3D frame was presented (vCPU thread, BQL held). XRGB8888,
       top-down, stride in bytes; valid only during the call. */
    void (*on_3d_frame)(void *ud, const uint8_t *pixels, int width, int height,
                        int stride);
    /* v5 (Linux): zero-copy. The backend renders each presented frame into
       one of a small ring of dma-buf backed buffers (DRM fourcc, linear
       modifier, top-down). This is called once per ring slot (and again
       when the slot is re-allocated at a new size) with a dup'ed fd the
       frontend owns. Return nonzero to accept; returning 0 (or leaving the
       callback NULL) keeps the readback path (on_3d_frame). vCPU thread. */
    int (*on_3d_dmabuf)(void *ud, int slot, int fd, int width, int height,
                        int stride, uint32_t fourcc, uint64_t modifier);
    /* v5: the frame in `slot` is complete (GPU work finished). */
    void (*on_3d_frame_ready)(void *ud, int slot);
    /* v6 (macOS): zero-copy ring slot backed by an IOSurface (BGRA8,
       top-down). `iosurface` is an IOSurfaceRef the backend keeps alive
       until the slot is re-offered or 3D ends; wrap it in a Metal texture.
       Return nonzero to accept; 0 keeps the readback path. vCPU thread. */
    int (*on_3d_iosurface)(void *ud, int slot, void *iosurface, int width, int height);
    /* v12: the device pushed its updates itself, outside a refresh tick
       (virtio-gpu's RESOURCE_FLUSH, the Voodoo 2's end of frame), and the
       on_update calls just made complete a frame: show it now rather than
       at the next tick. Main loop or vCPU thread, BQL held. */
    void (*on_flush)(void *ud);
} qemu_embed_display_cb;

/* Create + initialize QEMU. argv is a plain qemu-system command line
 * (argv[0] ignored); "-S" and "-display none" are appended. Fatal config
 * errors exit the process (QEMU semantics), so validate before calling.
 * Returns NULL only on argument errors. */
QEMU_EMBED_API qemu_embed_t *qemu_embed_new(int argc, char **argv,
                             const qemu_embed_display_cb *cb, void *ud);

/* Run the main loop. Returns QEMU's exit status when the guest shuts down
 * or qemu_embed_vm_shutdown() was requested. */
QEMU_EMBED_API int qemu_embed_run(qemu_embed_t *e);

/* Tear down. QEMU cleanup is incomplete upstream: one VM per process. */
QEMU_EMBED_API void qemu_embed_destroy(qemu_embed_t *e, int status);

/* --- VM control: async, any thread --- */
QEMU_EMBED_API void qemu_embed_vm_start(qemu_embed_t *e);      /* resume / first start */
QEMU_EMBED_API void qemu_embed_vm_pause(qemu_embed_t *e);
QEMU_EMBED_API void qemu_embed_vm_reset(qemu_embed_t *e);
QEMU_EMBED_API void qemu_embed_vm_powerdown(qemu_embed_t *e);  /* ACPI power button */
QEMU_EMBED_API void qemu_embed_vm_shutdown(qemu_embed_t *e);   /* hard stop of the loop */
QEMU_EMBED_API bool qemu_embed_vm_running(qemu_embed_t *e);

/* --- Input: enqueue from any thread, then flush once per batch --- */
/* qcode = QKeyCode (qapi enum value). See qemu_embed_atset1_to_qcode(). */
QEMU_EMBED_API void qemu_embed_key(qemu_embed_t *e, uint32_t qcode, bool down);
/* AT set-1 scancode (0xE0-prefixed codes as 0xE0xx) -> QKeyCode, 0 if none */
QEMU_EMBED_API uint32_t qemu_embed_atset1_to_qcode(uint32_t atset1);
QEMU_EMBED_API void qemu_embed_mouse_rel(qemu_embed_t *e, int dx, int dy);
/* x,y in [0,w) x [0,h): scaled to the guest's absolute range */
QEMU_EMBED_API void qemu_embed_mouse_abs(qemu_embed_t *e, int x, int y, int w, int h);
/* button: 0 left, 1 middle, 2 right, 3 wheel up, 4 wheel down, 5 side, 6 extra */
QEMU_EMBED_API void qemu_embed_mouse_btn(qemu_embed_t *e, uint32_t button, bool down);
/* Whether the active guest pointer device wants absolute coordinates. */
QEMU_EMBED_API bool qemu_embed_mouse_is_absolute(qemu_embed_t *e);
/* v8: the gamepad (M13 path A, `-device usb-gamepad`). One call carries
 * the *whole* pad, not one control. `axes` is four bytes (X, Y, Z, Rz,
 * two sticks, 0..255 with 0x80 centred), `hat` is 0..7 clockwise from
 * north or 8 for released, and `buttons` is a bitmap of twelve. Absolute
 * state rather than events, so a dropped update is corrected by the next
 * one instead of leaving the guest holding a button. Enqueued like the
 * rest and delivered by qemu_embed_input_flush(); a no-op on a machine
 * with neither pad device.
 *
 * The same call also feeds path B's `gameport` (0x201, `-device
 * gameport`), which takes the first two axes, the first four buttons and
 * the hat folded onto its own X/Y. The port has no room for the rest.
 * No version bump for that: one more consumer of the same bytes, and a
 * machine has one device or the other. */
QEMU_EMBED_API void qemu_embed_pad_state(qemu_embed_t *e, const uint8_t *axes,
                                         uint32_t hat, uint32_t buttons);
/* Whether this machine has either pad device for the above to reach. */
QEMU_EMBED_API bool qemu_embed_pad_present(qemu_embed_t *e);
/* Schedule delivery of everything enqueued (one sync). */
QEMU_EMBED_API void qemu_embed_input_flush(qemu_embed_t *e);

/* --- Audio: call BEFORE qemu_embed_new(); then pass
 *   -audiodev embed,id=snd0,out.frequency=48000,out.channels=2,out.format=f32
 * QEMU's mixer writes interleaved PCM in that format into the ring; the host
 * audio thread consumes it. `bytes` must be a power of two. wr_idx is
 * written by QEMU (release), rd_idx by the consumer (release); both are byte
 * positions in [0, bytes). One byte is kept free. */
QEMU_EMBED_API void qemu_embed_set_audio_ring(void *base, size_t bytes,
                               uint32_t *wr_idx, const uint32_t *rd_idx);

/* Display refresh pull interval in ms (QEMU default 30 → ~33 fps cadence).
 * The player sets ~16 for 60 Hz hosts. Any thread. */
QEMU_EMBED_API void qemu_embed_set_refresh_ms(qemu_embed_t *e, uint32_t ms);

/* v7: a socket the caller made, as the `fd=` of `-chardev socket,fd=N`
 * (doc 11 QMP). On Windows that N is a C-runtime descriptor, not a SOCKET:
 * QEMU resolves it with _get_osfhandle(), and the descriptor table belongs
 * to whichever CRT a module links, so the conversion has to happen on this
 * side of the library boundary, not in the caller. Pass the raw SOCKET;
 * the returned descriptor owns it. Elsewhere an fd is already an fd and the
 * value comes back unchanged. Returns -1 if it cannot be converted. */
QEMU_EMBED_API int qemu_embed_socket_to_fd(uint64_t sock);

/* v10: set an environment variable where this library and what it loads
 * read it. QEMU, our devices and the Direct3D executor read the environment
 * with their C runtime's getenv(), which on Windows answers from a copy
 * that runtime made when the process started: a variable the caller sets
 * later with SetEnvironmentVariable (all Rust's set_var does, on either
 * Windows target) never reaches it, and a caller on another C runtime has
 * no way into it. Sets the process's environment too. Call before
 * qemu_embed_new(), while no other thread exists. False on failure. */
QEMU_EMBED_API bool qemu_embed_setenv(const char *name, const char *value);

/* v9: the window's drawable size in pixels and its DPI, as a monitor the
 * guest's adapter can take the size of (virtio-gpu, M20: Windows 11 on Arm
 * with viogpudo). The console on show hears the first size at once, later
 * ones after a second without a newer one (a window being dragged), and a
 * console switched to hears the current one at once; an adapter with fixed
 * modes ignores it. What the guest does with it is its driver's: viogpudo
 * 0.1.302 on Arm64 takes it only when it starts. Any thread. */
QEMU_EMBED_API void qemu_embed_set_window_size(qemu_embed_t *e, uint32_t w,
                                               uint32_t h, uint32_t dpi);
/* v15: the guest screen's refresh rate in mHz (track M24), as the
 * monitor's own: it rides with v9's window size in the console's UI info,
 * which virtio-gpu writes into the EDID's preferred timing (75 Hz when
 * none is given) and announces with a display event. Our viogpudo paces
 * its vertical blank at it. 0 forgets it. Any thread. */
QEMU_EMBED_API void qemu_embed_set_refresh_rate(qemu_embed_t *e, uint32_t mhz);
/* v16: a vertical blank of the host screen's that starts a guest frame
 * (every Nth for a divided rate, track M24 step 4 B): QEMU's
 * qemu_host_vblank() on the main loop, which a virtio-gpu with
 * host-vblank=on (QEMU patch 90) passes to a guest that asked for it. Any
 * thread, cheap: nothing listens, nothing happens. */
QEMU_EMBED_API void qemu_embed_vblank(qemu_embed_t *e);
/* v9: whether the console on show takes the window's size, so the window
 * should not be held to the guest's mode. Any thread. */
QEMU_EMBED_API bool qemu_embed_display_follows_window(qemu_embed_t *e);

/* v11: the clipboard (track M23), through QEMU's own clipboard, which
 * `-chardev qemu-vdagent,clipboard=on` connects to a guest agent (the
 * SPICE agent protocol over a virtio-serial port). Text only, UTF-8.
 *
 * The callback hears the guest's text whenever the guest's clipboard
 * changes: QEMU thread, BQL held, `utf8` valid only during the call (copy
 * it, do not block). Call after qemu_embed_new(); the library joins
 * QEMU's clipboard then. */
typedef void (*qemu_embed_clipboard_cb)(void *ud, const char *utf8, size_t len);
QEMU_EMBED_API void qemu_embed_set_clipboard_cb(qemu_embed_t *e, qemu_embed_clipboard_cb fn,
                                                void *ud);
/* The host's clipboard text, offered to the guest (copied). Any thread,
 * after qemu_embed_new(). Text the guest has not asked for yet is simply
 * replaced by the next call. */
QEMU_EMBED_API void qemu_embed_clipboard_set_text(qemu_embed_t *e, const char *utf8,
                                                  size_t len);

/* Library version of the embed API, for the bindings to sanity-check. */
QEMU_EMBED_API uint32_t qemu_embed_api_version(void);
#define QEMU_EMBED_API_VERSION 16

#ifdef __cplusplus
}
#endif
#endif /* LIBQEMU_EMBED_H */
