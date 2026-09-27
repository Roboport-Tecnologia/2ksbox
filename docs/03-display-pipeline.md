# 3. Display pipeline: pixel accuracy, CRT shaders, latency, input

The player shades and shows exactly what the guest's video card outputs,
at native guest resolution and correct geometry, with a CRT look close
enough that a 1998 screenshot is hard to tell from a photo of the tube.
We own this pipeline end to end (ADR-005): a winit window, wgpu and
librashader. This doc covers the stages, the pixel rules, mode analysis
and geometry, latency and input. How 3D frames reach the player is doc
12, the reference CRT and its photo protocol doc 09, shader profiles doc
07, and the player's flags and env knobs `docs/development.md`.

## Stages

```
guest VGA/SVGA device (or a 3D frame: doc 12, doc 14)
  → QEMU DisplaySurface (raw framebuffer + dirty rects, in-process)
  → [render thread] texture upload (dirty-rect aware)
  → mode analysis (display aspect, scanline count)       player/src/mode.rs
  → librashader filter chain (slang preset)              shader-chain/src/lib.rs
  → geometry stage (aspect, integer scaling)
  → wgpu present (Metal / Vulkan / D3D12)
```

The chain follows RetroArch shader semantics (original-resolution input,
final viewport params, per-pass scaling), so the libretro slang presets
in `third_party/slang-shaders` (`crt-guest-advanced`, `crt-lottes`,
`crt-royale`, …) and any other `.slangp` work unmodified. Our own presets
live in `shaders/`. The one calibrated against the reference CRT is the
shadow-mask preset `shaders/syncmaster-753dfx.slangp`: the rig's Samsung
SyncMaster 753DFX has a delta dot trio at ≈0.20 mm (doc 09), not an
aperture grille. It is derived from the tube's geometry and waits on doc
09's photo pass. A Trinitron preset is still worth having as a style.

Open (M2): an answer for presets with no scanline-count parameter
(below).

## Pixel accuracy rules (all testable)

1. **Never scale before the shader.** The shader input is the exact guest
   framebuffer (640×480, 800×600, 320×200…), never a pre-stretched
   surface. QEMU's VGA breaks this for mode 13h (below).
2. **Non-square pixels.** 320×200 is a 4:3 picture with 1:1.2 pixels. The
   geometry stage applies the mode's display aspect, never
   width/height. Same for 640×400, 720×400 text (9-dot characters),
   360×240 and friends.
3. **Double-scan awareness.** Real VGA double-scans low-res modes (320×200
   is scanned as 400 lines). Scanline shaders need the scanline count,
   not the framebuffer height; mode analysis passes it through the
   preset's parameters.
4. **Integer scaling.** The picture's height is a whole multiple of its
   scanlines; the width follows from the aspect. Overscan crop is
   optional, off by default.
5. **Mode-change fidelity.** Mode analysis re-runs on every surface
   change; no garbage frames or stretched leftovers during a transition.
6. **Colour fidelity.** The guest device expands palettized modes; the
   surface is sRGB end to end (XRGB8888 uploads as BGRA8, no swizzle).

## Mode analysis

`player/src/mode.rs` turns a framebuffer size into what it meant on a
monitor of the era: the display aspect, the number of lines the CRT
scanned, and whether the CRTC double-scanned. It prints one line per mode
change:

```
[display] mode 320x200 VGA 320x200 (mode 13h) — 4:3 picture, pixel aspect 0.833, 400 scanlines (double-scanned)
[shader] mode parameters vga_mode=1 inter=800
```

The table is an exception list: an unlisted size has square pixels,
right for every SVGA and modern widescreen mode. The entries that matter
are the VGA's 200-, 240-, 350- and 400-line modes; the rest carry names,
feed the sweep, and hold any correction measured against the rig.

Two things are rules, not entries:

- **Double-scanning.** The CRTC sets its bit below ~300 lines.
- **The VGA raster** (`vga_raster`). A size at one of the four VGA widths
  (320, 360, 640, 720) and at most 480 lines is a 4:3 picture whatever its
  line count. QEMU's text path reports `rows × cheight` with the last
  partial row dropped, so a 400-line raster with a 12-line cell (XP's
  text-mode setup) arrives as **720×396**. Without the rule it would be
  an unlisted 1.818:1 picture, the XP installer stretched across the
  window.

