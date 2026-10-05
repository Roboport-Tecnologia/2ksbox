# Track M13: gamepads

A real gamepad reaches a guest three ways: path A is a USB HID pad, path
B a gameport at 0x201 (the only one that reaches DOS), path C the pad
mapped onto keys (every guest). The track is done. A real PlayStation 5
DualSense has driven both guest devices (the Game Controllers panel on
XP and Windows 98 SE, `PADTEST.COM` in a Win98 DOS box), and each device
has a guest-stage check. There is no separate design doc, so this file
holds the design. Neighbours: doc 03 "Input path" (the grab model, which
a pad never touches), doc 07 (the form's row), `docs/development.md`
"Gamepads" (player flags, `PLAYER_PAD_SCRIPT`), doc 06 (per-family
defaults), `patches/qemu/README.md` (patches 26 and 27).

## Scope and files

- `gamepad/`: the abstract pad, a crate shared by the player and
  `launcher-core` (like `shader-chain`). `Control` (20 controls, face
  buttons named by *position* so a binding does not move between an Xbox
  pad and a DualShock), `Shaping` (deadzone rescale, press thresholds),
  `Binding`, `default_key_bindings`.
- `gamepad/qemu/dev-gamepad.c` + `usb-gamepad.h`: `usb-gamepad`,
  overlaid into `hw/usb/`, built by patch 26 under `CONFIG_USB_HID`.
- `gamepad/qemu/gameport.{c,h}`: `gameport`, overlaid into `hw/input/`,
  built by patch 27 under `CONFIG_GAMEPORT`.
- `player-core/src/pad.rs`: the host end (`gilrs` and the scripted source,
  shaping, the key map, `--pads`, `--pad-sweep`).
- `embed/libqemu_embed.h`: `qemu_embed_pad_state` /
  `qemu_embed_pad_present` (embed API v8).
- `launcher-core/src/bundle.rs` (`bundle::Pad`, `pad_choices`,
  `default_pad`), `wizard.rs` (`choose_pad`, `pad_notes`,
  `pad_warning`), `player.rs` (`pad_args`).
- Guest probes: `guest-tools/src/padtest.asm` (`TESTS\PADTEST.COM`),
  `guest-tools/src/padwin.c` (`TESTS\PADWIN.EXE`).
- Tests: the `pad` check, `tools/hid-descriptor-check.py`,
  `tools/pad-guest-test.py`.
- `packaging/flatpak/com._2ksbox.Launcher.yml` grants `--device=input`.

## The design

### The host end

`gilrs` reads the pad on every host (evdev, XInput/DirectInput,
IOKit/GameController), with hot-plug. `PLAYER_PAD_SCRIPT` replays a
scripted pad on the guest's frame clock, which makes every path testable
on machines with no controller. The player polls on the **UI thread**,
once per published guest frame, because on macOS a HID source wants the
process's run loop and the QEMU thread has none. `player --pads` reports
what this host can read, and tells "no controller" from "no permission"
in a sandbox.

The choices (device, bindings) belong to launcher-core (ADR-014), but
the shared model is its own crate: the player cannot depend on
launcher-core without carrying the machine library.

A control never seen counts as **centred, not unknown**; otherwise a
resting stick's first report is a spurious change.

### Path A, `usb-gamepad`

Two analog sticks (X/Y, Z/Rz), an 8-way hat **with a null state**
(without it a released hat reads north) and twelve buttons in a six-byte
report, on the `-usb` controller every seamless-mouse machine already
has. It is its own file, not a fourth kind in `dev-hid.c`, because every
machine's `usb-tablet` runs on that code.

- **Absolute state, not events.** One call carries the whole pad, so the
  next update corrects a dropped one. The device NAKs the interrupt
  endpoint while nothing changed, so an idle pad never wakes the guest.
- **Fed out of band.** The embed shim calls `usb_gamepad_set_state()`
  from the input bottom half under the BQL. A joystick event class in
  QEMU's input core was the wrong model (that core is built around
  consoles, and a pad has none). The cost: the device is not
  upstreamable as is; the event class is the upstream version.
- Realize refuses a second `usb-gamepad`. The host drives one pad.
- `-usb` comes with the pad even when the seamless mouse is off.

| Guest | What it needs |
|---|---|
| XP | nothing; inbox `hidusb.sys` binds on first start |
| Win98 SE | its own HID driver; the New Hardware wizard asks for the Windows 98 source files the first time (the CD or the CAB folder) |
| Windows Me | untried |
| Win98 FE | untried; its USB stack may want the USB supplement |
| DOS | no USB stack, use path B |

### Path B, the gameport

Four RC one-shots and four buttons. A write to 0x201 starts the timers.
A read returns bits 0–3 set while each axis is charging and bits 4–7
clear while a button is held. Pulse width is `24.2 µs + 0.011 µs × R`
over a 0–100 kΩ pot, so an axis byte of 0x00 is 24 µs and 0xff 1124 µs.

- **No timer, and exact.** The write records
  `qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)` and one deadline per axis; a
  read compares.
