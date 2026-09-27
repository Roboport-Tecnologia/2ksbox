# The player's picture in a mitsuami window (GTK 4 spike)

Throwaway. It asks whether the player can move from winit to mitsuami
(`~/work/mitsuami`, next to this checkout) on Linux without giving up
latency. Everything here is a spike: no track doc, nothing packaged, the
player itself untouched. It borrows the player's `qemu_vm.rs`, `audio.rs`,
`qmp.rs`, `dmabuf.rs`, `pattern.rs` and `blit.wgsl` by `#[path]`.

## What runs

`player-gtk-spike` is a mitsuami window: a menu (Machine: Ctrl+Alt+Del,
Reset, Power Button, Close), the picture as a `NativeView`, and a status
line. The picture goes to the screen one of two ways:

- **offload** (default): the CRT chain renders into a ring of linear
  dma-buf images (`gpu.rs`), each handed to GTK as a `GdkDmabufTexture` on
  a `GtkPicture` inside a `GtkGraphicsOffload`. GTK attaches the buffer to
  a subsurface of its own (`GDK_DEBUG=offload` shows it), but only on its
  next frame clock paint.
- **subsurface** (`SPIKE_MODE=subsurface`): a desync `wl_subsurface` of our
  own over the widget (`subsurface.rs`), with an empty input region so GTK
  keeps the pointer, and a wgpu surface on it in Mailbox, as the player's
  window has. GTK only keeps the space.

`baseline` is the player's own path for comparison: winit, a wgpu surface
in Mailbox with a frame latency of 1, rendering on the wake.

Both binaries publish the player's test pattern from a thread
(`PATTERN_HZ`, default 60) and measure publish→presented from the
compositor's presentation-time feedback (GTK's frame timings in offload
mode, `pres.rs` elsewhere). `PLAYER_LATENCY=1` prints it every 240 frames.

```sh
cargo run --release --bin baseline
PLAYER_LATENCY=1 cargo run --release                          # offload
SPIKE_MODE=subsurface PLAYER_LATENCY=1 cargo run --release    # subsurface
# a live guest: the launcher's arguments, on an overlay
cargo run --release -- --shader <preset> -- $(launcherx --print-args <machine.toml>)
```

`SPIKE_MUTE=1` gives the guest a silent audio device; `SPIKE_INHIBIT=1`
takes the compositor's shortcuts while the window has focus (GTK's own
`gdk_toplevel_inhibit_system_shortcuts`). Keyboard and the absolute mouse
reach the guest; the relative mouse (pointer lock) does not yet.

To measure without a window on your desktop, run a private headless sway
(`WLR_BACKENDS=headless sway -c <config with one 60 Hz output>`) and point
the binaries at its socket with `WAYLAND_DISPLAY`. It paces with a timer
and presents on commit, so its absolute numbers are lower than a real
display's; it's an A/B bench.

## Results (2026-09-27, RX 9060 XT / RADV, sway 1.12, GTK 4.22)

Test pattern, publish→presented p50 (p95):

| | real display, 60 Hz, 60 Hz pattern | headless sway, 57 Hz | headless sway, 60 Hz |
|---|---|---|---|
| `baseline` (winit, the player's path) | 32.8 ms (32.8) | 0.4 (0.8) | 0.7 (0.8) |
| offload (`GtkGraphicsOffload`) | 47.6 ms (48.0) | 17.7 (18.4) | 17.7 (28.8) |
| subsurface (our own, desync) | not run | 0.4 (0.9) | 0.5 (11.5) |

- Offload works as offload: GTK attaches each frame to a subsurface and
  composites nothing. Its cost is the wait for GTK's frame clock, which
  paints, and commits, once per frame callback. On the real display that
  was 14.5 ms of a 60 Hz pattern phase-locked to the display.
- The desync subsurface matches the winit path at the median. Its p95 at
  60 Hz is worse; the cause is not known yet.
- The dma-buf export is linear (`DRM_FORMAT_MOD_LINEAR`), 7680-byte
  stride at 1920 px, and the CPU waits for the GPU (under 1.2 ms) before
  GTK gets the buffer: no fence is passed yet.

A live guest (`base98-br` on an overlay) aborts as soon as QEMU runs:
QEMU's main loop, on its own thread, iterates GLib's global default
`GMainContext` (`util/main-loop.c`), the one GTK's main loop runs on, and
dispatches GTK's sources (here the spike's wake, a thread-local source).
`libqemu-embed` links the system GLib on Linux, so the two share it. winit
has no GLib and never met this; Qt's GLib event dispatcher would (unless
`QT_NO_GLIB=1`). The fix is either QEMU on a private `GMainContext`
(`main-loop.c`'s four uses, its two `g_source_attach(…, NULL)`, and the
chardev/IO watches that pass a `NULL` context) or a private static GLib
inside `libqemu-embed` on Linux, as the macOS build already links.
