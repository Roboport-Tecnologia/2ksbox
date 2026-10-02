# Track M22: the player on mitsuami

Opened 2026-10-01 (user: "now lets make the mitsuami player"), merged to
`main` the same day (user), and worked on there. ADR-025:
the player gets a mitsuami front end beside the winit one, as the
launcher did (ADR-023, M19), and replaces it once it runs everywhere.
User decisions at the start: a `player-core` library with two thin front
ends (not a copy, not a rewrite of `player/` in place); GTK on Wayland
first, winit keeps every other host until then; the platform's menu bar
over a window that is otherwise only the picture.

## Scope and files

- `player-core/` (root workspace): everything but the window. The QEMU
  thread and embed glue (`qemu_vm.rs`, `qmp.rs`), the picture (`gpu.rs`:
  the guest frame, zero-copy slots, the CRT chain, the geometry stage,
  shots; any wgpu surface target), what one run shows and its wake, draw
  and present steps (`session.rs`), the mode sweep and calibration
  (`sweep.rs`), what the guest holds (`input.rs`: keys, Ctrl+Alt+Del, the
  gamepad), the key table by W3C code (`keys.rs`), audio, companions,
  mode analysis, the command line (`startup`), and the close question's
  sentences. Shared with `player/` (M2's and M13's files moved here; their
  owners still own them).
- `player-mitsuami/` (its own cargo workspace, like `launcher-mitsuami`,
  so the root build never needs GTK): the window, its menus, the surface's
  input. `tools/player-mitsuami-test.sh` and its `scripts/test.sh` check.
- `player/` keeps only winit: the window, `kbcapture.rs`, `keymap.rs`'s
  keymap reading, the drawn close prompt.

## Building

```sh
cargo build --release -p player                    # the winit player, as before
cd player-mitsuami && cargo build --release        # GTK 4 (4.10+)
cargo build --release --no-default-features --features kde,gilrs   # Kirigami (not run yet)
```

mitsuami comes from the same pinned `rev` as `launcher-mitsuami`; bump
both together. `--features qemu-x86_64` (or `qemu-aarch64`) links the other
embed library, as for the winit player, into a target dir of its own.

## Test loop

- `tools/player-mitsuami-test.sh [out]` (in `scripts/test.sh` as
  `player-mitsuami`, skipped unless the binary is built and sway, grim and
  wtype are installed): a private headless sway, then the mode sweep
  through this player, the test pattern read back off the compositor, and
  a virtual keyboard's Windows key reaching the surface.
- A guest, headless: a scratch bundle on a qcow2 overlay of a machine's
  disk, `launcherx --print-player-args` and `--print-args` on it, the
  player on a headless sway (`WLR_BACKENDS=headless
  WLR_LIBINPUT_NO_DEVICES=1 sway -c <one 60 Hz output>`, then
  `WAYLAND_DISPLAY=<its socket> GDK_BACKEND=wayland`), shots with
  `PLAYER_SHOT_EVERY` and `grim`, keys with `wtype -P Super_L -p Super_L`
  (wtype's own keymap puts letters on other keys; the keys mitsuami reads
  by keysym, the modifiers, Escape, arrows, come out right; the first key
  of a new virtual keyboard is lost, so send a Shift first). The headless
  seat has **no pointer**: clicks and the lock need a real desktop.
- `PLAYER_INPUT_LOG=1` prints every `SurfaceInput` with the lock and grab
  state; `PLAYER_SURFACE_LOG=1` every size the surface reports. Every other
  `PLAYER_*` knob is the core's and works as in the winit player.
- The launcher starts it in the winit player's place with
  `LAUNCHER_PLAYER_BIN=<…>/player-mitsuami/target/release/player-mitsuami`.

## Steps