### Geometry

The viewport is the largest rect of the mode's display aspect that fits,
with its height an integer multiple of the mode's **scanlines**, not its
rows (320×200 in a 2400-line surface is 6 pixels per scanline).

The rect is rounded to **whole pixels**, size and origin. Aspect
correction makes widths fractional (320×200 at 1x is 533.33 wide), and a
fractional viewport samples on a grid that moves with the window, so the
picture crawls during a drag. Rounding costs at most half a pixel of
aspect, far inside the sweep's 0.5 %.

Below 1x there is no whole scale, so the window's minimum inner size is
the 1x picture in physical pixels (320×200 → 534×400), re-applied on
every mode change and clamped to the monitor.

### The geometry stage's one moment

All of the above is decided when an input changes and only read while a
frame is drawn. `Gpu::guest_surface_changed` (new picture size) redoes
mode analysis, the minimum size, the preset's scanline count and the
fit; `Gpu::resize` (new host surface) redoes only the fit. Both write one held rect (`Gpu::geom`);
`viewport()` reads it and the draw derives nothing.

The trigger is the surface's own texture, not QEMU's `on_switch`. QEMU
announces a switch up to one refresh tick before the first frame of the
new mode exists, and re-fitting there draws the old pixels in the new box
for a tick, which rule 5 forbids. Exactly three things (re)create the
texture: the framebuffer's upload (`ensure_texture`), a 3D slot taken or
dropped (`use_slot`), and a slot re-imported at another size
(`import_slot`). So analysis, fit, the chain's output size and the
preset's parameters move with the pixels they belong to.

### Scanline count

A preset derives its scanline count from the input height or guesses
from a threshold, and both are wrong here. Mode analysis sets
crt-guest-advanced's own parameters. `vga_mode` ("VGA Single/Double Scan
mode") switches it from its console interlace guess to the two VGA cases,
and `inter` picks between them (double-scan when `inter` is above the
source height; its default of 375 is a guess).

Measured through the real chain (`--mode-sweep`, scanlines counted in the
drawn frame; `PLAYER_MODE_PARAMS=0` is the control):

| Mode | Tube | Mode analysis | Control |
|---|---|---|---|
| 320×200 | 400 | **400** | 200 |
| 320×240 | 480 | **480** | 240 |
| 640×200 | 400 | **400** | 200 |
| 640×350 | 350 | **350** | 350 |
| 640×400 | 400 | **400** | 200 |
| 720×400 | 400 | **400** | 200 |
| 640×480 | 480 | **480** | 240 |
| 800×600 | 600 | **600** | 300 |
| 1024×768 | 768 | **768** | 384 |

Below 375 lines the control draws one scanline per guest row; above, it
treats the source as interlaced and draws one field's worth. Only
640×350 comes out right, by accident.