- A standalone ISA device at 0x200–0x207 (every card with a port decoded
  all eight), not part of a sound card, so it does not come and go with
  `bundle::Sound`.
- **The d-pad drives the first stick's axes to their ends**
  (`gameport_set_state`; the stick wins while the d-pad is centred). A
  Gravis GamePad has no pots, so a DOS game can read a d-pad no other
  way.
- Buttons 1–4 are the four face buttons in `gamepad::HID_BUTTONS` order,
  the same numbers as path A. There is no room for the rest or a hat.
- The same `qemu_embed_pad_state` feeds it; the shim calls both devices'
  entry points and the absent one returns at once.
- An idle port reads **0xf0**. An absent one reads 0xff off the open
  bus, which is how a game decides there is no joystick.

**Pacing is what makes the counts mean anything.** A game counts loop
iterations until a bit clears. On the DOS family's `-icount
shift=7,align=on` (doc 06, "Why a rate control, and what it costs") a
stick's ends and centre count 12 / 265 / 571 against true pulse ratios
of 1 : 23.8 : 46.4, steady to ~6 %. Unpaced, the counts are ten times
higher and an untouched axis spans 2.25x, so a game with a fixed timeout
sees a stick jammed at one end.

**Win9x gets no gameport driver, by decision.** The port is not Plug and
Play, so a Windows game would need "Standard Game Port" through Add New
Hardware plus calibration. On 98 the USB pad already reaches both APIs
such a game calls, sample for sample: DirectInput, and winmm
(`joyGetPosEx` over VJOYD, whose generic "Microsoft PC-joystick driver"
exposes the HID pad). The port stays offered on Win98 for DOS boxes,
which read 0x201 directly. XP, Windows 7 and 11 and Other are not
offered it (nothing enumerates a non-PnP port there).

### Path C, keys

`KeyMap` in `player-core/src/pad.rs` turns the pad into the key presses the
player already sends (d-pad and left stick are the arrows by default).
Every poll it recomputes the wanted scancode set and diffs it against
what is held, so no key can stick down in the guest, and a set union
handles keys with two holders. Releases go before presses in one
`input_flush`, so a stick swung across centre never leaves both arrows
down. Focus loss lifts keys through the map. There is no analog, and a
game that asks for a joystick still finds none.

### The launcher model

`bundle::Pad` is `none` / `usb` / `gameport` / `keys`, one per machine.
`pad_choices` lists what a family offers; the first is the default.
**`none` is first everywhere**, so a machine nobody asked for a pad on
grows no device in its Device Manager.

| Family | Offered |
|---|---|
| XP, Windows 7, Windows 11, Other | `none`, `usb`, `keys` |
| Win98 | `none`, `usb`, `gameport`, `keys` |
| DOS | `none`, `gameport`, `keys` |

The form refuses a device the family cannot use, and a `pad` value no
build knows falls back. It warns per device: Windows finds a USB pad by
itself, the gameport needs Add New Hardware and calibration.
`launcher_core::player::pad_args` writes `--pad <name>`.

## Tests

`pad` (host stage) runs the model against a real QEMU per family, reads
the port through the monitor, runs `player --pad-sweep` for the key map
and `tools/hid-descriptor-check.py`. `pad-guest` reads the gameport from
FreeDOS with `PADTEST.COM`; `pad-guest-xp` and `pad-guest-98` run
`PADWIN.EXE`, whose DirectInput and winmm columns must agree.
`pad-guest-98` needs a launcher machine whose Windows has bound the pad
once (`WIN98_PAD_MACHINE`, default `claude98`) and skips without one.
All run the player, so they skip without a display. Commands and knobs:
`docs/testing.md`.

## Traps

- **No `usb-tablet` beside the pad on Win98.** With it, DirectInput
  enumerates no joystick (same image, A/B; XP does not care; mechanism
  unknown). `pad-guest-test.py` passes `-usb -device usb-gamepad` only.
- **PADWIN, not PADTEST.** DOS and cmd both resolve a bare name to the
  `.COM` first.
- The Flatpak needs `--device=input` (`--device=dri` does not cover
  `/dev/input`). Check with `player --pads` inside the sandbox.
- `gilrs` links libudev on Linux (the Flatpak's runtime carries it). A host without it builds `--no-default-features` and gets the
  scripted pad only.
- `gilrs` on Windows is XInput-first (four pads); a DualShock needs its
  DirectInput fallback.
- On macOS, USB HID pads need no entitlement under the hardened runtime.
  A Bluetooth controller may prompt.
- Patches 26/27 and the embed API move together; after pulling, rebuild
  the embed library before the player links.

## What stayed open

- **Path C with a real controller.** Only `PLAYER_PAD_SCRIPT` has driven
  the key map; its guest test was dropped by decision (more harness than
  the path is worth). `--pad-sweep` covers the map, shared keys,
  ordering and scancodes. A wrong binding there looks like a broken
  game, not a broken pad.
- Windows 98 first edition and Windows Me with the USB pad are untried.
