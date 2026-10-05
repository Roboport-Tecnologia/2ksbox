# Track M5: the CD-ROM backend (docs 05 and 17)

Raw optical-drive emulation: the `libdisc` Rust crate (disc model, image
formats, EDC/ECC, subchannel, MMC responders), the `cdimage` QEMU block
driver over its C API, and the ATAPI patch that serves TOC, raw sectors,
subchannel and CD-DA from the model. The work ran 2026-09-04 to 2026-09-09
and is all on `main`; later fixes land there under `M5:`. This doc keeps scope, test loop, traps and open
items. The design is elsewhere:

- Doc 05: the problem statement and the acceptance table, one row per
  protection.
- Doc 17, the spec: byte layouts and formats; the L-EC rule (EDC decides,
  parity repairs, §2.5); subchannel synthesis and the undeclared-pregap
  decision (§2.6); the negative control and what the protections read
  (§2.6b–c); which dumps carry a protection signal (§6.x).
- `docs/tracks/m5-dirdisc.md`: M5g, a host folder as a disc.
- Doc 07: the disc shelf (patch 52, M6).

## Scope and files

- `libdisc/`:
  - `src/`: the model in `lib.rs`; `cue.rs`, `ccd.rs`, `mds.rs`, `iso.rs`,
    `sector.rs`, `ecc.rs`, `subq.rs`, `msf.rs`, `mmc.rs`, `capi.rs`;
  - `src/bin/discx.rs`;
  - the C header `libdisc/libdisc.h` (bump `LIBDISC_API_VERSION` on any
    change);
  - the block driver `libdisc/qemu/cdimage.{c,h}`, overlaid into the QEMU
    tree by `prepare-qemu.sh`.
- QEMU patches (rows in `patches/qemu/README.md`): `50-cdimage-block-driver`
  (meson, `CONFIG_CDIMAGE`), `51-atapi-disc-model` (`hw/ide/`),
  `53-atapi-dvd-profile` (M5g), `54-atapi-audio-seek-stop` (Win9x's
  seek-as-stop, doc 17 §5.4), `55-atapi-audio-read-error`,
  `56-atapi-medium-type` (MODE SENSE's medium type, for DOS CD drivers).
  Numbers 57–59 are reserved for this track; patch 52, the shelf, is
  shared with M6.
- Guest programs: `guest-tools/src/cdtest.c` (`CDTEST.EXE`, MCI CD audio)
  and `cdshelf.c` (`CDSHELF`, the shelf inside the guest).
- Tools: `tools/atapi-guest-test.py`, `tools/cd-rate-guest-test.py`,
  `tools/xp-cdimage-test.sh`, `tools/cdaudio-guest-test.sh`,
  `tools/cdshelf-guest-test.sh`, `tools/read-error-inject.c`.
- Shared: `scripts/prepare-qemu.sh` and `configure-qemu.sh` (the overlay
  and `-Dlibdisc_dir`), `scripts/test.sh`, and the `-drive` /
  `ide-cd,audiodev=` lines in `launcher-core/src/bundle.rs`.

## Test loop

```sh
scripts/build-libdisc.sh                    # cargo + relink QEMU (see traps)
target/release/discx selftest build/test/disc
scripts/test.sh all                         # before every commit
```

| Check | What it runs |
|---|---|
| `libdisc` | `discx selftest`, through the C API |
| `cdimage` | `qemu-img` / `qemu-io` on the selftest images |
| `atapi-guest` | a DOS program driving the drive by PIO: every reply equals `discx dump`, an unreadable and a repairable sector, audio, the shelf |
| `atapi-read-error` | unreadable audio sectors play as silence (Linux) |
| `guest-cdimage` | XP copies a converted disc through cdrom.sys; `CDTEST.EXE` plays the tone into the drive's wav |

Local-only tools (`docs/testing.md`): `tools/cd-rate-guest-test.py` (what
is read does not depend on the read rate), `tools/cdaudio-guest-test.sh`
(how each Windows family stops a drive), `tools/cdshelf-guest-test.sh`.

`discx scan`, `subscan` and `repair` diagnose a real dump (doc 17 §6.1).
`CDIMAGE_TRACE=1` prints every packet, reply and sense on the disc path;
`LIBDISC_NO_CORRECT=1` turns off L-EC correction for an A/B.

The real dumps are not in the repo. On the Linux box they are in
`/mnt/data2/david/Downloads/oldstuff`, with the `discx repair` control
copies under its `clean/`.

## Traps

- **meson does not track `liblibdisc.a`.** After a change under `libdisc/`
  a plain `ninja` keeps the old code in QEMU, `qemu-img` and the embed
  library; `scripts/build-libdisc.sh` (and `scripts/build.sh`) relink them.
- **A new PIO end-transfer function** goes into `hw/ide/core.c`'s
  `ide_is_pio_out` / `transfer_end_table`, or QEMU aborts on the first
  data word.
- **A plain `.iso` must keep probing to `raw`**: the XP guest stage and
  every recorded number use that path. The new IDEState fields have no
  vmstate, so a snapshot with a `cdimage` medium is unsupported.
- **Watch a check fail before believing it passes.** Three protections
  "passed" while the `discx repair` copies passed too. SafeDisc 2 and
  ProtectCD never read their band (doc 17 §2.6b); only Crimson Skies'
  SafeDisc 1.50.020 does (§2.6c).
- **Put a protected title's disc on its own IDE channel** (`ide.1`, as the
  player does). SafeDisc 2 refused FIFA 2002 as the boot disk's slave.
- **Check which device sent a sense reply.** QEMU's default *empty* CD
  drive answers `GET CONFIGURATION` with errors that look like ours.
- **Run FIFA 2002 from its `.mds`, not the DIC `.cue`.** The DIC dump has
  64 undescrambled sectors outside the band that break the install (doc 17
  §6.x).
- **A DOS program doing bus-master DMA must set PCI_COMMAND_MASTER**, or
  the engine reports a clean finish and writes nothing.

## What stayed open

- **Re-measure the protected dumps on the rig** (also a
  `docs/00-status.md` open thread). Since the EDC-first correction (doc 17
  §2.5), check that every L-EC failure on FIFA 2002, AoM disc 1 and
  Settlers 3 CD01 lands in `discx scan`'s *unreadable* column, and that
  FIFA 2002 still installs and reaches its menus.
- **Protections.** FIFA 2002 never reached a match (suspect: the display
  path, not the disc). Age of Mythology not yet run as a second SafeDisc
  2.x title. SecuROM needs DPM in `mds.rs` (M5e) and an owned dump.
  Multisession not done.
- **CHD (M5e).** v5 with `cdlz`/`cdzl` hunks needs zlib, LZMA and FLAC;
  pure-Rust decode versus `chdman` at import time is undecided.
- **Speed model.** The drive delivers as fast as the host reads, and
  QEMU's block throttle reaches only the raw driver. A title that paces
  itself on CD reads needs a `speed=` property on `ide-cd` that both
  drivers honour (measurements in `docs/00-status.md`).
- **Not recorded:** Win98's CD Player playing track 2 by ear in the
  player; a medium swap while audio plays.