**QEMU has usually double-scanned the mode before we see it.** On
`-vga cirrus` a mode 13h screen arrives as a **640×400 surface** (QEMU's
VGA doubles both axes; measured with `TEXTCAL.COM`, doc 09). Scanlines
and aspect come out the same, but the shader sees 640 columns where the
tube had 320, so `h_sharp` and the mask act on half-width pixels. Don't
undo it (detect the doubling or patch QEMU's VGA) without a photograph
of the real tube showing it matters. The double-scan branch still serves
a genuinely short surface (`d3dpt-vga` at a 200- or 240-line mode, a 3D
path). **720×400 text is not doubled.**

**Presets without the parameters.** crt-lottes and crt-royale derive
their scanline period from `OriginalSize` and expose no override, and
rule 1 forbids handing them a pre-doubled surface. The player says so
once and leaves them at their defaults:

```
[shader] this preset exposes no scanline-count parameter (vga_mode, inter): a double-scanned
mode will be drawn with one scanline per guest row instead of the two per row the tube drew
```

Per-mode `.slangp` variants, or a pack limited to presets that take the
parameters, stay open with the pack.

### Screenshots

**Ctrl+Alt+S** shoots the guest's frame: the texture QEMU published, at
the mode's own size, before the geometry stage and the chain. That is the
picture to compare with a golden BMP, a native run or another emulator.
An imported 3D slot is shot the same way. Files land in `PLAYER_SHOT_DIR`
(default: the working directory) as `2ksbox-NNNN.png`.
**Ctrl+Alt+Shift+S** shoots what the window shows instead: the chain's
last output drawn again, with the window's own blit, into a texture of
the swapchain's size and format, black bars included and the close
prompt left out. Drawing it again rather than running the chain again
keeps an animated preset from being stepped by a shot. `PLAYER_DUMP_OUT`
writes the chain's output alone, at the viewport's size.

### Sampling outside the picture

A curved preset samples outside the frame's edges, and what comes back
must be **transparent black**, the slang default `clamp_to_border`.
librashader's wgpu runtime silently downgrades such samplers to
`clamp_to_edge` on a device opened without
`ADDRESS_MODE_CLAMP_TO_BORDER`, smearing the outermost row and column
over everything outside the tube. So the chain's device is opened with
that feature (`shader_chain::required_features`, used by the player and
the launcher preview), and the player logs `[shader] clamp-to-border
sampling: …` at startup.

## The mode sweep

`player --mode-sweep <dir>` (the `mode-sweep` check, ~2 s, no guest, no
QEMU) puts a geometry test image through the real chain at every mode in
the table plus one unlisted size: a circle drawn in display space (round
only when the aspect is applied), one-pixel lines and SMPTE bars. It
checks aspect, fit, whole-pixel scanline pitch, the parameters reaching
the preset and the scanline count, and writes a PNG per mode to `<dir>`.
Two details make it mean something:

- It renders to a **fixed 3200×2400 surface** and never acquires a
  swapchain image. The result must not depend on the compositor, and an
  occluded window (a test behind a terminal) blocks the second acquire
  forever under FIFO.
- The scanline count comes from the **pitch between the first and last
  band**, since the pitch need not be a whole number of pixels. Below
  three output pixels per scanline the preset draws a flat field, so the
  count is not checked there (1152×864 and up, square-pixel modes whose
  count is their height).

## Latency budget

Target: **≤ 1 host frame added** between guest frame completion and
photons at 60 Hz, measured.

- **In-process handoff.** The display listener publishes surface + dirty
  rects; the render thread uploads immediately. No encode, no copy chain.
- **Present mode.** Mailbox where available, else FIFO;
  `desired_maximum_frame_latency = 1`.
- **Refresh mismatch.** Guests run 70 Hz (VGA text/DOS) and 60/75/85 Hz
  SVGA on 60/120/144 Hz/ProMotion hosts. The newest complete guest frame
  is presented at host vsync, never resampled. VRR passthrough is for
  later.
- **Acquire before sampling.** The QEMU refresh tick wakes the event loop
  (`EventLoopProxy`); the redraw first acquires the swapchain image, then
  samples the newest guest frame, uploads and presents. Sampling first
  aged every frame by a vblank whenever the queue was saturated, which on
  Metal was always, since the 16 ms tick is slightly faster than 60 Hz
  (≈32 ms publish→present on the M1 Air, 2.5 ms after).
- **Occlusion.** An occluded or minimized window may get no presents
  (Wayland frame callbacks, macOS occlusion); the loop skips on
  `Occluded`/`Timeout` and never lets QEMU stall behind it. Per-frame work
  that must not stall (importing zero-copy slots) runs on the wake event.
- **Measurement.** `PLAYER_LATENCY=1` reports publish→present-return;
  with vsync its floor at 60 Hz is uniform 0–16.7 ms (p50 ≈ 8). Guest
  draw→publish (0–`PLAYER_REFRESH_MS`) comes before that window.
- **Quiet unless something is off** (user request). Unasked, the player
  prints each mode change and anything that went wrong. The
  per-100-frames counter is `PLAYER_REFRESH_LOG=1`, beside
  `PLAYER_LATENCY` and `PLAYER_CURSOR_LOG`.

## Input path

winit events go to QEMU's input injection directly on the event thread.

**Pointer.** Absolute (USB tablet) for the desktop, relative (PS/2 plus a
host cursor grab) for mouselook, chosen by the bundle's `seamless_mouse`
(doc 07's "Seamless mouse" checkbox). With the tablet the player never
grabs and the guest's hardware cursor is the host cursor; without it a
click takes the pointer and Ctrl+Alt+G gives it back. The player follows
the guest (`mouse_is_absolute`), so a machine can be switched without
touching the player.

**Keyboard.** A key goes to the guest by **where it sits** (winit's
physical key), since the guest has a layout of its own. The exception is
the keys a host keymap option moves (xkb's `ctrl:swapcaps`, `ctrl:nocaps`,
`caps:escape`, `altwin:swap_alt_win`). Control, Shift, Alt, AltGr, Super,
Caps Lock and Escape go as the host reads them (`keymap::as_host_reads`),
so a Caps Lock the host made Control is Control in the guest. The press's
answer is kept for the release. When the window loses focus the player
releases every key the guest still holds (`lift_all_keys`,
`player/src/main.rs`); otherwise Cmd+Tab sends the Windows-key press to
the guest and its release to the next app, and the guest keeps Win down.

**Host shortcuts go to the guest while the window has focus**, grabbed or
not (`player/src/kbcapture.rs`). A Windows machine is on the tablet and
never grabs, and its Start menu is the Windows key. winit has no keyboard
grab, so each windowing system gets its own:

- **Wayland.** `zwp_keyboard_shortcuts_inhibit_manager_v1`, one inhibitor
  for the window's life. The compositor applies it only while the surface
  has focus, and its own `--inhibited` bindings are the user's way out.
  **Not in the Flatpak on a wlroots compositor** (a limitation, user
  decision 2026-09-23): Flatpak 1.15+ gives the sandbox a socket of its
  own through `wp_security_context_v1`, and sway (1.12, `is_privileged`
  in `sway/server.c`) hides the inhibit manager from such clients along
  with layer-shell and screencopy. The player then says "the compositor
  offers no keyboard-shortcuts-inhibit protocol" and the shortcuts stay
  the host's. Binding the real socket into the sandbox is refused by
  bubblewrap. The way round is the X11 path over Xwayland, which runs
  outside the sandbox and asks the compositor on the grab's behalf:
  `flatpak override --user --nosocket=wayland --socket=x11
  com._2ksbox.Launcher`, at the price of the whole app under Xwayland.
  GNOME and KDE decide this for themselves and were not checked.
- **X11.** An active `XGrabKeyboard` while focused, which beats the window
  manager's passive grabs on Super.
- **Windows.** The keyboard is registered for raw input with
  `RIDEV_NOHOTKEYS`. While a window of this process is in front the shell
  does not act on its keyboard hotkeys (both Windows keys, every Win+
  shortcut, Ctrl+Esc), and the key arrives as an ordinary `WM_KEYDOWN`
  down the same path as every other key. No program gets the system
  hotkeys (Alt+Tab, Alt+Esc, Alt+F4, Alt+Space, Ctrl+Alt+Del, Win+L), so
  they stay the host's. The user confirmed raw input on a real keyboard.
  `PLAYER_KEYBOARD_LOG=1` prints the registration and every focus change.
  **Don't go back to a `WH_KEYBOARD_LL` hook.** Windows called it for
  every key except while the player's own window was focused (zero
  calls, not late ones), and blinded a third process's hook too. The
  cause was never found; measurement ruled out `LowLevelHooksTimeout`,
  re-arming, the hook thread's timer, the window, a Windows rule,
  integrity levels and exploit mitigations.
- **macOS.** The window server's hot key operating mode is set to "all
  disabled except Universal Access" while the window has focus and back
  to enabled when it loses focus or the capture is dropped
  (`CGSSetGlobalHotKeyOperatingMode`, SkyLight's private interface: the
  call VirtualBox's `DarwinDisableGlobalHotKeys` and UTM's
  `VMMetalView.captureMouse` make, and UTM ships it on the Mac App Store).
  Off go Cmd+Tab, Cmd+Space, Mission Control and the Spaces arrows
  (Ctrl+Up, which era games use), the screenshot chords and every other
  binding of the Keyboard settings pane; the keys arrive at the window as
  ordinary `keyDown:`s. The Universal Access chords stay the host's:
  accessibility is never the guest's. Cmd+H, Cmd+Opt+H and Cmd+Q are not
  hot keys but the app menu's key equivalents (winit's menu), which the
  menu would swallow before the window sees them; the capture blanks them
  and gives them back on drop. Cmd+Opt+Esc and the fn / Touch Bar media
  keys are not hot keys of the pane and stay the host's. The user
  confirmed Ctrl+Up and Cmd+Tab in a Win98 guest on the Air (2026-09-24).

  **Two public roads were measured dead on macOS 26** (2026-09-24), so
  don't go back to them. The Carbon `PushSymbolicHotKeyMode`, public
  since 10.6 and still exported, is a stub: `GetSymbolicHotKeyMode` reads
  back what was pushed while the window server's mode stays 0 and Cmd+Tab
  keeps switching. An active HID-level `CGEventTap` with the Accessibility
  permission granted, sandboxed or not, sees the modifier presses and
  never the Ctrl+Up / Cmd+Tab / Cmd+Space key downs: the window server
  acts on the chord before any tap. The private call works inside the App
  Sandbox too (probed with an ad-hoc-signed sandboxed binary), so both
  builds use it. `PLAYER_KEYBOARD_MAC=presentation` keeps the public
  `NSApplicationPresentationOptions` route as the fallback should App
  Store review ever refuse the symbol: `disableProcessSwitching` (Cmd+Tab)
  and `disableHideApplication` (Cmd+H), which want the Dock auto-hidden
  while the app is active and cover nothing else, Ctrl+Up included.
  `PLAYER_KEYBOARD_LOG=1` prints the road taken, the equivalents taken,
  and every focus change with the mode read back.

  **Cmd+Q asks, and never `exit()`s under the QEMU thread.** winit's Quit
  item is `terminate:`, and its delegate answers no
  `applicationShouldTerminate:`, so AppKit would `exit()` from inside the
  run loop with the QEMU thread alive (the atexit race the player joins
  the thread to avoid). The player adds that method to winit's delegate
  class at start (`kbcapture::quit_closes_window`, verified with a quit
  Apple event on the Air, 2026-09-24): it closes the key window instead
  and cancels the terminate, so the menu's Quit, the Dock's and Cmd+Q
  while the host has its shortcuts all end as the title bar's close does.
  A `CloseRequested` with Cmd held is Cmd+Q from the keyboard and asks
  like Alt+F4; while the guest has the shortcuts, Cmd+Q reaches the window
  as a key and asks the same way (Win+Q means nothing to the guest, Quit
  means everything to the hand); a second Cmd+Q is the answer.

