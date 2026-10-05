# 20. Music: FM, General MIDI and the MT-32 (M12)

Sound effects are digitised samples through the sound card's DMA
channel, which QEMU's `sb16` and AC'97 have always given a guest (into
the player through the `embed` audiodev, patch 20). *Music* is a score
played on a synthesizer: FM registers on the OPL chip, MIDI bytes out of
an MPU-401, or GF1 voices on a Gravis Ultrasound. Upstream QEMU's `sb16`
carries no OPL (a real SB16 has a YMF262 at 0x388), its separate `adlib`
is an OPL**2** on the old MAME core, and it has **no MPU-401 at all**,
so a game that asks a Sound Blaster for music, or offers "Roland MT-32",
plays to nobody.

This doc covers the music devices and synthesizers we add, and the
fixes to QEMU's SB16 the same work turned up. Each family's audio
hardware is in doc 06, the track's state in `docs/tracks/m12-music.md`.

| The guest sees | Played by | Needs |
|---|---|---|
| an OPL3 at 0x388 (and at the SB base) | Nuked-OPL3, the cycle-accurate YMF262 | nothing |
| an MPU-401 UART at 0x330 | a SoundFont General MIDI synthesizer | a `.sf2` bank (one ships) |
| the same MPU-401 | a Roland CM-32L (the MT-32 family) | the user's own Roland ROMs |
| a Gravis Ultrasound at 0x240 | QEMU's own `gus` | the guest's GUS drivers |

## 1. Shape

```
guest port writes ─► hw/audio/opl3.c   ─┐
                                        ├─► libsynth (Rust staticlib) ─► AUD voice ─► QEMU mixer ─► embed audiodev ─► player
guest MIDI bytes ─► hw/audio/mpu401.c ─┘                                                   ▲
                                                                     sb16 / AC97 / gus ────┘
```

**The engines live inside QEMU, not in the player.** `libsynth/` is a
Rust staticlib with a C API, linked into each `qemu-system-<target>`
and `libqemu-embed-<target>` with an ISA bus and driven by two device models, as `libdisc` is
(doc 17 §5.2, patch 50). A synthesizer in the player, fed MIDI bytes
through the embed API, was rejected for three reasons:

1. **One clock.** Music goes through QEMU's mixer on the same virtual
   clock as the SB16's DMA, under one `embed` audiodev cushion. In the
   player it would have a second clock and buffer, and every stall would
   slide one against the other.
2. **Testable headless.** `-audiodev wav` captures what the guest plays
   with no host audio device.
3. **No new libraries.** All three engines are pure Rust (`nuked-opl3`,
   `rustysynth`, `moont`). FluidSynth (glib) or Munt (C++, CMake) would
   add to what the macOS app and the Flatpak carry.

