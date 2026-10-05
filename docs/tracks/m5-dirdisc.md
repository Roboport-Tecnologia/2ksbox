# Track M5g: a host directory as a CD-ROM (`isodir:`)

`isodir:/path/to/folder` serves a host directory as a read-only ISO 9660 +
Joliet volume that libdisc generates lazily: no image file, no copy, no
`xorriso` at run time. Opened and merged on 2026-09-06; a later step
raised the ceiling from a CD to a dual-layer DVD. This doc keeps scope,
test loop, traps and open items. The design:

- Doc 17 §8: volume layout, decisions, limits, model additions.
- Doc 17 §5.1: the `isodir` driver beside `cdimage`, and the `raw` node
  the block layer puts on top of it.
- Doc 07: the launcher side ("Add folder…", forced Insert/Eject).
- `docs/tracks/m5-cdrom-backend.md`: the M5 code this track extends.

## Scope and files

- `libdisc/src/isodir.rs`: the layout builder.
- `libdisc/src/lib.rs`: `Source::Mem`, `eof_pad`, lazy payload handles,
  `extent_at` as a binary search.
- `libdisc/src/msf.rs`: `Msf::from_lba` saturates.
- `libdisc/src/bin/discx.rs`: `selftest`'s `dirdisc` case, and `info` /
  `dump` / `convert` / `export` / `mktree` on a directory.
- `libdisc/qemu/cdimage.c`: the `isodir` BlockDriver, an overlay file of
  ours, so no QEMU patch.
- QEMU patch 53 (`atapi-dvd-profile`): above an 80-minute CD the medium
  reports a DVD-ROM profile. Patch 52's shelf strips the prefix before
  checking the host path.
- Launcher, shared with M6: `launcher-core/src/disc_library.rs`
  (`qemu_medium`), `bundle.rs` (comma doubling), `control.rs` (forced
  insert), `cli.rs` (`--discs publish`), and the disc shelf's "Add
  folder…" (then `launcher-qt/qml/DiscShelfWindow.qml`; since
  2026-10-02 the Add menu's "Folder as disc…" in
  `launcher-mitsuami/src/discs.rs`).
- `libdisc.h` did not change: `libdisc_open` on a directory is the whole
  interface, so `LIBDISC_API_VERSION` stays 1.

## Test loop

```sh
scripts/build-libdisc.sh                        # cargo + relink QEMU
target/release/discx selftest build/test/disc   # includes the dirdisc case
target/release/discx info isodir:<dir>          # layout, size, mangled names
build/qemu/qemu-img info isodir:<dir>           # names isodir
scripts/test.sh all
```

| Check | What it proves |
|---|---|
| `dirdisc` | xorriso reads the fixture tree back identical. bsdtar with Joliet off reads the 8.3 tree, including which of two colliding names has which contents. `qemu-img convert` equals `discx export`. A SeaBIOS probe with `CDIMAGE_TRACE=1` shows the ATAPI path found the model (a plain `.iso` is the control). |
| `guest-dirdisc` | XP copies the extracted guest-tools directory, served as a folder, through cdrom.sys; every file matches. |
| `dirshelf` | The launcher's headless verbs shelve three folders under their own names (one with a space, one with a comma, one plain). The shelf file and `--print-args` say `isodir:`, commas are doubled, and our QEMU opens both. |

Local tools:

- `tools/dirdisc-guest-test.sh <image> [win98|xp]`: a folder read with the
  guest's own `dir` and `type`. With `BIG=1`, marker files at 703 MiB,
  878 MiB, 2 GiB, 4 GiB and 7.8 GiB; Win98 and XP read all five.
- `tools/xp-cdimage-test.sh <image> isodir:<dir> <dir>`: a round trip.
- `tools/cdshelf-guest-test.sh`.

The harness traps (mtools' hidden-sectors field, XP's lazy writer, macOS
tool differences, NFD names from bsdtar) are in `docs/testing.md`.

## Traps

- **The `raw` node hides failures.** The block layer puts a `raw` format
  node above a protocol driver found by prefix, so `cdimage_disc()` must
  walk down through format nodes. If it does not, files still read and
  only the model's answers (TOC, READ CD, a bad sector's sense) go
  missing. The SeaBIOS probe in `dirdisc` catches it without a guest.
- **Everything that inspects a medium string must understand `isodir:`**,
  not only the code that passes it on. The shelf's C side once
  `access()`ed the prefixed path and called every folder missing.
- **Validate where the disc is built**, so the arithmetic after it cannot
  fail. A 34 GiB folder once panicked in MSF on the lead-out and reached
  the guest as `LIBDISC_EIO`. The builder now refuses the size up front
  with both sizes in the message, and `Msf::from_lba` saturates.
- **Force tray changes.** The launcher does (doc 07); why is in
  `docs/00-status.md` "Gotchas".
- **A program that polls a drive consumes its media-change news.** The
  sense goes once, to whoever asks first, so `CDSHELF` waits on the
  shelf's LIST (which runs under a pending UNIT ATTENTION without
  clearing it), never on TEST UNIT READY, and Windows gets the news.
  It used to dismount the volume after a swap instead, which raced
  AutoPlay and broke the freshly mounted volume (2026-10-04).
- **A host file changed under a mounted disc reads as `EMEDIUM`**, never a
  torn file. Eject and insert again to see edits.

## What stayed open

- **The stale-file rule is checked on the host only** (`discx selftest`'s
  EMEDIUM case). A guest version would race XP's read-ahead and be flaky.
- **No MSCDEX run.** The DOS tools' FreeDOS floppy has no CD driver;
  bsdtar with Joliet off stands in.
- **Not bootable.** No El Torito catalogue (doc 17 §8 says how to add one).