**Ctrl+Alt+K** toggles the capture for the rest of the run (off drops it
outright, on builds a new one; the title bar says when the host has its
shortcuts). `PLAYER_KEYBOARD_CAPTURE=0` starts a run with it off.

**Checking it by hand** (there is no scripted test: posting keys to
another app needs the Accessibility permission on macOS and a grab-aware
harness elsewhere). Start any machine, click into it, press the Windows
key (Cmd on a Mac): the *guest's* Start menu opens and the host's does
not. Then Win+Tab / Cmd+Tab: the guest's task switcher, and the host's
window stays in front. Ctrl+Alt+K, and the same keys go to the host.

**The player's own chords.** Ctrl+Alt+Del is the host's everywhere, so
**Ctrl+Alt+Shift+D** is the guest's (Shift let go, Delete pressed, and
released with D). **Ctrl+Alt+Shift+F** toggles windowed full screen
(borderless, on the window's monitor). **A close with Alt held asks
first**, since Alt+F4 reaches the player whenever the host has its
shortcuts and a window close stops the machine. The player draws the
question itself (`player/src/prompt.rs`, the VGA 8×16 font blended over
the finished picture), because Linux has no message box that works in the
Flatpak and over a full-screen window. Enter, Close or a second Alt+F4
closes; Esc or Back returns; nothing reaches the guest while it is up.
The title bar's close button does not ask.

## 3D and the pipeline

Guest 3D (qemu-3dfx's GL, the Direct3D executor, the Voodoo
2's VGA-surface frames) goes through the same librashader chain as 2D.
The target is zero-copy: IOSurface on macOS, dma-buf into Vulkan on
Linux (doc 12 §4). Windows, and a Linux host without the Vulkan
extensions, read frames back instead; that works but is the path to
leave. 2D correctness never depends on it.

## Testing

- `--mode-sweep` (above) and `PLAYER_DUMP_OUT` (the shaded frame,
  headless, works while occluded); both are in `docs/testing.md`.
- Real-CRT calibration: `build/crtcal-render` writes doc 09's patterns as
  BMPs, and `player --shader <preset> --calib <dir>` shades them for
  holding against photographs of the rig's tube (doc 09 has the
  protocol).
- Not built yet: golden-image boots to known screens compared pixel-exact
  with shaders off, and a mode-cycling test floppy.