1. **`player-core`, and the player on GTK / Wayland (done 2026-10-01).**
   The winit player's `main.rs` split: the toolkit-free half into
   `player-core` (its `Gpu` no longer holds a window: it takes any surface
   target, reports the minimum window size the mode wants, `take_min_size`,
   and leaves the present to the front end); `player/main.rs` is the winit
   half, its behaviour unchanged (the host suite passes as at its
   baseline; an XP overlay boots to the desktop with the same mode, cursor
   and shots as before the split). `player-mitsuami`: an `App::open`
   `Window` (the title notes the lock and the shortcuts as winit's does,
   full screen, a minimum size from `take_min_size`, capped at the screen
   by mitsuami), a `GpuSurface` presented to in Mailbox from the UI thread
   on each QEMU wake (`wake.rs`, a future the QEMU thread's waker
   resolves through mitsuami's executor), and the menus: Machine (Send
   Ctrl+Alt+Del, Pause, Reset, Power Button, Close as the platform's
   Quit) and View (Full Screen, Release Mouse, Send Shortcuts to Guest,
   Save Screenshot, Save Screenshot as Shown), with the winit player's
   chords as their shortcuts, answered on the surface too while the
   keyboard is grabbed. What winit's player did by hand is mitsuami's:
   the keyboard grab (Ctrl+Alt+K turns the wish off; the platform ends the
   grab on every focus loss and the next key or click takes it again), the
   pointer lock with raw motion for a PS/2 guest (`Motion` only where no
   raw counts come), the guest's cursor as the surface's cursor image, keys
   a keymap moved read by keysym, keys let go on focus loss. A close with
   Alt held (or the menu's Close) asks in the platform's alert, Cancel
   first; the title bar's button does not ask. Checked headless: the mode
   sweep (19 modes), the pattern, XP (`basexp-br` on an overlay, KVM) to
   the desktop through CRT Aperture with the d3dpt-vga driver and the
   guest cursor, the Windows key opening XP's Start menu and the arrows
   moving in it. Not checked: anything with the pointer (no pointer on the
   headless seat), the menus and the alert by hand, the grab under a real
   compositor, latency against the winit player on a real display.
2. **The pointer and the window on a real desktop.** The user ran it by
   hand on 2026-10-01 ("seems like it's working"); what it covered is
   not recorded yet. Clicks, the lock and
   raw motion on a PS/2 guest, the tablet with the guest's cursor image,
   the menus, the close alert, Ctrl+Alt+K, full screen; then
   publish→presented against the winit player (the spike's numbers below
   are the bar). **A mitsuami bug to fix there first:** on sway with the
   window *tiled*, the surface sits 5 px right of and below its area
   (the picture's centre 5 px off, a strip of window background at the
   top and left); floating, it lands exactly. GTK's surface transform in
   the tiled state, `mitsuami-gtk/src/surface.rs::place`.
3. **X11, and Kirigami.** mitsuami's X11 child window: not run yet.
   **Kirigami (2026-10-01):** the KDE build runs the picture where the
   GTK one does (and without GTK's tiled offset). Its menus were
   Kirigami's menu button, a menu drawn in the window, so under the
   surface (the user's report). Opening that menu as a window of its own
   (mitsuami ac5dac9) worked about half the time: Kirigami makes the
   menu's items as it opens, and the popup window kept the empty menu's
   9 px height. On the user's suggestion a window with a GPU surface now
   shows a classic menu bar instead (mitsuami 1cf8624, 2c3f5e5), its menus
   windows of their own with their items from the start: 24 opens in 24
   over the surface, and Ctrl+Q runs the player's Close as on GTK
   (Kirigami's own Quit had taken it). Checked under a headless sway with
   a virtual pointer, `zwlr_virtual_pointer_v1` from a scratch client
   (`wayland-protocols-wlr`), which also gives step 2 a pointer to test
   with. Build: `cargo build --release --no-default-features --features
   kde,gilrs --target-dir target/kde` (its own target dir, or it replaces
   the GTK binary).
4. **macOS and Windows.** The keyboard grab there is weaker than the winit
   player's capture (doc 03 "Input path"): mitsuami's WinUI grab is a
   `WH_KEYBOARD_LL` hook, which doc 03 measured going blind while the
   player's own window had focus (raw input with `RIDEV_NOHOTKEYS` is the
   player's way), and AppKit's leaves Mission Control and the Spaces
   arrows to the system (the player turns the window server's hot keys
   off). Either mitsuami takes the player's ways, or `kbcapture`'s halves
   move into `player-core` on the surface's raw handle. Windows also needs
   the native MSVC build mitsuami requires (ADR-023's cost).
5. **What a player window can show now that it has a toolkit:** the disc
   shelf for the drive and the shader profile, from `launcher-core`
   (the player then needs the bundle it was started for).
6. **The launcher's player, packaging, the flip:** a build stage, the
   packagers' run, `player-mitsuami` shipped as `2ksbox-player` (and the
   per-target players), and `player/`'s winit front end deleted.

## The spike it came from (2026-09-27, RX 9060 XT / RADV, sway 1.12, GTK 4.22)

`spikes/player-gtk` (deleted with this track's first commit; the code is
in git history before it) asked whether the player could leave winit for
mitsuami on Linux without giving up latency. The test pattern,
publish→presented p50 (p95):

| | real display, 60 Hz, 60 Hz pattern | headless sway, 57 Hz | headless sway, 60 Hz |
|---|---|---|---|
| winit, the player's path | 32.8 ms (32.8) | 0.4 (0.8) | 0.7 (0.8) |
| `GtkGraphicsOffload` fed dma-bufs | 47.6 ms (48.0) | 17.7 (18.4) | 17.7 (28.8) |
| a desync subsurface of our own | not run | 0.4 (0.9) | 0.5 (11.5) |

- Offload works as offload (GTK attaches each frame to a subsurface and
  composites nothing), but waits for GTK's frame clock, which paints and
  commits once per frame callback: about a refresh more.
- The desync subsurface, placed over the widget, sized with `wp_viewport`
  and with an empty input region so GTK keeps the pointer, matched winit
  at the median; its p95 at 60 Hz was worse, cause not found. That route
  is what mitsuami's `GpuSurface` became (its ARCHITECTURE.md §13.26).
- A live guest (`base98-br`, CRT Aperture, headless sway) over 150 s:
  subsurface 5.9–7.9 ms p50 (16.2 p95), offload 34.6–40.4 ms (48.1).

### Why QEMU links a GLib of its own

With the system GLib the live guest aborted as soon as QEMU ran. QEMU's
main loop, on its own thread, iterates GLib's global default
`GMainContext` (`util/main-loop.c`), the one GTK's main loop runs on, and
dispatched GTK's sources there. `libqemu-embed` linked the system GLib on
Linux, so the two shared it. winit has no GLib and never met this; Qt's
GLib event dispatcher would (unless `QT_NO_GLIB=1`). The fix is a private
static GLib inside `libqemu-embed` (`scripts/build-deps.sh` on Linux,
linked by `scripts/configure-qemu.sh`, the Linux default since
2026-09-27): GLib, pcre2 and libslirp static with their symbols hidden by
`--exclude-libs`, smartcard off (libcacard links the system GLib). The
library then lists no GLib, gio, gobject, slirp or pcre2 in `ldd` and has
no `g_*` symbol in its dynamic table. A QEMU built with `QEMU_DEPS=system`
aborts under the mitsuami player as soon as the guest starts.
