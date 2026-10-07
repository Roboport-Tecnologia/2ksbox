# Developer guide

The technical companion to the top-level `README.md`, which is for
people who *use* 2ksbox. This covers the build stage by stage, the
player's command line and environment, the launcher's front ends,
packaging, logs and licensing. Neighbours:

- `docs/00-status.md`: current state, open threads, next steps,
  cross-cutting gotchas. Read it first.
- `docs/testing.md`: the testing policy and every test tool.
- `CLAUDE.md`: the locked decisions and conventions in brief.
- `docs/tracks/`: one document per work track.
- `patches/qemu/README.md`: every QEMU patch.
- [build-macos.md](build-macos.md) and [build-windows.md](build-windows.md):
  the platform specifics.

## What exists vs. what we build

| Piece | Status |
|---|---|
| x86 emulation | Exists: a QEMU 11.1 fork, trimmed to what we use, with our own TCG fast paths (x87, SSE, SIMD, REP strings, same-value SMC, inline TB lookup); KVM / WHPX on x86 hosts, HVF for Windows 11 on Arm on a Mac |
| Guest 3D | We build the paravirtual Direct3D device (`d3dpt`, a host executor on DXVK), qemu-3dfx's GL pass-through, and an emulated Voodoo 2 for Glide (doc 21) |
| Guest display drivers | We build `d3dpt-vga` drivers for XP (miniport + DX9 DDI, doc 15), Win98 (mini-VDD + 16-bit driver, doc 19) and Windows 7 (WDDM, track M18) |
| Guest music | We build OPL3 and MPU-401 devices over `libsynth` (doc 20) |
| CRT shaders | Exists: libretro slang presets through librashader (a library, not RetroArch) |
| Player | We build it: in-process QEMU, wgpu + librashader, mode analysis, low-latency audio (Rust) |
| Launcher | We build `launcher-core`, shipped as `launcher-mitsuami` (native widgets through mitsuami: AppKit, WinUI 3, GTK 4), and `launcher-capi` for other languages |
| CD-ROM backend | We build `libdisc` (cue/bin, subchannel, CD-DA, `isodir:` folders), the ATAPI patches, the disc shelf |
| Machine families | We build Windows 98, Windows XP, Windows 7, Windows 11, DOS (throttled CPU rates) and Other |

Authentic-hardware emulation (a real S3, cycle-accurate chipsets) is
86Box's territory and out of scope. The exception is the Voodoo 2,
vendored verbatim from 86Box because only a real chip runs every Glide
title, and it is the machine's only Glide (ADR-016, ADR-020).

## Design docs

0. [**Status and how to resume**](00-status.md) (read first)
1. [Goals and non-goals](01-goals.md)
2. [Architecture: in-process QEMU, process model, threading](02-architecture.md)
3. [Display pipeline: pixel accuracy, CRT shaders, latency](03-display-pipeline.md)
4. [3D acceleration: strategy and paths](04-3d-acceleration.md)
5. [CD-ROM backend: raw images and copy protection](05-cdrom-backend.md)
6. [Guest machines: the families](06-guest-machines.md)
7. [Front end: player + launcher](07-frontend.md)
8. [Roadmap and milestones](08-roadmap.md)
9. [Reference hardware rig](09-reference-hardware.md)
10. [Decision records (ADRs)](10-decisions.md)
11. [M1 embed API design](11-m1-embed-api.md)
12. [M3 window-less GL context provider design](12-m3-context-provider.md)
13. [x87 shadow doubles: the FPU stack as host doubles in TCG](13-x87-inline-tcg.md)
14. [Paravirtual Direct3D device for XP and Win98](14-d3d-paravirt.md)
15. [A real XP display driver: d3dpt-vga, miniport + Direct3D DDI](15-guest-display-driver.md)
16. [SSE on the host FPU: scalar and packed ops inline in TCG](16-sse-inline-tcg.md)
17. [CD-ROM backend: implementation specification](17-cdrom-implementation.md)
18. [Pinned guest registers: the x86 register file in TCG](18-pinned-guest-registers.md)
19. [A native Win98 display driver: d3dpt9x](19-win9x-display-driver.md)
20. [Music: the OPL3 and MPU-401 devices and their engines](20-music.md)
21. [The Voodoo 2 device](21-voodoo2.md)
22. [The CPU-benchmark evaluation of the patch queue](22-tcg-evaluation.md)
23. [Dynamic binary translation: a literature survey](23-dbt-literature.md)
24. [Integration: shared folders and the clipboard](24-integration.md)

Also: [testing](testing.md), [macOS](build-macos.md),
[Windows](build-windows.md), [tracks](tracks/).

## The build, stage by stage

`scripts/build.sh` is the one command, and the one to run after every
`git pull`; it redoes only what changed. `--help` lists the stages
(`deps qemu edk2 virtio rust mitsuami dxvk exec guest`; `deps` builds
QEMU's libraries from source, [build-macos.md](build-macos.md) "The
libraries"; `mitsuami` is the launcher, which needs GTK 4.10+
development files on Linux (`pkg-config gtk4`) and nothing extra on a
Mac; `edk2` is an Arm host's alone, Windows 11 on Arm's firmware
(`scripts/build-edk2.sh`, `patches/edk2/README.md`); `virtio` is
Windows 11's drivers disc for the host's processor
(`scripts/build-virtio-win.sh`), on every host but an Intel Mac).
Naming
stages builds only those,
`--test` follows with `scripts/test.sh host`, and a stage whose tools
are missing is skipped with the reason in the closing summary. What it
runs, for driving one stage by hand:

