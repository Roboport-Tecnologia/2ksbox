# Track M12: music (doc 20)

What a guest plays *music* on: the OPL3 and MPU-401 devices, the three
engines behind them (`libsynth/`), and the machine form's sound-card and
music pickers. The design is doc 20; each family's audio hardware is in
doc 06's Audio rows. Work happens on `main`.

## State

Every planned stage has landed. Left: an open bug in Win98 and optional
work (doc 20 §8).

- **Engines** (doc 20 §1–§3): `nuked-opl3`, `rustysynth` (General MIDI
  from a SoundFont) and `moont` (a CM-32L), pure Rust, linked into QEMU
  as a staticlib the way `libdisc` is.
- **Devices** (patch 60): `-device opl3[,sbbase=]` and
  `-device mpu401,synth=gm|mt32`. The MPU-401 has no interrupt line unless
  `irq=` asks for one; its hardware IRQ 9 is PIIX4's ACPI SCI, where a
  queued ACK rebooted Win98 (doc 20 §5.1).
- **Pickers** (doc 20 §6): `bundle::Sound` / `bundle::Music` in
  `launcher-core`, the Qt form, the C API (`lc_wizard_sound_*`,
  `lc_wizard_music_*`) and `launcherx --music`.
- **Packaging.** Every package carries `soundfonts/TimGM6mb.sf2`, passed as
  `LIBSYNTH_SF2` by `player-core/src/companions.rs`.
- **QEMU's own SB16.** An interrupt line a driver can acknowledge (patch
  25, §5.2), mixer volumes applied (patch 61), wave-device names DirectX 9
  can take (`SETUP /I 5`, §5.3).
- **In guests.** Duke Nukem 3D plays its score through the MPU-401 and the
  OPL3 (`tools/duke-guest-test.py`). Win98 plays MIDI through the port
  once "MPU-401 Compatible" is added from Add New Hardware, but loses
  instruments (next steps, item 1).
- **The MT-32 is unverified, by decision.** Nobody here has CM-32L ROMs
  and the user will not get any. The option, the form's refusal without a
  ROM directory, the ROM finder and its message for an original MT-32's
  half-size PCM ROM are proved; `moont` is taken at its word. Someone with
  a dump runs `synthx selftest --roms <dir>` first if a title sounds wrong.

## Scope and files

- `libsynth/`: `src/opl.rs`, `src/gm.rs`, `src/mt32.rs`, `src/midi.rs`
  (the byte-stream parser), `src/capi.rs`, `libsynth.h`,
  `src/bin/synthx.rs`.
- `libsynth/qemu/opl3.c` and `mpu401.c`, overlaid into `hw/audio/` by
  `scripts/prepare-qemu.sh`; `patches/qemu/60-opl3-mpu401-devices.patch`
  wires them into meson/Kconfig with `-Dlibsynth_dir`.
- `soundfonts/`: the shipped bank and its provenance.
- `launcher-core/src/bundle.rs` (`Sound`, `Music`, their `*_choices`,
  defaults and arguments) and the form's two rows in `wizard.rs`.
- Checks: `libsynth`, `music`, `sb-mixer`, `sb16-irq` (host) and
  `midi-guest` (guest) in `scripts/test.sh`; `tools/midi-guest-test.py`,
  `tools/duke-guest-test.py`.

## Build and test loop

```sh
cargo build --release -p libsynth          # synthx; configure-qemu.sh links the staticlib into QEMU
target/release/synthx selftest build/synth # the engines alone (the libsynth check)
scripts/test.sh host                       # libsynth, music, sb-mixer, sb16-irq
tools/midi-guest-test.py                   # both devices from a DOS guest, judged by QEMU's wav
tools/duke-guest-test.py                   # a real game, off the user's own disc (local only)
```

For music that starts right and goes wrong, capture the guest's writes
with `LIBSYNTH_MIDI_LOG=<file>` / `LIBSYNTH_OPL_LOG=<file>` in the player's
environment and replay them with `synthx midilog`, `opllog` and `play`
(doc 20 §7.2). Both devices log what the guest did to them every 5 s
(§7.1). Every tool is in `docs/testing.md`.

## Rules

- The engines never learn about QEMU and the devices never learn about
  MIDI: the parser is `libsynth`'s, the ports are the devices'. A change
  that needs both changes the C API and bumps `LIBSYNTH_API_VERSION`.
- `soundfonts/TimGM6mb.sf2` is a default, not a fixture. The `libsynth`
  check plays the shipped file so a package with a broken bank fails.
- Nothing of Roland's is ever added to this repository.

## Next steps

1. **Win98's own MIDI loses instruments.** Both our synths lose
   instruments under dxdiag's music test while Microsoft's software synth
   does not, so the engines are not the first suspect (doc 20 §7.2).
   Capture one run with both logs set and read it with `synthx`.
2. **"MPU-401 Compatible" from Add New Hardware**, the step Win98 needs
   before it plays MIDI to the port (`docs/00-status.md` "Next steps").
3. **A host MIDI port** (doc 20 §8.1): send the stream to real hardware
   or the host's synth, player-side, with an embed API bump.
4. **Duke's FM music from a batch file**, if anyone cares. With
   `MusicDevice = 2` it says "Couldn't find selected sound card" unless
   its own `SETUP.EXE` starts the game; possibly QEMU's `sb16`, not our
   OPL3.