A **host MIDI port** (a real MT-32, an SC-55 or the OS's synth) is left
out: it is a host device, so it belongs on the player's side of the
embed API, and no headless test can check it (§8).

## 2. The engines, and why these

| Engine | Crate | Licence | Why |
|---|---|---|---|
| OPL3 | `nuked-opl3` | LGPL-2.1+ | The Nuked core is a register-level model of the YMF262 from a die scan, the core DOS emulators have moved to. Its `Opl3Device` layer carries the two timers and the status register, which QEMU's `adlib` lacks and every AdLib *detection* routine reads. |
| General MIDI | `rustysynth` | MIT | A SoundFont 2 synthesizer in pure Rust. It renders at any rate and allocates nothing on the render path after the first block. |
| MT-32 / CM-32L | `moont` | LGPL-2.1+ | A Rust port of Munt that claims, and tests, sample accuracy against it. The alternative was Munt as a C++ submodule with a build stage per platform. |

All three are compatible with the workspace's `GPL-2.0-only` (ADR-009).

## 3. The C API

`libsynth/libsynth.h` is the one header, shared by the crate
(`libsynth/src/capi.rs`) and both devices. Both devices check
`LIBSYNTH_API_VERSION` at realize; without it a stale staticlib is a
guest that plays nothing and logs nothing.

The two handle types are **unlocked by design**: QEMU drives both sides
with the BQL held (port writes on the vCPU thread, rendering from the
audio timer in the main loop). Nothing blocks or allocates on the render
path, and every entry point catches a panic, so a synthesizer that falls
over costs the guest its music, never QEMU its process.

The MIDI half takes **the byte stream one byte at a time, as the guest
wrote it**: running status, System Exclusive split across any number of
writes, real-time bytes in the middle of either. `libsynth/src/midi.rs`
is that state machine, shared by both synthesizers; the MPU-401 device
knows only that a byte arrived.

## 4. The bank, and the ROMs

**The bank decides General MIDI's sound** more than the engine does.
One ships, `soundfonts/TimGM6mb.sf2` (5.7 MB, GPL-2, the bank MuseScore
1.x shipped and Debian packages). `soundfonts/README.md` has its provenance and
hash, and says why we don't ship the better-sounding GeneralUser GS: its
author does not vouch for every sample's origin, which is fine for a
bank a user picks but not for one in a GPL package. The machine form
lets the user pick any `.sf2` instead.

**The bundle does not name the bank** unless the user chose one. A
machine says `synth=gm`, and the device finds the file by the rule every
companion of ours uses (the Direct3D executor's, doc 14): the `soundfont=`
property, then `LIBSYNTH_SF2` (a packaged player sets it in
`player-core/src/companions.rs`), then `soundfonts/TimGM6mb.sf2` in a
checkout. One machine file works in a checkout and in any package.

**The MT-32 ROMs are the user's own**, and nothing of Roland's is ever
added here. An MT-32 is a sampler: the LA synthesis is emulated, but the
sounds are a 64 KiB control ROM and a 1 MiB PCM ROM, neither
redistributable. The device takes a *directory* (`romdir=`) and finds
the two images **by size, not name**, because every dump names them
differently (`CM32L_CONTROL.ROM`, `cm32l_ctrl.rom`,
`ctrl_cm32l_1_02.rom`…). Without them the machine refuses to start and
says what is missing; it never boots silent.

**It is a CM-32L, which decides which dump works.** `moont` emulates the
CM-32L, the MT-32's superset with 33 extra PCM samples (what a CM-64 or
LAPC-I has inside). An MT-32 game plays on it, but an *original*
MT-32's 512 KiB PCM ROM is refused with a sentence saying so.
Supporting it would mean Munt itself, the trade §2 declined.

Nobody on the project has CM-32L ROMs, so the engine is unverified here.
Someone with a dump runs `synthx selftest --roms <dir>`.

## 5. The devices

Both are ours. `scripts/prepare-qemu.sh` overlays them from
`libsynth/qemu/` into `hw/audio/`, and patch 60 wires them into
meson/Kconfig.

### `-device opl3`

An ISA device with the YMF262's register pair at **0x388–0x38B** and,
with `sbbase=`, the mirror a Sound Blaster has at **2x0–2x3**, where an
SB-aware game looks. That mirror is why this is not QEMU's `adlib` with
a better core. Address and data are the chip's two register banks (bank
0 is the OPL2-compatible one). Reads return the status register, where
the two timers show. Every AdLib driver detects the chip by writing
timer 1, waiting and reading the flag back, so a chip whose timers do
not tick is one no game finds. The voice opens at the chip's own 49716
Hz (`freq=`) and QEMU's mixer does the one conversion.