```sh
scripts/prepare-qemu.sh      # overlay qemu-3dfx + embed/, the patch queue, sign_commit
scripts/configure-qemu.sh    # uv-managed Python; also builds libdisc and libsynth
ninja -C build/qemu qemu-system-i386 qemu-system-x86_64 qemu-img qemu-io \
  libqemu-embed-i386.so libqemu-embed-x86_64.so   # .dylib on macOS, which has no x86_64 target
cargo build --release        # default members; the player links libqemu-embed-i386
cargo build --release -p player --features qemu-x86_64 --target-dir target/qemu-x86_64   # Windows 11's player (Linux)
scripts/build-virtio-win.sh  # Windows 11's drivers disc (the clipboard's driver, the agent), build/virtio-win/2ksbox-drivers-x64.iso
# an Arm host (M20 step 4): ninja also builds qemu-system-aarch64 and libqemu-embed-aarch64, then
scripts/build-edk2.sh        # Windows 11 on Arm's firmware into qemu/pc-bios
scripts/build-virtio-win.sh  # there its drivers disc instead, build/virtio-win/2ksbox-drivers-arm64.iso
cargo build --release -p player --features qemu-aarch64 --target-dir target/qemu-aarch64   # its player
codesign --force --sign - --entitlements packaging/macos/hypervisor.entitlements target/qemu-aarch64/release/player   # a Mac: HVF
cargo check --release --workspace          # launcher-capi, the one non-default member
(cd launcher-mitsuami && cargo build --release)   # the launcher (build.sh's mitsuami stage)
(cd player-mitsuami && cargo build --release)     # the player every package ships; Windows 11's with
                                                  # --features qemu-<arch> --target-dir target/qemu-<arch>
(cd launcher-mitsuami && cargo build --release)  # the launcher; its own workspace
# Direct3D pass-through (doc 14):
scripts/prepare-dxvk.sh && scripts/configure-dxvk.sh && ninja -C build/dxvk && scripts/build-d3dpt-exec.sh
# the guest-tools ISO (SETUP.EXE, the guest DLLs, both display drivers):
scripts/wddm-prebuilt.sh fetch   # Windows 7's WDDM driver, the PC's build for these sources
guest-tools/build-wrappers.sh
```

What each stage needs to know:

- **Prepare steps are stamped.** A prepare re-applies its patch queue
  and hands the build system thousands of fresh mtimes, so running it
  every time costs a full QEMU rebuild. `build.sh` hashes each prepare's
  inputs into `build/.stamp-*` and skips it when they are unchanged;
  `-f` re-runs them all (after a tree was edited by hand, or a checkout
  moved). An overlay missing from a stamp's inputs is a trap (00-status,
  "Building").
- **`qemu/embed/` is an rsync copy of `embed/`** made by
  `prepare-qemu.sh`. A stale copy links the player against an old
  library: `undefined symbol _qemu_embed_…` (on macOS `Undefined symbols
  for architecture arm64: _qemu_embed_…`); `qemu-embed/build.rs` warns
  when the copy is stale. Bumping the embed API moves the header's
  `QEMU_EMBED_API_VERSION` and the crate's `API_VERSION` together.
- **Re-run `configure-qemu.sh` whenever meson files change**, and after
  a prepare, before `ninja`: a refreshed overlay can make ninja
  regenerate the build with default options, `werror` back on among
  them. `configure-qemu.sh` also builds `libdisc` (the CD-ROM model) and
  `libsynth` (the music engines), which link into QEMU (patches 50 and
  60).
- **`QEMU_PYTHON=<interpreter>`** makes `configure-qemu.sh` use that
  interpreter and never consult uv (3.8–3.13 enforced; 3.14 only with the
  real `distlib`). It is for a sandbox that has a Python and cannot fetch
  one, such as the Flatpak.
