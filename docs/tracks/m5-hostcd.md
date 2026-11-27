# Track M5h: the host's own optical drive (`hostcd:`)

`hostcd:/dev/sr0` puts the disc in a real drive on the host into a
second CD drive in the guest, beside the one the shelf fills, so a game runs from its original CD or DVD. The disc can
be swapped in the real drive at any time and the guest sees a disc
change. Opened 2026-11-23 (user: a real USB CD/DVD drive, "I need the
feature of being able to swap cds at any time"). The design is doc 17
§9; this doc keeps scope, test loop, traps and open items.

## Scope and files

- `libdisc/src/drive.rs`: the drive as a disc. TOC to model, READ CD /
  READ(10) through `SG_IO`, the read-ahead window, the pregap search,
  `ready` and `eject`.
- `libdisc/src/lib.rs`: `Source::Drive`, `Disc::drive`, the dispatch in
  `open`, `read_raw`, `classify_into` and `read_sub`.
- `libdisc/src/capi.rs`, `libdisc.h`: `libdisc_drive_ready`,
  `libdisc_drive_eject`; `LIBDISC_API_VERSION` 2.
- `libdisc/qemu/cdimage.c`: the `hostcd` driver and its watch thread.
- `launcher-core/src/host_drive.rs`: the host's drives (Linux:
  `/sys/class/block/sr*`).
- `bundle::Machine::host_drive` and `host_drive_args`: **a second
  CD-ROM**, apart from the drive the shelf fills (user: "the real drive
  should be a separate drive, apart from the virtual one"). The
  secondary IDE slave (`bus=ide.1,unit=1`) on the era's families, AHCI
  port `ide.3` on Windows 11 (after the drivers disc on `ide.2`); qdev id
  `hostcd` (`control::HOST_CDROM_ID`). No shelf property: only the real
  drive changes its disc.
- `wizard::Form`: the "Real CD/DVD drive" picker on the Storage page
  (None, each drive by model, a drive the machine names that is not
  plugged in now), the machine details' row, `--wizard-edit`'s last
  argument (`/dev/srN|none`).
- `launcher-mitsuami/src/wizard.rs`: the picker.

The first cut put the drive on the shelf like an image; the user wanted
it as its own drive, so the shelf, CDSHELF and the player's Disc menu
never see it.

## Test loop

```sh
scripts/build.sh                                  # libdisc, QEMU, the players
target/release/discx info hostcd:/dev/sr0         # the model from the drive's TOC
target/release/discx scan hostcd:/dev/sr0 0 20000 # read speed and L-EC
```

The Moto Racer CD (`oldstuff/Moto.Racer.1997.DSI.CD`, 1 data and 12
audio tracks) is the reference disc: its `.mds` dump is the oracle.
`discx info` gives the same layout from the drive and the dump (track 2's
index 0 at 86510 included), and data sectors are byte-identical raw and
cooked. Audio differs by the drive's read offset, as it would between
two drives.

## State

- Step 1, libdisc reads a drive: done (Linux).
- Step 2, the QEMU driver with the swap: the disc is seen at boot on XP
  and plays (user, 2026-11-24). The music froze the machine for seconds
  at a time: fixed for CD audio (a read-ahead worker and
  `libdisc_read_raw_nowait`, doc 17 §9), user's retest pending. Win98's
  CD Player lists no audio tracks until a swap (user, 2026-11-24; parked
  by the user, the cause not found: not a late disc, not the model, not
  the audio status).
- Step 2b, data reads off the main loop: done (patch 102, 2026-11-26),
  checked on XP (a whole-disc copy, the main loop never held more than
  3 ms, 2.0 s on the old path); the user's retest with the game
  pending.
- Step 3, the machine setting: done headless (`--wizard-edit`, the QEMU
  arguments of a 98 and a Windows 11 machine, both drives in one QEMU);
  a click test of the picker pending.

## Traps

- The first drive returns the *next* frame's Q, and some Q frames with
  bits flipped under a non-matching CRC. Search by the address in the
  frame, never by the LBA asked for; serve synthesized Q.
- A READ CD that crosses from a data track into an audio pregap fails
  whole after ~10 s of retries. Never let the read-ahead leave a track.
- One command moves at most `max_sectors` (120 KiB over USB); more fails
  with EIO and looks like a bad sector. `read_window` splits runs.
- Drive I/O on QEMU's main loop freezes the guest for the whole seek.
  `LIBDISC_DRIVE_SYNC=1` brings the old synchronous reads back as the A/B;
  a probe that asks QMP every 100 ms during the run measures the freeze.
- A CD voice that writes nothing holds back the sound card's own voices
  (QEMU's mixer goes as far as its slowest voice): write silence when the
  drive has not delivered yet.
- The first Q read after a spin-up can fail its CRC; the pregap search
  tries a few frames. A cold open can take more than 10 s; the `hostcd`
  check's SeaBIOS probe waits 30.
- The host's own probes (blkid / udisks reading UDF anchors near the end
  of a mixed-mode disc) show in dmesg as READ(10) "Illegal mode for this
  track" on audio sectors. They are not ours, and harmless.
- Testing seeks makes the drive loud (spin-speed changes); the user
  asked if it was defective. Keep drive-hammering runs short.
- `/dev/sr0` must be readable by the user: logind's `uaccess` ACL gives
  it to the seat's user, otherwise the `optical` group.

## Open

- Windows (`IOCTL_SCSI_PASS_THROUGH_DIRECT`), macOS (IOKit SCSITask,
  with the disc unmounted; the App Store sandbox may refuse it).
- The Flatpak needs `--device=all` for `/dev/sr*`.
- A copy-protected original (SafeDisc 1: Crimson Skies) through the
  drive.
- The guest's tray lock is not passed to the real drive.