**The SB16's mixer scales it** (patch 61), as on a real board where the
chip's output ran into the card's. Master × FM volume (0x30/0x31 ×
0x34/0x35, or the SB Pro's 0x22 × 0x26) reaches the voice through the
audio core's mixer-input registry, which the chip attaches to at
realize. So Windows' Volume Control "MIDI" slider works on FM music,
and a bare AdLib (no card, no mixer) plays at unity. CD audio is the
other input, under the card's CD volume and output switch. Upstream
QEMU stored these registers and applied none.

### `-device mpu401`

An ISA device at **0x330–0x331**: a data port, and a status/command port
whose two flags are "ready for a byte" and "a byte is waiting". UART
mode (command `0x3F`) is what everything of the era uses, DOS games and
Windows 9x's MPU-401 driver alike. Reset (`0xFF`) answers `0xFE` as the
hardware does, which is how a game decides the port exists. There is no
MIDI *in*, so the only byte the guest ever reads is that ACK.
Properties: `synth=gm|mt32` picks the engine, `soundfont=` / `romdir=`
feed it, `gain=` (percent, 100) trims it against the sound card, and
`irq=` is §5.1.

### 5.1 And no interrupt, by measurement

The device has **no interrupt line unless `irq=<0-15>` asks for one**.
A real MPU-401 uses IRQ 2/9, but QEMU's PIIX4 puts the **ACPI SCI on IRQ
9** (`hw/acpi/piix4.c`), and every Win98 the launcher installs is an
ACPI install (doc 06). With the line on IRQ 9, the ACK a driver's reset
queued became an interrupt no handler acknowledged. The line stays high
until the data port is read, as on the hardware, so the guest's handler
was re-entered on every `IRET` until the ring-0 stack ran out:

    234 × Servicing hardware INT=0x59      (slave PIC base 0x58 + 1 = IRQ 9)
    SP=0030:d444c248 … d444c040 … d444c000  (0x68 of ring-0 stack per entry)
    check_exception old: 0xffffffff new 0xe  #PF at CR2=d444bffc — off the end
    check_exception old: 0xe new 0xe         #DF
    check_exception old: 0x8 new 0xe         Triple fault

The user sees **Windows 98 reboot on its own** (Duke Nukem 3D's
`SETUP.EXE` in a DOS box: Choose Music Card, General Midi, 0x330, Test
Music Card). DOS never shows it, because it leaves IRQ 9 masked.

Nothing is lost without the line: it exists for MIDI in, which this
device lacks, and every driver of the period polls for the ACK. The
`music` check writes the reset and requires the slave PIC to have
nothing pending afterwards.

### 5.2 And the Sound Blaster's, which has to be acknowledgeable

QEMU's `sb16` asserted IRQ 5 in two places where nothing a driver reads
could lower it again (patch 25). A Sound Blaster holds its line until
the DSP status port is read, which QEMU models by clearing bit 0 or 1
of mixer register 0x82 on a read of 0x2xE / 0x2xF, so an assertion with
**no bit set** holds the line for good. On the edge-triggered ISA PIC
every later completion is then a level 1 into an already-high line, and
the card stays deaf until the next DSP reset. The two sites:

- **`reset()` pulsed the line while auto-init DMA ran**, an interrupt
  no hardware makes. A guest resetting the DSP has IRQ 5 masked, so the
  edge sat in the master PIC's IRR with no owner, and Windows' VPICD
  will not unmask a physical IRQ in that state.
- **The end of a silence block** (DSP 0x80, timer and short-block paths)
  raised the line without the status bit. A reset now also cancels a
  pending silence block, which would otherwise fire behind the guest's
  back.

In Duke Nukem 3D's `SETUP.EXE` on Win98 (Test Sound FX Card) the test
played once, then every press said *"Playback failed, possibly due to
an invalid or conflicting IRQ"*, with `info pic` showing `pic0 irr=20
imr=b8` (IRQ 5 latched and masked) while mixer 0x82 and the status port
read 0x00. The `sb16-irq` check
counts rising edges of IRQ 5 with `info irq`: a DSP reset must add none
and each silence block exactly one.

### 5.3 And its name, which DirectX 9 cannot take in Portuguese

On a Portuguese Win98 (`claude98`) `dxdiag` dies with an illegal
operation and DirectSound games do not run. The cause is **the guest's
language**, not the host, the card or QEMU (the same fault on `embed`,
on `none` and on a bare `qemu-system-i386`), and a real Portuguese Win98
with a real SB16 and DirectX 9 fails the same way.

The fault, `exceção c0000409H no módulo DSOUND.DLL`, is the `/GS`
cookie check (`STATUS_STACK_BUFFER_OVERRUN`) in DirectX 9.0c's
DSOUND.DLL (4.09.0000.0904). It copies a wave device's `szPname` with a
byte loop into a 32-byte buffer with the cookie behind it, and a caps
name is a fixed 32 bytes with no NUL required. `SB16.VXD` builds each
name as `"%s [%x]"`, and the Portuguese wave-in name is 33 characters,
cut to 32 with no terminator (English says `SB16 Wave In [220]`).
`TESTS\WAVECAPS.EXE` shows it:

    waveOut 0: "Saída de som wave da SB16 [220]"  nul_at=31
    waveIn  0: "Entrada de som wave da SB16 [220"  nul_at=-1

**The VxD can override the name.** At start it reads `WaveInDevName`,
`WaveOutDevName` (and the MIDI, mixer, aux and DirectSound names) from
`HKLM\SOFTWARE\Creative Tech\DeviceInfo\<enumerator>\<hardware ID>`
(the device ID's first component, then `HardwareID` without its `*`, cut
at the first `,`) and uses them in place of its own strings, port still
appended. Windows detects QEMU's `sb16` as `ROOT\*PNPB003\0000`, so the
key is `DeviceInfo\ROOT\PNPB003`.

The fix is **SETUP's "Sound Blaster 16 device names"** component (`SETUP
/I 5`, 9x only, `guest-tools/README.md`). For each Creative wave device
whose name does not end within 32 bytes, it writes a shorter one to the
key of every devnode driven by `sb16.vxd`: "wave" dropped ("Entrada de
som da SB16", 28 with the port), else cut at a word to 25 characters.
Then it asks for the restart the VxD needs; a machine whose names fit
is told "nothing to do". After the restart WAVECAPS reads `"Entrada de
som da SB16 [220]" nul_at=28` and dxdiag stays up. DirectSound games
run with it too (user, 2026-09-23).
`tools/setup-guest-test.sh` requires the component's line on Win98.

## 6. What a machine offers

The machine form (doc 07) has two pickers, both `launcher-core`'s
(ADR-014). As with the display adapter, a family offers what it has a
real question about, and **the first entry is its default**
(`bundle::sound_choices` / `music_choices`).

**Sound card** is the digital audio device, `bundle::Sound`:

| Family | Offers | Default |
|---|---|---|
| Win98 | SB16 (with the OPL3), Gravis Ultrasound, none | SB16: Windows has the driver in the box, and a DOS box inside 98 finds the card it expects |
| DOS | SB16, Gravis Ultrasound, AdLib only, none | SB16 |
| XP | AC'97, SB16, none | AC'97 |
| Other | ES1370, AC'97, none | ES1370: BeOS and a period Linux both drive it out of the box (doc 06) |
| Windows 7, Windows 11 | HD Audio, none | HD Audio: their in-box driver (doc 06) |

**Music** is what sits behind the MPU-401, `bundle::Music`:

| Entry | What it is |
|---|---|
| General MIDI (SoundFont) | the shipped bank, or the user's own |
| Roland MT-32 / CM-32L | the user's ROMs |
| None | no MPU-401 at all. A port that swallowed notes would be one a game picks and plays to nobody |

**Win98 and DOS start on General MIDI, XP and Other on None.** The first
two have no synthesizer of their own (98's MIDI output is the FM chip).
XP ships a wavetable synthesizer, and Other is the family we add no
drivers to; both have the port one pick away. Windows 7 and Windows 11
are offered None only: each has its own synthesizer, and the ISA port
is not tried there.

**The FM chip is in neither picker.** SB16 or AdLib puts an OPL3 on the
machine; AC'97, the ES1370 or HD Audio does not.

**Changing either is a hardware change** to an installed guest, and the
form says so in the display adapter's orange: Windows re-detects the
card, and a DOS game's setup must run again.

**Not offered: an Ensoniq AudioPCI beside the SB16 on 98** (tried and
removed). Windows 98 has no ES1370 driver in the box, and the AudioPCI
packages tried by hand did not make the card work.

The guest side is the user's (doc 06). A DOS box wants
`BLASTER=A220 I5 D1 H5 P330 T6` for the MPU-401 to be found, and an
`ULTRASND` line matching QEMU's `gus` (port 0x240, IRQ 7, DMA 3) plus
Gravis's drivers. **Windows 98 finds the SB16 itself, but
plays MIDI to the port only after "MPU-401 Compatible" is added by hand
from Add New Hardware.**

## 7. Tests

The checks are `libsynth` (`synthx selftest`, the three engines through
the devices' C API; the CM-32L with `--roms`), `music` (the pickers to a
real QEMU, the devices sounding into QEMU's wav, and the MPU-401 reset
raising **no** interrupt, §5.1), `sb-mixer` (patch 61), `sb16-irq`
(§5.2, patch 25), `midi-guest` (a DOS guest plays on both devices) and
`tools/duke-guest-test.py` (Duke Nukem 3D's score on our MPU-401, ~300–450
note-ons per 5 s on 5–8 channels; local only, it needs the user's disc).
`docs/testing.md` says what each asserts and how to run it.

### 7.1 What the devices say about themselves

Both print a line every 5 s while the guest drives them, and nothing
when it does not:

    opl3: 2466 register writes, 516 key-ons in 5.0 s
    mpu401: 1245 bytes, 415 note-ons on 6 channels in 5.0 s

Check these first for a silent game: no line means the game never wrote
to the port (its setup names another device, or it found nothing). Duke
Nukem 3D's *Sound Blaster* music entry writes nothing; its *AdLib*
entry, which probes 0x388, fills the log.

### 7.2 When the music plays and then goes wrong

"The music started correctly and then some instruments stopped" has two
causes that sound the same: the guest stopped sending those notes, or we
stopped playing them. Both devices keep a capture that replays with no
guest:

    LIBSYNTH_MIDI_LOG=/tmp/win98-midi.log   # the MIDI port's byte stream
    LIBSYNTH_OPL_LOG=/tmp/win98-fm.log      # the FM chip's register writes

Both go in the **player's** environment (QEMU is in its process) and can
be on together. Every write carries a microsecond stamp on the host's
clock, the timeline the music was heard on, since the engines render on
the host's audio callback. A MIDI capture holds the bytes and an `R`
for each port reset, an OPL one `<file>:<address>:<value>`. Then:

    target/release/synthx midilog /tmp/win98-midi.log     # what the guest sent
    target/release/synthx opllog  /tmp/win98-fm.log       # ... to the chip
    target/release/synthx play    /tmp/win98-midi.log x.wav   # what we make of it

Each verb reads either capture; the file's first line names the device.

- **`midilog`** prints a row per channel and a column per second of
  note-ons. A row that stops while the others go on was stopped by the
  guest; a row that goes on while the sound does not is our bug. Below
  the table it lists:
  - what silenced a channel from outside (volume or expression to zero,
    all-notes-off, all-sound-off) and when;
  - notes left held at the end (a missing note-off holds one of the
    engine's `gm::POLYPHONY` = 64 voices for ever);
  - the **most notes down at once**, which measures the commonest
    thinning: past the voice count each new note steals one still
    sounding;
  - bytes the parser could attach to nothing.
- **`opllog`** asks the same in the chip's units. The chip has no
  instruments or note-offs, so a muted instrument is a row that stops
  keying on, and running out of voices is channels left keyed on against
  the chip's 18.
- **`play`** renders the capture through the same engine at its original
  timing. If the music breaks there, the bug is ours and the capture
  reproduces it; if not, look at the guest.

**When a guest's two hardware synthesizers misbehave together and its
software one does not, the engines are not the suspect**, because they
share no synthesis code.

## 8. Not here (and the order to add it)

1. **A host MIDI port**, sending the stream to real hardware or the
   host's synth (CoreMIDI / ALSA / WinMM through `midir`), for someone
   who owns a module. It needs player-side code, an embed API bump for
   the byte sink, and a port list in the machine form.
2. **MIDI in.** Nothing of the era needs it. The IRQ and the status
   register's second flag exist for it; adding it is a queue and a
   `qemu_irq`.
3. **A wavetable header on the sound card** (the SB16's daughterboard
   connector) instead of a separate MPU-401, only if a game insists.
4. **The MT-32 through moont's GM mapping** (`GmDevice`), for General
   MIDI music on MT-32 timbres, which a few 1994 titles assume.
