# Track M5h: the host's own optical drive (`hostcd:`)

`hostcd:/dev/sr0` puts the disc in a real drive on the host into the
guest's drive, so a game runs from its original CD or DVD. The disc can
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
- QEMU patch 52: the shelf strips `hostcd:` as it strips `isodir:`.
- `launcher-core/src/host_drive.rs`: the host's drives (Linux:
  `/sys/class/block/sr*`), with `disc_library::qemu_medium`,
  `DiscKind::Drive`, `control::medium_path` and `Shelf::host_drives`.
- `launcher-mitsuami/src/discs.rs`: one "Drive: <model>" item per drive
  in the shelf's Add menu.

A drive goes on the shelf like an image, by device path, so the boot
disc, a live insert, the player's Disc menu and the guest's CDSHELF all
take it with no change of their own.

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
- Step 2, the QEMU driver with the swap: written; a guest run pending.
- Step 3, the launcher's shelf: written; a click test pending.

## Traps

- The first drive returns the *next* frame's Q, and some Q frames with
  bits flipped under a non-matching CRC. Search by the address in the
  frame, never by the LBA asked for; serve synthesized Q.
- A READ CD that crosses from a data track into an audio pregap fails
  whole after ~10 s of retries. Never let the read-ahead leave a track.
- `/dev/sr0` must be readable by the user: logind's `uaccess` ACL gives
  it to the seat's user, otherwise the `optical` group.

## Open

- Windows (`IOCTL_SCSI_PASS_THROUGH_DIRECT`), macOS (IOKit SCSITask,
  with the disc unmounted; the App Store sandbox may refuse it).
- The Flatpak needs `--device=all` for `/dev/sr*`.
- A copy-protected original (SafeDisc 1: Crimson Skies) through the
  drive.
- The guest's tray lock is not passed to the real drive.
