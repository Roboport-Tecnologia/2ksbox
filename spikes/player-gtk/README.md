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

- **offload** (`SPIKE_MODE=offload`): the CRT chain renders into a ring of linear
  dma-buf images (`gpu.rs`), each handed to GTK as a `GdkDmabufTexture` on
  a `GtkPicture` inside a `GtkGraphicsOffload`. GTK attaches the buffer to
  a subsurface of its own (`GDK_DEBUG=offload` shows it), but only on its
  next frame clock paint.
- **subsurface** (the default): a desync `wl_subsurface` of our
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
PLAYER_LATENCY=1 cargo run --release                          # subsurface
SPIKE_MODE=offload PLAYER_LATENCY=1 cargo run --release       # offload
# a live guest: the launcher's arguments, on an overlay
cargo run --release -- --shader <preset> -- $(launcherx --print-args <machine.toml>)
```

It takes the player's command line (`--shader`, `--shader-params`,
`--pad`, `--` and QEMU's), and points QEMU at this checkout's SoundFont
and Direct3D executor the way the player finds them, so the launcher can
start it in the player's place (the pad is not read):

```sh
LAUNCHER_PLAYER_BIN=$PWD/target/release/player-gtk-spike ../../launcher-qt/target/release/launcher-qt
```

A live guest needs QEMU on a GLib of its own (the last section says why),
which is the Linux build's default since 2026-09-27 (`scripts/build.sh`;
`QEMU_DEPS` in `docs/development.md`). A QEMU built with
`QEMU_DEPS=system` aborts as soon as the guest starts.

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

A live guest (`base98-br` on an overlay, `crt-aperture`, headless sway)
boots to the desktop in both modes once QEMU has a GLib of its own
(below). Publish→presented p50 (p95) over 150 s: subsurface 5.9–7.9 ms
(16.2), offload 34.6–40.4 ms (48.1).

With the system GLib, the live guest aborts as soon as QEMU runs:
QEMU's main loop, on its own thread, iterates GLib's global default
`GMainContext` (`util/main-loop.c`), the one GTK's main loop runs on, and
dispatches GTK's sources (here the spike's wake, a thread-local source).
`libqemu-embed` links the system GLib on Linux, so the two share it. winit
has no GLib and never met this; Qt's GLib event dispatcher would (unless
`QT_NO_GLIB=1`). The fix taken
is a private static GLib inside `libqemu-embed` (`scripts/build-deps.sh`
on Linux, linked by `scripts/configure-qemu.sh`): GLib, pcre2 and
libslirp static, their symbols hidden with `--exclude-libs`, smartcard
off (libcacard links the system GLib). The library then lists no GLib,
gio, gobject, slirp or pcre2 in `ldd` and has no `g_*` symbol, defined or
undefined, in its dynamic table.