- **QEMU links a GLib of its own on Linux** (`scripts/build-deps.sh`
  builds pcre2, GLib and libslirp into `build/deps/<arch>`, static,
  libtpms with OpenSSL's libcrypto for patch 75's TPM, and
  spice-protocol's headers for the clipboard;
  `build.sh`'s `deps` stage runs it). `configure-qemu.sh` links them
  with their symbols hidden and turns smartcard off, so
  `libqemu-embed` shows no system GLib in `ldd`. The reason is QEMU's
  main loop: it iterates GLib's global default `GMainContext` on QEMU's
  thread, and with a GLib shared with its process, a toolkit that runs
  on that context (GTK) would have its sources dispatched there (`tracks/m22-mitsuami-player.md`, "Why QEMU links a GLib of its own"). GLib 2.90
  wants meson 1.4. **`QEMU_DEPS=system`** links the distribution's GLib
  and libslirp instead, and libtpms if it has one (`build.sh` and
  `configure-qemu.sh` both read it;
  `build.sh` reconfigures QEMU when it changes).
- **A `D3DPT_PROTO_VERSION` bump makes the executor and the guest-tools
  ISO stale, silently.** The suite fails as `d3dpt-dp2: protocol
  mismatch` and as a guest that never attaches. `build.sh` rebuilds
  both; on a host that cannot (no mingw), its summary names the
  artefacts left behind.
- **On macOS every stage targets the macOS floor**, 12.0
  (`scripts/macos-floor.sh`). `build.sh` and `test.sh` export
  `MACOSX_DEPLOYMENT_TARGET`, QEMU and DXVK take it as a flag, and a
  cargo workspace linked for a newer macOS is cleaned first
  ([build-macos.md](build-macos.md), "The floor").
- A build belongs to one checkout. Never borrow another's `build/`,
  `target/` or `*_BIN` (00-status, "Building").
- **`--x86_64` on an Apple Silicon Mac is the Intel build**, under
  Rosetta, into `build/x86_64/`, `build/deps/x86_64/` and
  `target/x86_64-apple-darwin/` beside the native build's; the `dxvk`
  stage is skipped there (no Vulkan on an Intel Mac) and the `exec` stage
  builds only the executor on Wine ([build-macos.md](build-macos.md),
  "The Intel build").

The player with no launcher:

```sh
target/release/player        # no arguments: the test pattern (integer-scaled 4:3)
target/release/player -- -L $PWD/qemu/pc-bios -machine pc -m 32 \
  -drive file=path/to/floppy.img,format=raw,if=floppy -boot a -vga std -net none
```

Everything after `--` is a `qemu-system-i386` command line. The player
adds `-display none` and `-audiodev embed,id=embed0` itself; attach the
audio with e.g. `-machine pc,pcspk-audiodev=embed0` or `-device
sb16,audiodev=embed0`. `launcherx --print-args <machine.toml>` gives the
exact line a launcher machine runs.

## The player: command line and environment

```
player [--shader <preset.slangp>] [--shader-params <k=v,...>]
       [--pad usb|gameport|keys] [--share <dir>] [--pads] [--pad-sweep <frames>]
       [--mode-sweep <dir>] [--calib <bmp|dir>] [--companions]
       [--] <qemu args...>
```

### Shaders

- `--shader <preset.slangp>` (or `PLAYER_SHADER=`) runs a libretro slang
  preset, e.g. `third_party/slang-shaders/crt/crt-lottes.slangp`;
  `shaders/README.md` has the curated ones.
- `--shader-params <name=value,...>` (or `PLAYER_SHADER_PARAMS=`)
  overrides the preset's parameter defaults by name
  (`BRIGHTBOOST=1.4,GAMMA_INPUT=2.4`). A launcher shader profile
  (`launcher-core/src/shader_profile.rs`) resolves to this.
- `--calib <bmp|dir>` shades doc 09's CRT calibration patterns
  (`build/crtcal-render` writes them) and exits.
- `--mode-sweep <dir>` runs doc 03's mode sweep instead of a guest and
  writes a PNG per mode. `PLAYER_MODE_PARAMS=0` is its control: the
  preset guesses the scanline count from the framebuffer height.

### Frames and dumps

- `PLAYER_DUMP=frame.png PLAYER_DUMP_SEQ=150` dumps guest frame 150 and
  exits.
- `PLAYER_DUMP_OUT=out.png` dumps the *shaded* frame (GPU readback) at
  `PLAYER_DUMP_SEQ` and exits, even while the window is occluded.
- `Ctrl+Alt+S` writes the guest's own frame (native size, no geometry
  stage, no CRT chain) as `PLAYER_SHOT_DIR/2ksbox-NNNN.png`, or in the
  user's Pictures folder's `2ksbox` when that is unset (doc 03,
  "Screenshots"). It is evidence of what the
  machine rendered, for tests and bug reports. **It is not a picture
  of the product**: a screenshot meant for people (a Store listing,
  Flathub, the README) is the player's window after the shader chain,
  at the window's resolution, with the player full screen
  (`Ctrl+Alt+Shift+S`, or the OS's own screenshot); a guest frame scaled up shows something the
  box never draws (user, 2026-09-25).
- `Ctrl+Alt+Shift+S` writes that picture: the window's content after
  the geometry stage and the CRT chain, at the window's size, black
  bars included, to the same numbered files. It draws the chain's last
  output again into a texture of the swapchain's format with the
  blit the window uses (inside the viewport it equals the mode sweep's
  chain dump, 0 pixels differ); the close prompt is left out.
- `PLAYER_SHOT_EVERY=300` takes the guest-frame shot every 300 presented guest
  frames, driven from the wake path so a window behind a terminal still
  shoots. This is how a headless run sees a 3D frame; a QMP screendump
  shows the VGA surface, frozen while the 3D device presents.
- `PLAYER_REFRESH_MS=16` (default) is the guest frame pull interval
  (QEMU's own default is 30).
- `PLAYER_REFRESH_LOG=1` prints a frame counter every 100 guest frames:
  is the guest drawing at all.
- `PLAYER_LATENCY=1` prints publish→present latency percentiles every
  240 guest frames.
- `PLAYER_ZERO_COPY=0` refuses every dma-buf the backend offers, so 3D
  frames take the readback path instead of the ring (doc 12 §4). The
  readback copies under a lock, so a wrong picture that survives it is
  not the ring's.
- `PLAYER_PUBLISH_LOG=1` prints a line per published frame naming its
  source (which ring slot, the readback path, the VGA surface, the
  cursor's republish) and a line per presented frame. Two sources taking
  turns is a flicker the picture alone does not show. Pair the presented
  slot with `PLAYER_SHOT_EVERY=1`'s shots.
- `PLAYER_CURSOR_LOG=1` prints each change of the host cursor: default,
  hidden, or the guest's shape.

### Keys, pointer and window

- `PLAYER_KEYS="120:enter,360:ctrl+g"` presses keys or chords at guest
  frames, each held `PLAYER_KEYS_HOLD` frames (default 6, ~100 ms): a
  down+up in one flush is a zero-length press that a game polling the
  keyboard state never sees.
- `Ctrl+Alt+G` releases the grab. `Ctrl+Alt+Shift+D` is Ctrl+Alt+Del in
  the guest. `Ctrl+Alt+Shift+F` toggles borderless full screen on the
  window's monitor.
- A close with Alt held (Alt+F4 while the host has its shortcuts) asks
  first, in the window: Enter, Close or a second Alt+F4 stops the
  machine; Esc or Back returns to it. The title bar's close button does
  not ask.
- While the window has focus the host's shortcuts go to the guest, so
  the Windows key opens the guest's Start menu. The mechanism is
  Wayland's shortcut inhibitor, an X11 keyboard grab, or raw input with
  `RIDEV_NOHOTKEYS` on Windows (the two Windows keys, every Win+
  shortcut and Ctrl+Esc; Alt+Tab, Alt+F4, Ctrl+Alt+Del and Win+L are
  system hotkeys no program gets), or the window server's hot keys off
  on macOS (Cmd+Tab, Cmd+Space, Mission Control, Ctrl+arrows, the
  screenshot chords; the app menu's Cmd+H and Cmd+Q taken too, and
  Cmd+Q asks before it closes, like Alt+F4;
  `PLAYER_KEYBOARD_MAC=presentation` uses the public presentation
  options instead, which cover only Cmd+Tab and Cmd+H). `Ctrl+Alt+K` toggles
  them between host and guest (the title says when they are the
  host's). `PLAYER_KEYBOARD_CAPTURE=0` starts with them the host's;
  `scripts/test.sh` sets it. `PLAYER_KEYBOARD_LOG=1` prints what the
  Windows or macOS side did: whether the raw-input registration or the
  hot key mode was accepted, and what winit and the system each thought
  about focus at every change. A shortcut that still reaches the host is
  nearly always a window that was not in front. The one known exception is the Flatpak on a wlroots
  compositor, where the sandbox never sees the inhibit protocol (doc 03,
  "Flatpak" below). Design and measurements: doc 03 §"Input path",
  `player/src/kbcapture.rs`.
- `qemu-embed: input:` lines on stderr report the embed input queue's
  drain latency, zero-length presses and drops, only when something is
  off.

### The mitsuami player (M22)

`player-mitsuami/` takes the same command line and every `PLAYER_*`
knob, which are `player-core`'s. It is its own cargo workspace: `cd
player-mitsuami && cargo build --release` (GTK 4.10+; `--no-default-features
--features kde,gilrs` for Kirigami); on Windows `build-windows.sh
mitsuami` builds it with MSVC after the launcher (`docs/build-windows.md`),
and there it presents through Direct3D 12 unless `WGPU_BACKEND` says
otherwise (Vulkan's frames never show on its child window). On macOS and
Linux `build.sh mitsuami` builds it beside the launcher, with Windows
11's (`--features qemu-x86_64` / `qemu-aarch64` into
`player-mitsuami/target/qemu-<arch>`). Every package ships it as
`2ksbox-player` (since 2026-10-07); the winit player is deprecated and
builds only for `test.sh` and the tools until they move. It is the default player: once it
is built, a launcher in the checkout starts it instead of the winit one
(`launcherx --paths`; `LAUNCHER_PLAYER_BIN` overrides). Its chords are the winit player's, as menu shortcuts
(Machine: Send Ctrl+Alt+Del, Send Shortcuts to Guest, Release Mouse,
Pause, Reset, Power Button, Close; View: Full Screen, the two
screenshots; on macOS View has AppKit's own Enter Full Screen instead of
ours, which the chord still toggles), and
a keyboard close asks in the platform's alert. Two knobs of its own:
`PLAYER_INPUT_LOG=1` prints every input the surface reports, with the lock
and grab state, and `PLAYER_SURFACE_LOG=1` every size it reports.
`tracks/m22-mitsuami-player.md` has what is checked and what is not.

### Audio and music

- `PLAYER_AUDIO_MS=40` (default) is the cushion QEMU keeps in the ring
  under the host device's pull: latency on top of the device's period,
  and how late QEMU's main loop may run before a gap is heard. Raise it
  if gaps are counted, lower it under KVM. The stderr lines to read are
  `qemu-embed: audio:` and `[audio] … underruns` (gaps), `[audio] device
  asks for N frames` (how chunky the device is), and `[audio] the
  guest's mix went past full scale` (voices summed past full scale,
  since QEMU applies no mixer volume there, and how far the limiter
  turned it down). Pacing design: doc 11.
- `QEMU_EMBED_AUDIO_TRACE=1` prints the embed audiodev's pacing, a line
  per call.
- `PLAYER_AUDIO_NULL=<frames>` drains the ring with no device, like a
  DAC taking that many frames a period at 48 kHz (`1` = 1024,
  PipeWire's default).
- `PLAYER_AUDIO_TAP=out.wav` records exactly what the player handed the
  device, padded silence included (`tools/audio-glitch-test.py` counts
  clicks in it).
- `LIBSYNTH_SF2=<file.sf2>` is the General MIDI bank (doc 20). The
  player sets it itself (the packaged bank, or `soundfonts/` in a
  checkout), so this is for trying another; a machine that names its own
  wins. `LIBSYNTH_MT32_ROMS=<dir>` is the same for the CM-32L's ROMs,
  which are the user's own.
- `LIBSYNTH_MIDI_LOG=<file>` / `LIBSYNTH_OPL_LOG=<file>` capture what a
  guest wrote to a music device, for `synthx midilog` / `opllog` /
  `play` (doc 20 §7.2).

### The shared folder and the clipboard (M23)

- `--share <dir>` (after `--pad`) serves `<dir>` to the guest as
  `\\10.0.2.4\host` (user `2ksbox`, password `2ksbox`): `libsmb` in the
  player on a Unix socket in a 0700 directory under the temp dir, and
  `guestfwd=tcp:10.0.2.4:445-unix:<socket>` added to the machine's
  `-netdev user` (QEMU patch 79). No user-mode network or a Windows host:
  `[share] not shared: …` and the machine runs without it.
  `PLAYER_SMB_LOG=1` prints every request.
- The clipboard needs no option: a command line with `-chardev
  qemu-vdagent` gets the bridge to the host's clipboard
  (`player-core/src/clipboard.rs`), which prints a `[clipboard]` line
  per transfer.

### Gamepads

Track: `docs/tracks/m13-gamepads.md`. The launcher writes `--pad` from
the machine's `pad` setting (`launcherx --print-player-args
<machine.toml>` shows it).

- `player --pads` says what this host can read: the one place a build
  without the `gilrs` feature, or a sandbox with no `/dev/input`,
  reports itself.
- `--pad usb` (or `PLAYER_PAD=usb`) drives the machine's `usb-gamepad`
  (patch 26): two sticks, an 8-way hat and twelve buttons, bound by the
  HID driver of XP, 98 SE and Me with nothing installed (98 SE asks for
  its source files the first time). The launcher adds `-usb -device
  usb-gamepad`. Not offered on DOS.
- `--pad gameport` drives the gameport at 0x201 (patch 27) with the same
  pad state: two axes and four buttons, the face buttons as 1–4, the
  d-pad folded onto the first stick's axes. Offered on DOS and Win98; on
  9x the port wants Add New Hardware and a calibration.
- `--pad keys` (or `PLAYER_PAD=keys`) maps the pad onto keys: the d-pad
  and left stick are the arrows, the face buttons Ctrl, Alt, Space and
  Enter, Start is Esc. It works on every guest, but a game asking
  DirectInput for a joystick still finds none.
- `PLAYER_PAD_SCRIPT="30:lx=1.0,45:south=1,51:south=0"` is a synthetic
  pad that wins over real hardware: a control set to a value at a guest
  frame (frames, not milliseconds, so a run lands in the same place
  every time). Controls: `lx`/`ly`/`rx`/`ry` (-1.0..1.0, negative is
  left/up), `south`/`east`/`west`/`north`,
  `dpad_up`/`down`/`left`/`right`, `l1`/`r1`/`l2`/`r2`, `l3`/`r3`,
  `select`/`start` (0 or 1).
- `PLAYER_PAD_LOG=1` prints every shaped reading and its transitions.
- `PLAYER_PAD_SHAPING="0.30,0.55,0.40"` overrides deadzone, press and
  release. Release must be below press: with one threshold a stick held
  at it chatters at the poll rate.
- `player --pad-sweep <frames>` replays `PLAYER_PAD_SCRIPT` with no
  window, QEMU or guest and prints what came out (with `--pad keys`, the
  key presses too). It is the `pad` check.

### QMP

The player always attaches a control monitor over a socketpair (no
socket file). A script adds its own `-qmp unix:…,server,nowait`.

- `PLAYER_QMP=1` logs every QMP event (SHUTDOWN, RESET, STOP… are logged
  regardless).
- `PLAYER_QMP_EXEC='{"execute":"query-status"}'` (or a JSON array) runs
  requests once the guest has drawn its first frame and prints the
  replies, from a thread of its own (a `quit`'s reply comes only after
  QEMU has torn down, which waits for the UI thread).

### Direct3D pass-through (doc 14)

The `d3dpt-vga` adapter loads the executor (`D3DPT_EXEC_LIB`, else
`build/d3dpt/libd3dpt_exec.so`) and DXVK (`D3DPT_DXVK_LIB`) on the
guest's first use. `D3DPT_EXEC=auto|dxvk|wine|none` picks the back end,
as does the adapter's `exec=` (`-global d3dpt-vga.exec=wine` in the
machine form's extra arguments). `-global d3dpt-vga.no-exec=on` models a
host with no executor. On Windows `D3DPT_D3D9=auto|dxvk|system` picks
the Direct3D 9 underneath ([build-windows.md](build-windows.md)).
`launcherx --host-check` says which back end this host gets.

The executor on Wine (ADR-018) is loaded through `libd3dpt_exec_remote`
inside QEMU, whose knobs are:

| Variable | Default |
|---|---|
| `D3DPT_EXEC_REMOTE_LIB` | `build/d3dpt/` or `lib/2ksbox/` |
| `D3DPT_EXEC_HOST` (`d3dpt-exec-host.exe`) | `wine/` beside the library |
| `D3DPT_WINE` | the Mac Wine apps, then `wine64`/`wine` on `PATH`; a path that does not exist means *no Wine* (how a test takes it away) |
| `D3DPT_WINEPREFIX` | `<data dir>/2ksbox/wine` (`$XDG_DATA_HOME`, `~/.local/share`, `~/Library/Application Support`) |
| `D3DPT_WINE_RENDERER` | `gl`; `vulkan` is a data point only |
| `D3DPT_REMOTE_DIR` | the directory of the shared VRAM file |

Diagnostics:

- `D3DPT_DP2_TRACE`, `D3DPT_DDI_REREAD`, `D3DPT_DDI_NOFOG` trace the
  display driver's DP2 stream (doc 15; `docs/testing.md`).
- `D3DPT_DDI_FLUSH_DRAWS=n` sets the executor's flush hint (16 draws; 0
  turns it off, doc 15), and `D3DPT_DDI_FLUSH_AB=n` alternates it with n,
  one 5 s rate line each, for an A/B inside one run (`tools/ddi-rate.py`).
- While a 3D device is active, the player shows the VGA surface again
  after 1 s without a presented frame if the guest drew on it (an error
  box, a movie, a crashed game): `[display] no 3D frame for …`.
- `player --companions` prints what `player-core/src/companions.rs` resolved
  for the executor, DXVK and the Wine pair; it is the packagers' check.

### OpenGL pass-through (doc 12)

The guest's `OPENGL32.DLL` (`OPENGL\` on the ISO, `SETUP /GAME 3`) is
qemu-3dfx's wrapper; it reaches the device through the mapper SETUP's
component 2 installs (`MAPPER\` on the ISO). It reads **`WRAPGL32.EXT` from the game's own
folder**, shipped beside it with `ExtensionsYear,1997`: a modern host's
extension string runs to thousands of characters, and a 1990s title
copies it into a fixed buffer (GLQuake's is 4096 bytes; it returns into
the text of the list). Raise the year for a later game or delete the
file. `SETUP /GAME` never overwrites one.

qemu-3dfx's host-side knobs are in `mesagl.cfg`, read from QEMU's
*current working directory* at start-up, one `Key,value` per line:
`ExtensionsYear`, `ExtensionsLength`, `VertexCacheMB`, `DispTimerMS`,
`BufOAccelEN`, `ContextMSAA`, `ContextSRGB`, `ContextVsyncOff`,
`RenderScalerOff`, `FpsLimit`, `DumpShader`, `CheckError`, `FifoTrace`,
`FuncTrace`. On macOS `DispTimerMS` also picks the GL profile (0, the
default, is core; non-zero is compatibility). If presentation stutters
unless the mouse moves, try `DispTimerMS,16`, `ContextVsyncOff,1` or
`FpsLimit,60` first.

On a Linux host, frames go through the embed backend's dma-buf ring
(doc 12 §4):

- `EMBED_ZC_PROBE=<n>` sets how often a slot is checked for still being
  the memory it was made over (a known colour written through GL, read
  back with `gbm_bo_map`). A slot that fails is freed and made again:
  this is the repair, not a diagnostic. Checks are dense while the ring
  is young, then one present in 512. `=0` turns it off; `EMBED_ZC_HEAL=0`
  leaves a bad slot alone to study.
- `EMBED_ZC_SLOTS=<n>` uses the first n buffers (default all).
- `EMBED_ZC_CHECK=<n>` reads four pixels of the buffer just blitted,
  every n-th present, through GL and from the buffer's memory. Weaker
  than the probe: while the picture does not change, the two agree
  whether or not the buffer is written. `EMBED_ZC_MARK=1` adds a line per
  present, for cutting a `FuncTrace,2` log.
- `PLAYER_ZC_IMPORT=0` accepts every dma-buf and imports none (declining
  one turns the ring off), so the ring runs with no Vulkan behind it.
  The picture is wrong while set; it is for reading `EMBED_ZC_CHECK`
  lines.
- `EMBED_ZC_SETTLE=<ms>` waits that long after offering a buffer before
  using it, since accepting an offer only queues the import. It rules the
  import race out (it is not the cause).
- `tools/zc-vulkan-test.c` drives the ring with the frontend's Vulkan
  import alone (`docs/testing.md`).

## The launcher's front ends

`launcher-core` decides everything and a front end draws and forwards
events (doc 07, ADR-014). `launcher-mitsuami` is the only front end
and the one every package installs as `2ksbox` (ADR-023, which
supersedes ADR-015): native widgets through the mitsuami toolkit,
AppKit on macOS, WinUI 3 on Windows, GTK 4 on Linux (Kirigami with
`--no-default-features --features kde`). The Qt launcher
(`launcher-qt`) was deleted on 2026-10-02 (user decision), as the egui
one was before it (ADR-017).

`launcher-mitsuami` is its own cargo workspace, so a root `cargo build`
never needs GTK. `build.sh`'s `mitsuami` stage builds it; a Linux host
with no GTK 4.10+ development files skips that stage and can roll no
package. On Windows it is MSVC like the rest of the package (WinUI 3,
static C runtime) and builds only on a PC (`scripts/build-windows.sh mitsuami`,
[build-windows.md](build-windows.md)). Run it from a checkout with

```sh
(cd launcher-mitsuami && cargo run --release)
launcher-mitsuami/target/release/launcher-mitsuami   # or the built binary
scripts/win-run.sh launcher                          # Windows, in MSYS2's MINGW64 shell
```

It finds the player as every launcher does (doc 07: the mitsuami player
when built, else the root `target/release`; `LAUNCHER_PLAYER_BIN`
overrides).

The toolkit-free debug verbs (`launcher_core::cli`: `--print-args`,
`--print-player-args`, `--prepare` (a Windows 11 machine's firmware
variables, made before a hand-run QEMU starts it), `--new`, `--discs`,
`--host-check`, `--paths`, `--diagnose`, `--wizard-edit`, …) answer
identically from `launcher-mitsuami` and from `launcherx`, a binary
with no toolkit that `scripts/test.sh` and the guest tools drive:

```sh
cargo build --release -p launcher-core --bin launcherx
target/release/launcherx --print-args ~/.local/share/2ksbox/machines/xp/machine.toml
```

The launcher grabs its own real windows: `LAUNCHER_SHOT=<png>`
(`launcher-mitsuami/src/shot.rs`) draws the window's content into a PNG
after `LAUNCHER_SHOT_DELAY_MS` (800) and exits, and `LAUNCHER_SCREEN`
picks the window (unset is the machine window; `wizard[:<family>…]`,
`edit:<machine.toml>`, `clone:<machine.toml>` and the rest are in
[tracks/m19-mitsuami-launcher.md](tracks/m19-mitsuami-launcher.md)
"Test loop"). On Linux it runs on a private Broadway display, so
nothing opens on the desktop:

```sh
gtk4-broadwayd :7 &
LAUNCHER_LIBRARY_DIR=/tmp/lib LAUNCHER_DISC_LIBRARY=/tmp/discs.toml \
LAUNCHER_SHADER_PROFILES_DIR=/tmp/profiles \
GDK_BACKEND=broadway BROADWAY_DISPLAY=:7 GTK_USE_PORTAL=0 \
LAUNCHER_SHOT=/tmp/main.png launcher-mitsuami/target/release/launcher-mitsuami
```

AppKit has no offscreen mode, so on a Mac the window shows for a
moment. `LAUNCHER_SHOT_TREE=1` also prints every node's kind and frame.
`scripts/test.sh`'s `mitsuami` check drives the windows this way.

**`launcher-capi`** is a C ABI over the same models (opaque handles,
index-addressed rows, caller-owned strings) for a front end in Swift or
anything that speaks C:

```sh
cargo build -p launcher-capi            # liblauncher_capi.{a,so}; not a default member
cc -Ilauncher-capi/include my_frontend.c target/debug/liblauncher_capi.a -lstdc++ -lm -ldl -lpthread
```

`launcher-capi/include/launcher_core.h` is the header;
`launcher-capi/examples/smoke.c` is a working miniature front end and
the `capi` check.

## Testing

Integration and end-to-end only, run locally by `scripts/test.sh
host|all`; policy and tools in [testing.md](testing.md). CI
(`.github/workflows/ci.yml`) is manual-trigger only (`workflow_dispatch`)
and never runs the suite.

## Packaging

Everything is named **2ksbox** (ADR-011). The install layout every
package shares is doc 07's "The install layout". Every packager opens
the staged launcher's real window with `LAUNCHER_SHOT` and requires a
PNG (on Linux and in the Flatpak on a private Broadway display),
because a toolkit that is half installed passes every other check and
opens nothing.

### Linux tarball

```sh
scripts/package-linux.sh                  # build/package/2ksbox-<version>-linux-<arch>.tar.zst
scripts/package-linux.sh --with-shaders   # + the ~80 MB preset collection
```

It stages the launcher, the two players (the era machines' and Windows
11's `2ksbox-player-x86_64`, each with its embed library), our `qemu-img`,
the firmware, the guest-tools ISO, Windows 11's drivers disc
(`build/virtio-win/2ksbox-drivers-x64.iso`, the `virtio` stage) and the
libraries QEMU `dlopen`s (executor + DXVK, the Wine pair) into one relocatable
prefix, checks that everything resolves inside it from a scrubbed
environment (`docs/testing.md`), and rolls a tarball. **GTK 4 is not in
it**: it needs the distribution's GTK 4, 4.10 or later (Arch and
Fedora: `gtk4`; Debian/Ubuntu: `libgtk-4-1`), and `install.sh` names
it when the loader cannot find it. The extracted tree runs in place (`bin/2ksbox`); `install.sh`
copies it into a prefix (`~/.local` by default) with a desktop entry,
`com._2ksbox.Launcher.desktop` (the window's `app_id`). The tarball
ships no system libraries, so it wants a host much like the one that
built it; the Flatpak is the portable answer.

### Flatpak

```sh
scripts/package-flatpak.sh          # build, install --user, smoke check
flatpak run com._2ksbox.Launcher
```

Built from source against `org.gnome.Sdk` 49 (GTK 4 from GNOME's
runtime, `org.freedesktop.Platform` 25.08 underneath; it was
`org.kde.Platform` 6.10 while the launcher was Qt, until 2026-10-02).
Set `FLATPAK_BUILD_DIR` (and flatpak's own `FLATPAK_USER_DIR`) to keep
the ~12 GB build tree off the root filesystem. The build is offline, as Flathub requires: every
crate is declared with a checksum in `packaging/flatpak/cargo-sources.json`.
Run `scripts/gen-flatpak-cargo-sources.sh` and commit the result
whenever a dependency changes. Both manifests take their branch from the
builder (`--default-branch=stable`, Flathub's), so an older `master`
build stays installed beside a new one until it is uninstalled; the
script names the branch in every ref.

**A file picked in the sandbox is kept by its real path.** The portal's
dialog returns a document-portal path, which QEMU cannot lock and which
hides a disc image's companion files; the launcher turns it back into
the host path the portal records on the file (doc 07, `browse::picked`)
and heals a shelf written before this on load. `launcherx --picked
<path>` prints what is kept.

**Host shortcuts stay the host's on sway** and other wlroots
compositors, a documented limitation (user decision, 2026-09-23): the
sandbox's Wayland socket carries a security context and sway hides the
shortcut-inhibit protocol from it (doc 03 "Input path" has the
mechanism). The override is X11 over Xwayland for the whole app:

```sh
flatpak override --user --nosocket=wayland --socket=x11 com._2ksbox.Launcher
```

The app ships no Wine and no PE pair. Below the Vulkan floor they come
from the app's add-on, `com._2ksbox.Launcher.Wine`
(`packaging/flatpak/com._2ksbox.Launcher.Wine.yml`): a 64-bit Wine built
from source, trimmed to what WineD3D over OpenGL needs, and the pair
built with `org.freedesktop.Sdk.Extension.mingw-w64`, mounted at
`lib/2ksbox/wine`. The script builds it after the app (the app is its
runtime) and the check expects it; `--no-wine` skips it, `--wine-only`
rebuilds just the add-on onto the installed app. A user installs it from
the app's page in the store, or with
`flatpak install flathub com._2ksbox.Launcher.Wine`; the wizard's
Direct3D note says so on a below-floor host.

Windows 11's drivers disc is made on the host by
`scripts/build-virtio-win.sh x64` (the SDK has no Windows target for the
agent and no ISO writer) and comes in as a file, as the patched QEMU tree
does; the script refuses to start without it.

The launcher is GTK 4 there, and the KDE build is a second add-on,
`com._2ksbox.Launcher.KDE` (`packaging/flatpak/com._2ksbox.Launcher.KDE.yml`,
user decision 2026-10-04): the launcher built with `--features kde`, and
the Qt 6.10.3 and KDE Frameworks it needs (Qt's base, SVG, shader tools
and Quick, and Linguist's tools to build the frameworks' translations;
Kirigami, qqc2-desktop-style and their frameworks, Sonnet among them for
the desktop style's QML; Breeze's widget style and its icons as a
library), about 190 MB installed, built from source at the commits
`org.kde.Platform` 6.10 uses, since an add-on runs on its app's runtime.
It mounts at `/app/kde` with `lib` on the loader's path. The app's
`2ksbox` is a script (`packaging/flatpak/2ksbox.sh`) that starts
`/app/kde/2ksbox` when `XDG_CURRENT_DESKTOP` names KDE and the add-on is
there, and the GTK launcher (`bin/2ksbox-gtk`) otherwise. There is no
plasma-integration (Plasma's platform theme would bring KIO and some
thirty frameworks), so the script asks for the Breeze style and lets
KDE's settings read the session's `~/.config/kdeglobals` (the colour
scheme, the icon theme). `--no-kde` skips it, `--kde-only` rebuilds just
the add-on; the check runs `2ksbox` with `XDG_CURRENT_DESKTOP=KDE` on Qt's
offscreen platform and wants the add-on's launcher and a window grab.

### macOS (`2ksbox.app` / `.dmg`)

`scripts/package-macos.sh` on Apple Silicon bundles the whole non-system
dylib closure and the executor with the LunarG loader and KosmicKrisp
(the launcher is on AppKit and brings no toolkit), then signs with the
hardened runtime and the JIT entitlement, notarizes and staples.
On Apple Silicon both builds carry Windows 11 on Arm: its player
(`2ksbox-player-aarch64`, with the hypervisor entitlement), our EDK2 and
the ARM64 drivers disc. `--community` is ADR-019's community build, which adds the Wine pair.
`--x86_64` (after `scripts/build.sh --x86_64`) is the Intel app, made on
the same Mac under Rosetta: always the community build, with no Vulkan,
into `build/macos-x86_64`. Nothing in either app comes from Homebrew.
Details: [build-macos.md](build-macos.md), "The app" and "The Intel
build".

### Windows (`.zip`)

Built on a Windows PC, in MSYS2's MINGW64 shell
(`scripts/build-windows.sh`, `scripts/package-windows.sh`; ADR-026):
`2ksbox.exe` (the launcher) and `2ksbox-player.exe` (`player-mitsuami`),
WinUI 3 and MSVC, needing the Windows App Runtime 2.4 or later,
`2ksbox-player-x86_64.exe` (Windows 11's, which a Windows host does not
run yet: no TPM there), QEMU built against MSVC
(`libqemu-embed-i386.dll`, `libqemu-embed-x86_64.dll`, `qemu-img.exe`;
the `qemu-msvc` stage), the executor with DXVK, firmware and guest tools
in one portable folder that ships no runtime DLL. The
same folder packs as an MSIX for the Microsoft Store
(`scripts/package-msix.sh`, on a PC with the Windows SDK).
Details: [build-windows.md](build-windows.md).

## Diagnostics and logs

- `launcher --paths` prints where this build looks for each companion
  and where the library is (`library …`; `(packaged)` from an installed
  MSIX, whose library is `%USERPROFILE%\2ksbox`, and `LAUNCHER_PACKAGED=1`
  makes any build answer as one); ask for it first when something says a
  file is missing. `launcher --diagnose` adds this host's 3D and files it
  in `launcher.log` (beside the machine library), which is what to send
  when the launcher did not come up. On Windows, where the launcher has
  no stdout, `2ksbox-debug.bat` in the package does that.
- Every Start writes the full player command line to `launcher.log` as
  `[player] …`, quoted for pasting back into a shell.

## Licensing, for packagers

Everything that links QEMU in-process is GPL-2.0: the players
(`player`, `player-core`, `player-mitsuami`), `qemu-embed`, and `libdisc` and `libsynth`, which are compiled into QEMU.

The **launcher** (`launcher-core`, `launcher-mitsuami`, `launcher-capi`) and
the `shader-chain` crate it shares with the player are
**GPL-2.0-or-later** (ADR-009). None links QEMU (the launcher spawns the
player as a separate process), and they link Apache-2.0 crates (`ring`
under `ureq`'s rustls, among others) that GPLv2 cannot take and GPLv3
can.

Original code is Rust wherever possible (ADR-004); C appears only inside
QEMU / qemu-3dfx and in guest-side era code. The GPLv2 text is in
`COPYING`, every third-party component in `THIRD-PARTY-NOTICES.md`.

**If you package or redistribute the player, read this.** Its
dependency tree contains **Apache-2.0-only** crates (`winit`, `cpal`,
`ab_glyph`, `codespan-reporting`, `rspirv` among them), and Apache-2.0
is incompatible with GPLv2, which the player is pinned to because it
links QEMU. `winit` alone settles it; being clean would mean dropping
wgpu and librashader, the CRT shader chain the project exists for. **We
ship player binaries anyway**, with complete source and build scripts;
the reasoning and the rejected alternatives are ADR-010. If your
distribution's policy cannot accept that, please open an issue rather
than patching around it. The clean fix (QEMU in its own process) is
designed and costed.
