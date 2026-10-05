# 17. CD-ROM backend: implementation (M5, doc 05)

How the drive emulation of doc 05 is built: the `libdisc` disc model,
the image formats, error correction and subchannel, the C API, the MMC
reply layouts, the QEMU block driver and ATAPI patches, CD-DA, the
tests, what the protections we own actually read, and a host folder as
a disc (§8). Doc 05 has the problem and the acceptance table; the track
records are `docs/tracks/m5-cdrom-backend.md` and `m5-dirdisc.md`; the
disc shelf is doc 07 and patch 52. QEMU function names are exact for the
pinned tree (v11.1.2; the patches were ported from 9.2.4 in track M21).

## 1. Shape

```
image on disk            libdisc (Rust crate, staticlib, C API in libdisc/libdisc.h)
 .cue+.bin / .ccd+.img+.sub    disc model: sessions, tracks, indices, per-sector kind,
 / .iso / .mds+.mdf            raw ⇄ cooked synthesis, EDC/ECC, subchannel Q, MMC responders
 / isodir:<folder>
        │                                  │
        ▼                                  ▼
 qemu/block/cdimage.c  ── a QEMU *format* block driver (+ the isodir protocol driver):
   cooked 2048-byte view through bdrv_co_preadv (qemu-img, SeaBIOS El Torito,
   blockdev-change-medium, eject all keep working), plus
   cdimage_disc(bs) → the libdisc handle for whoever wants the raw model
        │
        ▼
 qemu/hw/ide/atapi.c ── patch 51: every command asks `cdimage_disc(blk_bs(s->blk))`;
   NULL → stock QEMU, byte for byte (plain ISO on the `raw` driver);
   non-NULL → sector reads, TOC, subchannel, capabilities, CD-DA from the model
        │
        ▼
 guest: an ordinary IDE/ATAPI CD-ROM (in-box drivers, protection drivers unmodified)
```

Decisions (consistent with doc 05 and ADR-004; do not reopen):

1. **Integration is a block driver, not a device property.** `-cdrom
   game.cue` probes to `cdimage`; `-drive
   file=game.cue,format=cdimage,media=cdrom` is the explicit form. The
   drive stays QEMU's `ide-cd`, and a medium swap is QMP
   `blockdev-change-medium`.
2. **Rust computes the MMC reply bytes** (TOCs, subchannel, READ CD
   layouts), so the host-side exerciser tests the exact bytes a guest
   sees. `atapi.c` copies buffers and drives the IDE state machine.
3. **Reads are synchronous** (`pread` inside libdisc), with no AIO. A
   guest reads at most 128 KiB per command. If stalls ever show, mmap
   inside libdisc; the C API does not change.
4. **Copy-protection fidelity comes from modelling the drive's error
   correction** (§2.5, §2.6c), with no annotation file.
5. **A plain `.iso` stays on QEMU's `raw` driver** (probe score 0), the
   bit-identical regression baseline. `format=cdimage` on an `.iso` is
   allowed, and the tests compare the two paths.
6. **Subchannel is stored deinterleaved** (P..W, 12 bytes each, the
   CloneCD `.sub` layout). Only the READ CD responder produces the MMC
   interleaved form.

## 2. The libdisc crate

`libdisc/` is a workspace member (`crate-type = ["rlib", "staticlib"]`)
with **no dependencies**. Keep it that way, because the staticlib is
linked into QEMU.

| File | Contents |
|---|---|
| `src/lib.rs` | the model (§2.1), `Disc::open` dispatching on extension, content, or a directory |
| `src/msf.rs` | `Msf` ⇄ LBA (LBA 0 = MSF 00:02:00) |
| `src/cue.rs`, `ccd.rs`, `mds.rs`, `iso.rs` | the image formats (§2.3–2.4b) |
| `src/isodir.rs` | a host folder as an ISO 9660 + Joliet volume (§8) |
| `src/sector.rs` | raw ⇄ cooked: sync, header, subheader, EDC placement, sector-kind detection |
| `src/ecc.rs` | EDC and RSPC P/Q parity, verification and correction (§2.5) |
| `src/subq.rs` | Q-channel synthesis and CRC-16, P channel (§2.6) |
| `src/mmc.rs` | READ TOC 0/1/2, READ SUB-CHANNEL, READ CD, READ DISC INFORMATION (§4) |
| `src/capi.rs` | the `extern "C"` functions of `libdisc/libdisc.h` (§3) |
| `src/bin/discx.rs` | the exerciser and diagnosis tool (§6.1) |
| `qemu/cdimage.c`, `cdimage.h` | the block drivers, overlaid into QEMU by `prepare-qemu.sh` (§5) |
| `libdisc.h` | the one C header, overlaid next to them |

### 2.1 Model

```rust
pub struct Disc {
    pub sessions: Vec<Session>,          // 1-based, ascending
    pub mcn: Option<[u8; 13]>,           // CATALOG
    pub raw_toc: Option<Vec<TocEntry>>,  // CCD/MDS entries verbatim; None = synthesize
    files: Vec<Payload>,                 // path, length, mtime; opened on demand
    blobs: Vec<Vec<u8>>,                 // generated sectors (isodir metadata)
    fds: Mutex<Vec<(usize, Arc<File>)>>, // 8-entry MRU handle cache
}
pub struct Session { pub number: u8, pub tracks: Vec<Track>, pub leadout_lba: i32 }
pub struct Track {
    pub number: u8,                      // 1..=99, unique across sessions
    pub mode: TrackMode,                 // Audio, Mode1, Mode2 (form per sector), Mode2Form1/2, Mode2Formless
    pub control: u8,                     // audio 0x0 (|1 pre-emphasis, |2 DCP, |8 4ch), data 0x4
    pub isrc: Option<[u8; 12]>,
    pub indices: Vec<(u8, i32)>,         // (index, absolute LBA), index 1 always present
    pub start_lba: i32,                  // index 1
    pub end_lba: i32,                    // exclusive
    pub extents: Vec<Extent>,            // contiguous coverage of [first index .. end_lba)
}
pub struct Extent { pub lba: i32, pub count: u32, pub source: Source, pub sub: Option<SubSource> }
pub enum Source {
    File { file: usize, offset: u64, layout: Layout, swap: bool, eof_pad: bool },
    Mem { blob: usize, offset: u64 },    // isodir's descriptors, path tables, directories
    Silence,                             // audio pre/postgap
    ZeroData,                            // data pre/postgap, valid EDC/ECC
}
pub enum Layout { Cooked2048, Mode2_2336, Raw2352, Raw2352Sub96 }
pub enum SubSource { File { file: usize, offset: u64 } } // 96 B/sector, deinterleaved
```

Rules:

- LBAs are `i32`, and the program area starts at LBA 0 (MSF 00:02:00).
  The readable range is `[0, last session lead-out)`; anything else is
  `Err(Range)`. libdisc never invents lead-in data, and track 1's
  mandatory 150-sector pregap is never in a file and never addressed.
- The last session's lead-out is `sector_count()`, READ CAPACITY and
  TOC point A2.
- **A *CD* ends at `msf::MAX_LBA`** (449,849, MSF 99:59:74, ~878 MiB),
  the most three BCD bytes address. A *disc* goes on: past an 80-minute
  CD (`CD_MAX_SECTORS` in `atapi.c`, 360,000 sectors) the drive reports
  a DVD-ROM profile (patch 53), nothing asks for an MSF, and the model
  reaches a dual-layer DVD-9, 4,173,824 sectors. `Msf::from_lba`
  saturates rather than asserting (a panic would be `LIBDISC_EIO` on an
  unrelated command). **The opener refuses a disc past any real
  medium** (§8).
- Payload files are held as path, length and mtime and opened on demand
  into the MRU cache (eager opens of 5,000 files pass macOS's default
  limit of 256 descriptors). Every open re-`stat`s, and a changed file
  is `Error::Medium`, a read error as from a damaged disc, never a torn
  read. `eof_pad` lets an extent's last sector run past its file's end;
  only `isodir` sets it, so a short image file stays an error. A host
  read failure is logged on stderr (`libdisc: …`, the first 32), since
  a guest sees only a sense code.
- `extent_at` is a binary search (a linear scan costs a comparison per
  file per sector read on a folder disc).
- Sector reads go through `read_raw(lba) -> [u8; 2352]` (stored, or
  synthesized from cooked per §2.5) and `read_sub(lba) -> [u8; 96]`
  (stored, or synthesized per §2.6). `read_cooked(lba)` is raw → kind
  detection → L-EC → user data, or `Err(Medium)` / `Err(Mode)` (audio,
  gap).

### 2.2 Byte conventions

- **Header MSF and Q-channel times are BCD** (`0x21` = 21). MMC TOC and
  subchannel *replies* carry binary MSF or a 32-bit big-endian LBA.
- A sector header's absolute MSF is the MSF of `lba + 150`.
- CD-DA: 16-bit little-endian, left then right, 588 stereo frames per
  sector. `FILE … MOTOROLA` is big-endian (swap). For `FILE … WAVE`
  the parser walks the RIFF chunks and uses the `data` chunk's offset
  (not a fixed 44), and refuses anything but 44100 Hz / 16-bit / stereo.
- Raw sectors in dumps are already **descrambled**. We never scramble.
- Sync: `00 FF×10 00`. Header: `MIN SEC FRAME MODE` (BCD ×3, then 1 or
  2). Mode 2 subheader: bytes 16..24; form 2 = bit 5 of byte 18.

### 2.3 Cue sheets (`cue.rs`)

Accepted (case-insensitive keywords, quoted or bare names, `REM`
ignored, CRLF or LF):

```
CATALOG 1234567890123
FILE "name" BINARY|MOTOROLA|WAVE
  TRACK nn AUDIO|MODE1/2048|MODE1/2352|MODE2/2336|MODE2/2352|CDI/2336|CDI/2352
    FLAGS [DCP] [4CH] [PRE] [SCMS]
    ISRC XXXXXXXXXXXX
    PREGAP mm:ss:ff
    INDEX nn mm:ss:ff
    POSTGAP mm:ss:ff
```

Where cue parsers go wrong:

- `INDEX` times are **relative to the start of the current FILE**,
  without the 150 offset. Absolute LBA = the running LBA at the file's
  start + the index frames.
- A track's sectors run from its lowest index to the next track's lowest
  index in the same file, or the file's end. File sectors = size ÷
  stride (2048 / 2336 / 2352; a remainder is an error naming the file).
- `PREGAP` sectors are **not in the file**. They are synthesized before
  index 01 as index 00 (silence, or zero data with valid EDC/ECC), and
  `POSTGAP` likewise after the last sector. Absolute LBAs advance across
  both.
- Several `FILE`s concatenate, and the running LBA carries over.
- Track 1 starts at LBA 0. `INDEX 00 00:00:00` + `INDEX 01 00:02:00`
  (pregap in the file) is accepted as is.
- Control: AUDIO 0x0, +0x1 `PRE`, +0x2 `DCP`, +0x8 `4CH`; data 0x4.
- Payloads resolve relative to the cue, then case-insensitively (dumps
  made on Windows), then fail naming the path tried.
- Single session only. A declared second session is named in the error.

### 2.4 CloneCD (`ccd.rs`)

`.ccd` is an INI file:

```
[CloneCD]  Version=3
[Disc]     TocEntries=n  Sessions=n  DataTracksScrambled=0  CDTextLength=0  CATALOG=…
[Session n] PreGapMode=n PreGapSubC=n
[Entry n]  Session= Point= ADR= Control= TrackNo= AMin= ASec= AFrame= ALBA= Zero= PMin= PSec= PFrame= PLBA=
[TRACK n]  MODE=0|1|2  INDEX 0=lba  INDEX 1=lba  ISRC=…
```

- `.img` holds 2352 bytes per sector from LBA 0, the optional `.sub` 96
  bytes per sector, deinterleaved. `DataTracksScrambled=1` is refused.
- Tracks come from `[Entry]` records with `Point` 1..99 (`PLBA` = index
  1, `Control`), and `[TRACK n]` adds `INDEX 0` and the mode. Session
  lead-outs come from `Point=0xA2`, one `Session` per distinct
  `Session=`.
- **Every `[Entry]` is kept verbatim** in `raw_toc` and replayed by READ
  TOC format 2 (§4.1), the fidelity SecuROM-era checks want.

### 2.4b MDS/MDF (`mds.rs`)

Alcohol 120% / Daemon Tools images have a `MEDIA DESCRIPTOR` header
(version 1.x), 24-byte session blocks at the offset in header byte 0x50,
80-byte track blocks (mode, subchannel flag `0x08` = 96 bytes
interleaved appended, ADR/control, point, MSF, PMSF, extra-block offset,
sector size 2048/2352/2448, start sector, 64-bit start offset, footer
with the `.mdf` name) and extra blocks (pregap, length). Every block
with a point becomes a raw TOC entry. Modes: `0xA9` audio, `0xAA` Mode
1, `0xAB` Mode 2, `0xAC`/`0xAD` form 1/2, and **`0xEC`, Alcohol's mixed
Mode 2 (XA)**, form per sector, read as Mode 2 and not audio (misread
as audio, NFS Porsche Unleashed's MDS was one unmountable 281,279-sector
CD-DA track). An MDS whose mode byte contradicts its TOC control bits is
refused rather than misread.

**The layout rule**, checked against a RAW+SUB dump's own Q frames: the
`.mdf` holds each track from `start_sector` (index 1) for `length`
sectors at `start_offset`. The `pregap` sectors are *not* in the file
and are synthesized as index 0, and track 1's pregap of 150 is never
addressed. DPM blocks (header byte 0x54) are ignored until a title
needs timing.

### 2.5 Raw ⇄ cooked and L-EC (`sector.rs`, `ecc.rs`)

Raw layouts (2352 bytes): Mode 1 `sync[12] header[4] data[2048] edc[4]
zero[8] p[172] q[104]`; Mode 2 form 1 `sync header subheader[8]
data[2048] edc[4] p[172] q[104]`; form 2 `sync header subheader
data[2324] edc[4]` (EDC optional, may be zero).

**EDC**: CRC-32, polynomial x^32+x^31+x^16+x^15+x^2+x+1, LSB-first
(table from reflected `0xD8018001`), init 0, no final XOR, stored
little-endian. Coverage: Mode 1 bytes 0..2064; form 1 16..2072; form 2
16..2348.

**ECC (RSPC)** over GF(2^8), primitive polynomial `0x11D`, on the
2064-byte area bytes 12..2076 (for Mode 2 the header is taken as four
zero bytes). It is the standard two-pass loop (Neill Corlett's ECM;
libmirage does the same). Each pass writes two parity bytes per major,
at `dst[major]` and `dst[major + major_count]`:

| Pass | major_count | minor_count | major_mult | minor_inc | writes to |
|---|---|---|---|---|---|
| P | 86 | 24 | 2 | 86 | sector[2076..2248] |
| Q | 52 | 43 | 86 | 88 | sector[2248..2352] |

For major `m`: `index = (m >> 1) * major_mult + (m & 1)`, `a = b = 0`;
`minor_count` times: `t = src[index]`, `index += minor_inc` (wrapping at
2064, or 2064 + 172 for Q, which reads the P parity too), `a ^= t; b ^=
t; a = gf_mul2(a)`; then `a = ecc_b_lut[a ^ b]`, `dst[m] = a`, `dst[m +
major_count] = a ^ b`. The tables: `ecc_f_lut[i] = 2·i` in GF,
`ecc_b_lut[ecc_f_lut[i] ^ i] = i`.

`verify_mode1` / `verify_mode2f1` recompute and compare, returning `Ok`,
`EdcMismatch`, `EccMismatch` or `NoSync`. `NoSync` is no sync or a wrong
mode byte in a data track: an audio-format tail, or the zero filler a
dumper writes for an unreadable sector, whose all-zero EDC and parity
would otherwise verify.

**A cooked read is decided by the EDC and repaired by the parity only
when the EDC fails.**

1. **The EDC says whether the delivered bytes are intact**
   (`ecc::edc_ok`, after the sync check). It is a CRC-32 over exactly
   what a cooked read hands over, so when it holds, any disagreement is
   in parity the guest never sees, and the sector is delivered without
   computing the parity. A Warcraft 3 dump settled this rule: 92 L-EC
   failures, every one EDC-clean, and its in-game video stopped midway
   while we refused sectors that were provably right.
2. **A wrong EDC gives the decoder work** (`ecc::correct`). Each P and Q
   codeword's two parity symbols locate one wrong symbol (a single error
   of magnitude `s0` at position `i` gives `s1/s0 = alpha^(m+1-i)`).
   The decoder corrects one per codeword and alternates the passes, up
   to four rounds. **The EDC is the verdict here too.** A sector is
   accepted, and written back, only when its CRC-32 then holds, so a
   mis-correction cannot pass. On the selftest disc a burst of up to 96
   bytes is recovered (the interleave spreads it over codewords); 128
   bytes, a filled body or a zeroed sector are not.

Only what the decoder cannot fix is `Err(Medium)`. `sector_info`'s `lec`
reports whether the sector verifies *as stored*, what `discx scan`
counts. `LIBDISC_NO_CORRECT=1` turns both steps off (every
L-EC failure a medium error) for an A/B.

A real drive's L-EC hardware corrects the same way before it hands data
over. A protection band still fails because its sectors are damaged far
past one symbol per codeword (DiscImageCreator writes the body as
`0x55`) and their EDC is wrong. What a *raw* read delivers is §2.6c.

C2 pointers set a bit for every byte whose recomputed parity disagrees,
approximate but enough for checks that count errors.

Synthesis (cooked image → raw request) builds sync + BCD header + data +
EDC + zero + P + Q per sector on demand (~10 µs, no cache).

### 2.6 Subchannel synthesis (`subq.rs`)

With no `.sub`, `read_sub(lba)` returns P all `0xFF` inside index 00
and for the 150 sectors before a track's index 01 (the pause flag), else
`0x00`, Q as below, and R..W zero. A `.sub` wins for every sector it
covers; sectors past a truncated one are synthesized.

Q, ADR 1 (position):

```
byte 0     control<<4 | adr(1)
byte 1     TNO   BCD track number
byte 2     INDEX BCD (00 in pregap)
byte 3-5   MIN SEC FRAME  BCD, track-relative; in index 00 it counts DOWN
           (to 00:00:01 at the last pregap sector on both real discs measured)
byte 6     ZERO
byte 7-9   AMIN ASEC AFRAME  BCD absolute (lba + 150)
byte 10-11 CRC-16 big-endian: CCITT 0x1021, init 0, over bytes 0..10, inverted
```

With an MCN or ISRC, ADR 2 goes at `lba % 100 == 98` and ADR 3 at `==
99` (MCN: 13 BCD digits high nibble first in bytes 1..7, byte 8 zero,
AFRAME in byte 9; ISRC: 12 characters 6-bit packed per Red Book in bytes
1..8, AFRAME in byte 9).

**Synthesizing an undeclared pregap is a guess.** Where a descriptor
gives a track no index 00, the disc may be any of three things and the
descriptor cannot tell them apart (the MDS `pregap` field is 0 for every
such track on both Alcohol dumps we have; measured with `discx
subscan`):

| Disc | P before the next track | Q there |
|---|---|---|
| AoE Gold | `0xFF` (pause) | previous track, index 01, counting up |
| Moto Racer | `0xFF` (pause) | next track, index 00, counting down over 149 sectors |
| Settlers 3 | `0x00` | previous track, index 01, counting up |

We synthesize the first. It reproduces AoE's subchannel exactly (277,626
of 277,626 ADR 1 frames) and costs ~0.7 % of frames on a
Moto-Racer-shaped disc. The second convention traded 1,633 wrong frames
on one disc for 1,866 and 1,650 on two others. MCN placement is the same
kind of guess: Rayman 2, the one disc here with MCN frames (3,307),
places them elsewhere (99.0 % agreement). No game notices: AoE Gold and
Moto Racer play their soundtracks, and nothing met so far reads Q
(§2.6b). A check that fingerprints subchannel wants a dump that carries
it (CCD `.sub`, MDS 2448), replayed verbatim.

### 2.6b The negative control, and what the protections read

A check that ran and was satisfied looks like one that never ran.
`discx repair` builds the disc that tells them apart: every
L-EC-failing sector made to verify, only bytes 2064–2351 changed (§6.1). The
control copies live under `oldstuff/clean/` (FIFA 2002: 584 sectors
repaired; Settlers 3 CD01: 547, leaving the 150 sync-less run-out
sectors a drive fails too).

**Both titles run from their repaired copies** (checked by the user), so
SafeDisc 2.x and ProtectCD are inconclusive in doc 05, not passing, even
though both binaries are wrapped: `fifa2002.exe` has SafeDisc 2's
`stxt371` / `stxt774` sections and `BoG_` marker, `S3.EXE` ProtectCD's
`.ficken` section (`7z` reads them out of the qcow2).

**The ATAPI traces** (`-trace ide_atapi_cmd_packet -trace
ide_atapi_cmd_error`, which also works through the player's QEMU
arguments), FIFA 2002 from its `.mds` on one image:

| Run | CD on | Display | Outcome | Reads | Multi-sector | Max LBA | Band reads |
|---|---|---|---|---|---|---|---|
| bare QEMU | ide0 slave | cirrus | *"insira o CD"* | 488 | 1 | 13272 | 0 |
| bare QEMU `-nodefaults` | ide0 slave | cirrus | *"insira o CD"* | 506 | 1 | 13272 | 0 |
| bare QEMU | ide.1 | cirrus | no dialog, then a crash | 505 | — | — | 0 |
| player | ide.1 | d3dpt-vga | **runs** | 781 | 281 | 250230 | **0** |

- SafeDisc 2's probe is an anchor `READ(10)` at LBA 800 and one
  pseudo-random single sector (~1300–9900), repeated (13 pairs in the
  passing launch): a timing-shaped pattern, not an error-pattern check.
  **It never reads a band sector**, which is why the repaired disc
  passes.
- Settlers 3 CD01 (`.ccd`, in the player) reaches its menu in 1196
  reads, none above LBA 191776 (the band starts at 195539), and one READ
  SUB-CHANNEL. It reads neither its data anomaly nor its Q anomaly, and
  the `.cue` (synthesized, clean Q) and `.ccd` (the disc's own Q) behave
  the same.
- **The check refuses when the CD shares the boot disk's IDE channel**
  (`-drive media=cdrom` lands at ide0 slave) and passes on `ide.1`,
  where the player and the launcher put it. The correlation is measured;
  a timing check perturbed by the shared channel is only a hypothesis.
  Run protected titles with the CD on its own channel.
- **Attribute every sense reply to its drive before calling it a bug.**
  The `GET CONFIGURATION` errors in the first trace came from QEMU's
  *empty* default CD drive, which answers from stock code (feature 0
  only). Ours answers an unsupported starting feature with the header
  alone, as MMC requires. The one error our drive returns, MODE SENSE
  page 0x1b, is the correct reply to a page we do not implement.

So SafeDisc 2 and ProtectCD never read the L-EC band. §2.5 is still the
right drive behaviour (`atapi-guest` proves the errors are delivered),
and §2.6c is the scheme that depends on it.

### 2.6c The protection that does read the band

**Crimson Skies**, SafeDisc 1.50.020 (the original loader,
`CRIMSON2.EXE` in the user's `claude98` install: `BoG_ *90.0&!!` at
0x29250 with version dwords `(1, 50, 20)`, the Secdrv strings,
`CRIMSON.ICD` beside it). Its dump `C_SKIES.cue` (Aaru 5.4.1) carries
**579 sectors from LBA 807 to 10018** with no sync pattern at all (not
`0x55` fill, and not merely scrambled). So a SafeDisc 1.x disc can
carry a band; `discx scan` says which ones do (§6.x).

The loader does three rounds of an anchor `READ(10)` at LBA 800 and one
pseudo-random sector in ~1400..10000 read **raw** (`READ CD`, byte 9 =
`0xF8`). When a raw read delivered the stored bytes, every round stopped
on the first band sector and the loader refused with *"Cannot locate the
CD-ROM"*. With a raw read of an unreadable sector answered `03/11/05`,
the loader does REQUEST SENSE, carries on probing, decrypts
`CRIMSON.ICD` and reaches the game's "Select Video Device" dialog. NFS
Porsche Unleashed (SafeDisc 1.x, 0 L-EC failures over 281,279 sectors)
works either way; the band is the difference.

The user's own install runs a no-CD `CRIMSON.EXE` that never reads the
disc.

**The raw rule** (`mmc::read_cd_sector`):

- user data selected, EDC/ECC not → a cooked read: verify, correct,
  `Err(Medium)` for the rest;
- user data and EDC/ECC, **no C2 field** → the stored bytes, but only
  if the sector is readable at all (the same `verify_or_correct` as the
  cooked path); an unreadable one is `Err(Medium)`, as a drive answers;
- **C2 error flags asked for** (byte 9 bits 1–2) → the stored bytes
  regardless, with C2 marking the untrusted ones. This is how a dumping
  tool gets an unreadable sector out of a real drive.

`discx selftest`'s `lec` case carries both halves.

## 3. The C API (`libdisc/libdisc.h`)

One hand-written header for the crate, the block driver and `atapi.c`,
like `d3dpt/d3dpt_proto.h`. Bump `LIBDISC_API_VERSION` (1) on any
change; `cdimage_open` refuses a mismatch. Every function is thread-safe
on one handle, since the layout is immutable and the payload handle
cache is behind a lock. `libdisc_open` also takes a directory (§8).

| Call | Does |
|---|---|
| `libdisc_api_version`, `libdisc_probe(head, len, filename)` | 0..100 confidence: `.cue` that parses, `.ccd` with `[CloneCD]`, `.mds` → 100; `.iso` → 0 (stays on `raw`) |
| `libdisc_open(path, err, errlen)` / `libdisc_close` | NULL and a message on failure |
| `libdisc_sector_count`, `_session_count`, `_track_count`, `_track_info`, `_sector_info` | the layout; `LibdiscTrackInfo` has number, session, control, mode, `start_lba` (index 1), `pregap_lba`, `end_lba`; `LibdiscSectorInfo` kind, track, index, `lec` as stored |
| `libdisc_read_cooked` / `_read_raw` / `_read_sub` | 2048 L-EC-checked bytes / 2352 / 96 deinterleaved |
| `libdisc_mmc_read_toc`, `_read_subchannel`, `_read_disc_information` | a reply into `out`, its length or an error; the caller truncates to the allocation length |
| `libdisc_mmc_read_cd_length` / `_read_cd_sector` | bytes per sector for a READ CD CDB (or `EINVAL`), then one call per sector |

| Code | Meaning | Sense |
|---|---|---|
| `LIBDISC_ERANGE` −1 | LBA outside `[0, lead-out)` | 05/21/00 |
| `LIBDISC_EMEDIUM` −2 | unreadable sector (§2.5, §2.6c), or a changed payload | 03/11/05 |
| `LIBDISC_EMODE` −3 | wrong sector kind for the request | 05/64/00 |
| `LIBDISC_EINVAL` −4 | bad parameter or CDB field | 05/24/00 |
| `LIBDISC_EIO` −5 | host file read failed, or a caught panic | 04/xx |

`capi.rs` holds a `Box<Disc>` behind the opaque pointer, fills `err` as
NUL-terminated ASCII, and wraps every body in `catch_unwind`, so a panic
never crosses the boundary.

## 4. MMC reply layouts (`mmc.rs`)

The replies follow MMC-3 (T10 1363-D), which Win9x/XP's `cdrom.sys`
and period protection drivers assume. Multi-byte fields are big-endian.
`msf=1` → `0, M, S, F` binary, `msf=0` → 32-bit LBA.

### 4.1 READ TOC/PMA/ATIP (0x43)

A 2-byte data length (excluding itself), then:

- **Format 0** (TOC): first and last track, then an 8-byte descriptor
  per track from `start` (0 or 1 = all, 0xAA = lead-out only, past the
  last track → EINVAL): `reserved, ADR<<4|control, track, reserved,
  time`; then track 0xAA, control 0x14 if the last track is data else
  0x10, at the last lead-out. Multisession lists every session's
  tracks. The one deliberate difference from QEMU's own
  `cdrom_read_toc` (kept for the no-disc path) is the lead-out's
  control: ours is `0x14`, as a real drive reports for a data disc,
  QEMU's `0x16`.
- **Format 1** (multisession): first and last session, one descriptor
  for the first track of the last session.
- **Format 2** (raw TOC): first and last session, then 11-byte
  descriptors `session, ADR<<4|control, TNO 0, POINT, MIN, SEC, FRAME,
  ZERO, PMIN, PSEC, PFRAME`; per session `A0` (PMIN first track, PSEC
  disc type: 0x20 CD-ROM XA when the session's first track is Mode 2,
  else 0x00), `A1` (last track), `A2` (lead-out), then each track at its
  index 1. Multisession adds `B0` (ADR 5) after the last `A2`: the last
  lead-out + 150 and 0x4C:0x2C:0x00, as drives report for closed discs.
  It is **always MSF**, whatever the `msf` bit. With `raw_toc` present,
  its entries are emitted verbatim.
- Formats 3–5 → EINVAL.

### 4.2 READ SUB-CHANNEL (0x42)

`out[0] = 0`, `out[1]` = audio status (0x11 playing, 0x12 paused, 0x13
completed, 0x14 error, 0x15 none), `out[2..4]` = data length; `subq=0`
→ the header only.

- **Format 1** (position): `1, ADR<<4|control, track, index, absolute
  time, relative time` from the Q of the position (§5.4); relative time
  counts down in index 0.
- **Format 2** (MCN): `2, 0, 0, 0, MCVal<<7`, 13 ASCII digits + NUL.
- **Format 3** (ISRC): `3, ADR<<4|control, track, 0, TCVal<<7`, 12
  ASCII + NUL + 0.

### 4.3 READ CD (0xBE) and READ CD MSF (0xB9)

Expected sector type `(byte1 >> 2) & 7`: 0 any, 1 CD-DA, 2 Mode 1, 3
Mode 2 formless, 4 form 1, 5 form 2. Byte 9: sync 0x80, header 0x60
(01 header, 10 subheader, 11 both), user data 0x10, EDC/ECC 0x08, C2
0x06 (01 = 294 bytes, 10 = 296 with the block error byte). Byte 10 & 7:
subchannel 0 none, 1 raw 96 (interleaved), 2 Q 16 bytes, 4 R–W 96.

`read_cd_length` sums the selected fields for the type per MMC-3's
table (Mode 1 `0xF8` = 2352, `0x10` = 2048; CD-DA any combination =
2352). C2 adds 294 or 296, and subchannel 96 or 16. Illegal combinations
are EINVAL, and a sector of the wrong type is EMODE (type 0 takes
anything). The fill copies sync, header, subheader, data, EDC/ECC, C2
and subchannel, in that order. In the MSF form the start is inclusive
and the end exclusive.

**L-EC applies to a raw read too**, by the raw rule in §2.6c. Only a
request that asks for C2 flags gets an unreadable sector's bytes.

### 4.4 READ DISC INFORMATION (0x51)

34 bytes: length 32, status 0x0E (complete, last session complete),
first track 1, sessions, first and last track of the last session,
lead-in and lead-out `0xFFFFFFFF`, disc type by the A0 rule. The stock
`cmd_read_disc_information` stays for the no-disc path.

## 5. The QEMU side

### 5.1 `block/cdimage.c` (in the repo: `libdisc/qemu/cdimage.c`)

A read-only format driver modelled on `block/bochs.c`:

- **probe** is `libdisc_probe`; **open** takes the `file` child as bochs
  does (the `.cue` itself becomes `bs->file`), refuses read-write
  (`bdrv_apply_auto_read_only`) and an API-version mismatch, and calls
  `libdisc_open` on the child's filename;
- **getlength** is `sector_count × 2048`, `request_alignment` 2048;
- **co_preadv** reads per sector through `libdisc_read_cooked`; any
  libdisc error is `-EIO`;
- **`cdimage_disc(bs)`**, exported through `include/block/cdimage.h`,
  returns the handle for `atapi.c`.

**The `isodir` protocol driver lives in the same file** (§8), with the
same state and read path but no `file` child. A directory can be
neither probed nor a `file` child, so it takes a prefix, `isodir:/path`,
which `bdrv_parse_filename` strips (QEMU's precedent is vvfat's
`fat:`).

**The block layer puts a `raw` format node above a protocol driver it
resolved from a prefix**, so `cdimage_disc()` walks down through format
nodes (after `bdrv_skip_filters`). A failed walk is invisible: the
guest still reads files, and only the model's answers go missing (TOC,
READ CD, a bad sector's sense). `CDIMAGE_TRACE=1` prints every packet,
reply and sense **only** when the model was found, the `dirdisc`
check's proof.

`atapi.c` calls `cdimage_disc` under the BQL on every command. A medium
change replaces `blk_bs(s->blk)`, so the handle is never cached in
IDEState.

### 5.2 Build: patch `50-cdimage-block-driver`

- A meson option `libdisc_dir` (the directory holding `liblibdisc.a`),
  the `libdisc` dependency with the staticlib's native libraries per
  platform, `CONFIG_CDIMAGE`, and `cdimage.c` in `block_ss`.
  `atapi.c`'s disc paths are under `#ifdef CONFIG_CDIMAGE`.
- `scripts/configure-qemu.sh` runs `cargo build --release -p libdisc`
  first (no cycle: the crate has no QEMU dependency) and passes its
  output directory as `-Dlibdisc_dir` (`target/release`, or the target
  triple's under a cross or MSVC build). `prepare-qemu.sh` overlays
  `cdimage.c` into `qemu/block/`, and `cdimage.h` and `libdisc.h` into
  `qemu/include/block/`, before the patch loop.
- **meson does not track the staticlib.** `cc.find_library` makes it
  no target's input, so after a change under `libdisc/` a plain `ninja`
  keeps the old code in `qemu-system-i386`, `qemu-img` and the embed
  library (seen as `qemu-img` calling an MDS "not a disc image libdisc
  reads"). `scripts/build-libdisc.sh` runs cargo and deletes those
  targets so ninja relinks them; `scripts/build.sh` builds libdisc
  before QEMU links it.
- The staticlib is linked into every `qemu-system-<target>` and
  `libqemu-embed-<target>`. The player is Rust too, so two copies of `std`
  share the process. On macOS the export list hides libdisc's symbols;
  on Linux the copies interpose identical code. QEMU's own Rust support
  is not enabled, and cargo builds the crate outside meson.

### 5.3 ATAPI: patch `51-atapi-disc-model`

Files:

- `hw/ide/atapi.c`;
- `hw/ide/ide-internal.h` (ASC 0x11 unrecovered read error, 0x64
  illegal mode for this track, and `ide_atapi_cmd_error_ascq`);
- `include/hw/ide/ide-dev.h` (new IDEState fields, **not** in
  `vmstate_ide_drive`, so migration and snapshots with a disc attached
  are unsupported);
- `hw/ide/ide-dev.c` (`audiodev` on `ide-cd`, §5.4).

```c
bool     atapi_disc_read;          /* this transfer is served by libdisc, not blk */
uint8_t  atapi_rc_type, atapi_rc_b9, atapi_rc_b10;  /* READ CD parameters; READ(10) = 0, 0x10, 0 */
uint32_t atapi_last_lba;           /* the head, for READ SUB-CHANNEL when not playing */
uint8_t  atapi_audio_status;       /* 0x11 playing … 0x15 none */
uint32_t atapi_play_lba, atapi_play_end;
uint8_t  atapi_audio_port[4], atapi_audio_vol[4];   /* mode page 0x0E */
AudioBackend *atapi_be; SWVoiceOut *atapi_voice; QEMUTimer *atapi_play_timer;
```

Every handler asks `atapi_disc(s)` itself. New table entries, all
`CHECK_READY` (the audio ones `NONDATA` too): READ SUB-CHANNEL 0x42,
PLAY AUDIO 0x45 / 0x47 / 0x48 / 0xA5, PAUSE/RESUME 0x4B, STOP PLAY/SCAN
0x4E, MODE SELECT(10) 0x55, READ CD MSF 0xB9; START STOP UNIT 0x1B stops
audio too. With no disc the audio commands are no-ops leaving status
0x15, READ SUB-CHANNEL reports 0x15 at position 0, and READ CD MSF
behaves like READ CD.

**The transfer path.** The disc path generalises `cd_sector_size` to
whatever `libdisc_mmc_read_cd_length` returns (≤ 2744 bytes, so at least 47 sectors
fit the 131,076-byte `io_buffer`).

- **PIO.** `cd_read_sector` returns 1 when it filled the sector
  synchronously (the disc path) and 0 when it started an async block
  read. `ide_atapi_cmd_reply_end` loops on 1 instead of recursing (a
  128 KiB transfer would recurse 64 deep). `cd_read_sector_sync` has the
  same branch.
- **DMA.** `ide_atapi_disc_read_dma_cb` mirrors the stock callback,
  fills each chunk synchronously and re-enters through a bottom half
  (`replay_bh_schedule_oneshot_event`), keeping the stack flat and
  completion asynchronous. `aiocb` stays NULL, and
  `ide_atapi_dma_restart` is guarded.
- **Errors mid-transfer** end the command with the §3 sense after the
  sectors already sent, as a drive aborts on the failing sector.
- `cmd_read` (READ 10/12) checks `sector_info` over the range first. An
  audio or gap sector is 05/64/00.
- TOC, sub-channel and disc information come from the responders.
- `GET CONFIGURATION` with a disc reports the CD-ROM profile with
  features 0x001E (CD read, C2) and 0x0103 (CD audio). Past 80 minutes
  it reports the DVD-ROM profile current, with CD-ROM listed and DVD
  Read 0x001F (patch 53). Mode page 0x2A describes a 48× CD-DA drive
  (speed 8467, C2, ISRC, UPC, R–W, separate volume and mute, 256
  levels, 128 KiB buffer, and the DVD read bit past 80 minutes). Page
  0x0E holds the audio ports. MODE SELECT parses page 0x0E only and
  errors on malformed lengths (05/26/00).
- INQUIRY's product string follows `-device ide-cd,model=…`.
- `ide_cd_change_cb` stops audio and resets the status and the head.

Implementation rules:

- **A new PIO end-transfer function must be listed** in `hw/ide/core.c`'s
  `ide_is_pio_out` and `transfer_end_table`, or QEMU aborts on the first
  data word. A static one in `atapi.c` cannot be, hence the exported
  `ide_atapi_data_out_done` (MODE SELECT's data-out).
- **No speed model.** Page 0x2A claims 48× with a disc (the stock no-disc
  path says 4×), `SET CD SPEED` is a no-op, and data arrives as fast as
  the host reads it (~580 MB/s over a whole disc under TCG). QEMU's
  block throttle (`throttling.bps-read=`) reaches only the `raw` driver,
  because `atapi_disc_read_sector` bypasses the BlockBackend: a
  throttled `.iso` measures 666 KB/s, the same data as a `.cue` 10,705
  KB/s (`THROTTLE=` in `tools/cd-rate-guest-test.py`). The data does not
  depend on the rate: 34 ways of reading one range (PIO and DMA, request
  sizes, byte-count limits, 2048 and 2352, paced 1×/4×/16×) match the
  host's checksum on both drivers. A `speed=` property both drivers
  honour waits for a title that paces itself on CD reads.
- **The mode parameter header names the medium** (patch 56): byte 2 of
  every MODE SENSE reply is 01h/02h/03h from the track list, 70h with no
  medium, 71h with the tray open. Upstream's constant 70h is "no disc";
  the DOS ATAPI drivers (OAKCDROM.SYS, UIDE) copy it into MSCDEX device
  status bit 11, and a game that checks the bit (Mortal Kombat 3) refuses
  its disc on a DOS boot while the same disc passes in a Windows DOS box.
  `tools/cdprobe.asm` prints what MSCDEX reports (drive count, 150Bh, the
  IOCTL INPUT subfunctions) from any DOS.

### 5.4 CD-DA

`DEFINE_AUDIO_PROPERTIES` on `ide-cd` (the backend lives in `IDEDrive`).
Without an `audiodev` no voice is opened and a `QEMUTimer` advances the
play position at 75 sectors per second, so polling games still see
tracks complete. With one, the drive opens a voice at 44100 Hz
stereo S16 (`audio_be_open_out`), active while playing.

`atapi_audio_cb` reads audio sectors with `libdisc_read_raw`, routes
them through page 0x0E (port 0 = left: channel mask 1 L, 2 R, 3 mix,
scaled by `vol/255`; port 1 = right), writes them (`audio_be_write`)
and advances.
At the end the status is 0x13. A data sector inside a play range ends it
with 0x14. **A host read error plays as silence** (patch 55), as a real
drive plays through a bad audio sector as a dropout; before, a transient
failure off a network share stopped a game's music for good.

Commands: PLAY AUDIO(10) start LBA + 2-byte length; (12) 4-byte length;
MSF start bytes 3..5, end 6..8, exclusive, `FF:FF:FF` = the disc's end;
TRACK/INDEX through `libdisc_track_info`; PAUSE/RESUME byte 8 bit 0;
STOP → 0x15. A play starting on a data sector is 05/64/00, and
`start == end` completes at once.

**Three commands stop a play, and each family sends only one of them**
(`tools/cdaudio-guest-test.sh`, which runs `CDTEST.EXE` under
`CDIMAGE_TRACE=1`):

- **XP** brackets every play with `PAUSE, SEEK, PAUSE, PLAY` and stops
  with **START STOP UNIT** `1b 00 00 00 00`, never 0x4E.
- **Win98's `mcicda`** sends one PLAY AUDIO MSF and two SEEKs for a whole
  play / pause / stop session, and no 0x4E, 0x1B or 0x4B. **On 9x a seek
  is the stop.** MCI seeks to where playback should end and reports
  "stopped" on its own authority, so a drive that treats the seek as
  advisory plays the disc out behind a stopped MCI (11 MB of wav for a
  4-second play). A SEEK therefore ends playback and moves the head
  (patch 54).
- A **data read** does not stop audio, although real drives abandon
  playback for one: Win98's CDFS re-reads the volume descriptors several
  times a second during a play (532 READ(10)s in one session).

READ SUB-CHANNEL's position is `play_lba` while playing, paused or
completed, else the head (`atapi_last_lba`): where a play ended, a seek
was sent, or the last sector read, whichever was latest. A head that
tracked only the last *read* made XP answer "track 01, 00:00:17", its
own volume descriptor, after a stop.

The launcher and player attach the drive explicitly, on its own channel
and with the embed audiodev:

```
-drive if=none,id=cd0,media=cdrom[,file=<medium>]
-device ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,audiodev=embed0[,shelf=<file>]
```

`ide1-cd0` is the QMP id a medium change addresses
(`launcher-core/src/control.rs`, forced so a tray the guest locked
still changes).

## 6. Tests

`docs/testing.md` has the catalogue and how to run each tool. This
section says what each proves about the model.

### 6.1 Host: `discx` (`libdisc/src/bin/discx.rs`)

Built by `cargo build --release -p libdisc`.

- **`discx selftest <dir>`** (the `libdisc` check) writes synthetic
  discs and checks them **through the C API**, the boundary
  `cdimage.c` uses. The discs are `mixed.cue/.bin` (a MODE1/2352 track
  of 2000 pseudo-random sectors, an audio track with index 00 in the
  file and a 1 kHz tone, an audio track with `PREGAP`, `CATALOG`,
  `ISRC`), the same disc as CloneCD with a synthesized `.sub`, a
  MODE1/2048 cooked copy, and `plain.iso`. The cases are `msf`;
  `raw-synth` (synthesized raw equals stored raw, cooked equals cooked);
  `lec` (an unreadable and a repairable sector, cooked and raw, both
  halves of the raw rule); `repair`; `edges` (range ends, EMODE on the
  wrong type); `subq-synth` (synthesized Q equals the `.sub`, CRC on
  every frame); `toc` (formats 0–2 identical across the three images,
  format 0 on the ISO as QEMU lays it out); `ccd`; `read-cd-length`
  (the MMC-3 table); `read-cd-fill`; `dirdisc` (§8); `panic-safety` (a
  corrupt cue is an error string, never an abort).
- **`discx info` / `dump <image> <what>`** print the layout and any
  responder's bytes (`toc 0 1`, `subq 1000`, `readcd 16 0 0xf8 1`); the
  oracle for `atapi-guest`.
- **`discx scan <image>`** L-EC-checks every sector of a real dump and
  splits the failures the way a guest meets them: read anyway (EDC
  intact), read after the decoder repairs them, or unreadable, listing
  the last. On a protection band every failure must be unreadable.
  **`subscan`** does the same for stored subchannel (Q CRC failures,
  clustering, how often `subq::synthesize` reproduces the disc's
  frames).
- **`discx repair <image> <outdir>`** writes the negative control
  (§2.6b): every L-EC-failing sector's EDC, reserved gap and ECC
  regenerated over the dumped user data (SafeDisc's `0x55` stays), each
  payload and descriptor copied by basename, sync-less sectors (run-out,
  which a drive fails too) left alone. Selftest's `repair` case checks
  that the sector reads, its data is still the corrupted data, and no
  byte outside the parity fields moved.
- **`discx convert <in.iso> <out.cue> [--audio tone.wav …]`** writes an
  ISO as MODE1/2352 cue/bin with synthesized EDC/ECC and appended audio
  tracks. Guest test discs are made from the guest-tools ISO this way.
- **`discx export <image> <out.iso>`** writes the cooked view out, so
  readers that are not ours can check a folder disc (§8).
- **`discx mktree <dir>`** writes the folder-disc fixture tree (§8).

The `cdimage` check has `qemu-img info` report `"format": "cdimage"`
and `sector_count × 2048` on the selftest's cue.

**An independent EDC/ECC oracle.** Neill Corlett's `ecm` (ecm-tools
1.03, public domain; `pacman -S ecm-tools`, or `ecm.c` + `common.h` +
`banner.h` from github.com/alucryd/ecm-tools and `gcc -O2 -o ecm
ecm.c`) strips a sector's EDC/ECC only when its own regeneration
reproduces them, so `ecm mixed.bin x.ecm` reporting `Mode 1
sectors.......... 2000` proves `ecc.rs` byte-exact (1999 on `lec.bin`,
whose flipped sector does not). Its "Mode 2 form 1 sectors … 151" are
the all-zero audio pregap (ecm's false positive on zero blocks). Re-run
it after any change to `ecc.rs`.

### 6.2 Guest, DOS: `tools/atapi-guest-test.py`

A NASM `.COM` on the `x87-guest-test.py` harness drives the secondary
channel (0x170/0x376) by PIO PACKET commands and dumps every reply and
REQUEST SENSE: INQUIRY, READ TOC 0/1/2, READ CD across track boundaries,
READ(10) of the unreadable sector (03/11/05), the repairable one and
their neighbours, READ SUB-CHANNEL 1/2/3, PLAY AUDIO MSF with the
position advancing, PAUSE, both stops (each followed by status 0x15),
GET CONFIGURATION, MODE SENSE 0x2A (its medium type 03h, patch 56) /
0x0E and MODE SELECT 0x0E read back, at byte-count limits 512 and 65534. Every reply must equal `discx
dump` of the same request. Then it exercises the shelf (patch 52). This
is the `atapi-guest` check; `ATAPI_READ_ERROR=1` is `atapi-read-error`
(patch 55).

### 6.3 Guest, Windows

- **Copying a disc through the OS driver.** `tools/xp-cdimage-test.sh`
  boots XP with a converted guest-tools disc (or any `.cue` / `.ccd` /
  `.mds` / `.iso` / `isodir:`), copies it through `cdrom.sys` and
  compares every file (the `guest-cdimage` and `guest-dirdisc` checks).
  Win98 has `tools/dirdisc-guest-test.sh`.
- **CD audio.** `CDTEST.EXE` (`guest-tools/src/cdtest.c`) plays track
  2 through MCI and logs positions to `C:\2KSBOX\CDTEST.LOG`. With
  `CDTEST=`, `xp-cdimage-test.sh` records the drive's audiodev to a wav
  that must hold the 1 kHz tone, and MCI must not say `playing` after
  `stop cd`. MCI answers from the state it commanded, so the drive and
  the wav are the verdict. `tools/cdaudio-guest-test.sh` adds the trace
  (§5.4).
- **Still wanted** (doc 09): ATAPI traces from the rig's real drive while
  a protected title checks its disc (an XP SPTI logger; a filter driver
  is out of scope), and CloneCD dumps with subchannel for SecuROM.

### 6.x Which dumps carry a protection signal

`discx scan` over the real dumps (outside the repo, in
`/mnt/data2/david/Downloads/oldstuff` on the Linux box):

| Dump | Protection (from the disc's files) | What `discx scan` finds |
|---|---|---|
| `fifa2002/` (DIC and Alcohol sets) | SafeDisc 2.x (`00000001.TMP`, `DRVMGT.DLL`, `SECDRV.SYS`) | 584 sectors from LBA 811, identical in both sets |
| `AOM_D1.ccd` | SafeDisc 2.x | 580 weak sectors, LBA 825–12000: valid sync and header, `0x55` fill, wrong EDC |
| `AOM_D2.ccd` | none | clean |
| `the settlers 3/CD01.ccd` / `.cue` | VOB ProtectCD (`.ficken` in `S3.EXE`) | 697 failures, identical in both |
| `CD02` | none | only its run-out, 234254–234403 |
| The Sims (CCD), `rayman2/` (redump) | SafeDisc 1.x | 0 failures |
| `nfs_porsche/*.mds` | SafeDisc 1.x | 0 failures over 281,279 sectors |
| Crimson Skies (`C_SKIES.cue`) | SafeDisc 1.50.020 | 579 sync-less sectors, LBA 807–10018 (§2.6c) |

Settlers CD01's 697: the 538-sector band at LBA 195539–196076 (corrupt
in the data and in Q's relative timing), nine scattered singles past it
with the EDC wrong too, and 150 sync-less run-out sectors at
219692–219841 before track 02. The protection is on disc 1 only.

What the table teaches:

- **SafeDisc 2.x writes a band; 1.x discs differ** (Crimson Skies has
  one, The Sims, Rayman 2 and Porsche none). Only `discx scan` can tell.
- **DiscImageCreator / redump sets carry the band.** `/sf` fills a bad
  sector's user data with `0x55` and keeps its header and wrong parity
  (`*.img_EccEdc.txt` logs it), so the sector still fails in the guest.
- **The two FIFA 2002 sets differ outside the band.** DIC logs 64
  sectors it could not descramble (LBA 135084–135086, 161089, 223875,
  224045) that fail as `-EIO` and break an install; the Alcohol set read
  them cleanly. **Run the `.mds`**, and keep the DIC `.cue` as the
  verification fixture, because its bad-sector list provably equals the
  dumper's log.
- **Real protection is a band**, hundreds of sectors against hundreds
  of thousands clean. Whole-disc failure is a format problem, most
  likely a scrambled image: `ccd.rs` refuses `DataTracksScrambled=1`,
  but a bare `.scm` from DIC carries no flag to catch.

## 7. Milestones

| Step | Delivers | State |
|---|---|---|
| M5a | cue/bin and ISO model, EDC/ECC, Q synthesis, C API, `discx`, the driver, patches 50/51's data path, TOC, READ CD, subchannel | done |
| M5b | CD-DA (play, pause, stop, position, pages 0x0E / 0x2A, `audiodev`), swaps over QMP | done; Win98's CD Player by ear not recorded |
| M5c | CCD + `.sub` replay, the raw TOC verbatim, a SecuROM title | replay done; no SecuROM title tested |
| M5d | SafeDisc: L-EC against a real dump | done (§2.6c); guarded by selftest's `lec` and `atapi-guest` |
| M5e | MDS/MDF (+ DPM), CHD, a timing profile if StarForce needs one | MDS done without DPM; CHD and timing open |
| M5f | the disc shelf in the launcher and the guest (with M6) | done (patch 52, doc 07) |
| M5g | a host folder as a disc (§8) | done |

The open items are in `docs/tracks/m5-cdrom-backend.md`.

## 8. Folder discs: `isodir`

`isodir:/path/to/folder` serves a host directory as a read-only
ISO 9660 + Joliet volume, generated lazily inside libdisc, with no image
file written and nothing copied. `isodir.rs` is a
layout builder that emits one `Source::File` extent per host file
(`Cooked2048`, with sync, header and EDC/ECC synthesized as for an
`.iso`) and `Source::Mem` extents for the metadata (a few hundred KB for
tens of thousands of files). `read_cooked` / `read_raw`, the responders,
patch 51, `qemu-img` and the shelf are unchanged, and `libdisc_open` on
a directory is the whole interface (`LIBDISC_API_VERSION` stays 1). The
model pieces it needed (`Source::Mem`, `eof_pad`, lazy handles,
`extent_at`'s binary search) are in §2.1. The builder still opens each
file once, so a missing file fails at open time.

### Decisions

1. **Read-only.** Files leave a guest through a FAT scratch disk, never
   vvfat read-write.
2. **Snapshotted at insert**, like a burned disc. Host edits appear on
   the next eject and insert (the shelf's LOAD, whose UNIT ATTENTION
   makes the guest's CD file system drop its cache); a file changed
   under a mounted disc reads as `EMEDIUM`, never torn (§2.1).
3. **Generated lazily, never written out.** `discx export` writes a real
   ISO only as the test oracle.
4. **Joliet for Windows and ISO 9660 level 1 (8.3) for DOS** in one
   volume, both trees pointing at the same extents. No Rock Ridge,
   because no guest reads it.
5. **A protocol prefix, not a probe** (§5.1). Everything that inspects a
   medium string must understand the prefix: the shelf's C side strips
   it before `access()`ing the host path (`cdshelf_host_path()`), or
   every folder shows as `[missing on the host]`. Patch 52's LOAD passes
   the name with no format and the block layer resolves the prefix.
6. **No QEMU patch.** The driver is in our overlay file `cdimage.c`.
7. **Not bootable.** There is no El Torito. A boot image named in the
   folder and a catalogue at a fixed LBA would add it without a
   redesign.

### Layout

One Mode 1 data track, LBA 0 to the lead-out:

| LBA | Contents |
|---|---|
| 0–15 | system area, zero |
| 16 | Primary Volume Descriptor (type 1, `CD001`, version 1) |
| 17 | Joliet Supplementary VD (type 2, escape `%/E`, UCS-2 level 3) |
| 18 | Volume Descriptor Set Terminator (type 255) |
| 19… | L and M path tables, primary then Joliet |
| … | directory records: the primary tree, then the Joliet tree |
| … | file extents, 2048-aligned, one contiguous run per file |
| last | 150 zero sectors (`TAIL_PAD`: guest drivers read ahead past the last extent, and every real ISO has it) |

Rules that are easy to get wrong:

- **Both-endian fields.** A directory record's extent LBA and length are
  8 bytes each, LE then BE. The PVD's path-table pointers are four
  4-byte fields (L, optional L, M, optional M), the optional ones 0.
- **Dates.** Directory records carry 7 binary bytes (year − 1900 … GMT
  offset in 15-minute units) from the file's mtime. The PVD's volume
  dates are the 17-byte digit form.
- **Every directory starts with `.` and `..`** (identifiers `0x00`,
  `0x01`), and the root's `..` is the root. A record never straddles a
  sector, so pad to the next.
- **Path tables** are ordered by level, parent number, identifier. The
  root is 1 and a parent's number is ≤ its child's. Joliet has its own
  pair of the same shape.
- **Primary names** are level 1: `A–Z 0–9 _`, 8.3, files `;1`,
  directories no extension or version, anything else `_`. Collisions
  become `~1`, `~2`… decided from a **sorted** listing, or which name
  gets `~1` depends on readdir order and two runs differ. This is the
  tree MSCDEX reads, and why level 2 is not used.
- **Joliet names** are UCS-2 BE, ≤ 64 characters, with `* / : ; ? \`
  and controls → `_` and `;1` appended. A name that is not UTF-8 is
  skipped, not mangled.
- **Deterministic order.** Directories go breadth-first, entries by
  primary identifier, and files are laid out in the same walk. Two opens
  of an unchanged tree are byte-identical.
- **An empty file** has length 0 and owns no sectors, but gets the first
  file extent's LBA rather than 0 (a reader may drop an extent inside
  the system area).
- **Volume space size** = every sector including the system area and
  the tail padding = the lead-out = READ CAPACITY.

### Limits (`isodir.rs` constants)

- **Refused**, with a message naming the path: a file of 4 GiB or more
  (single extent only; multi-extent support differs between Windows
  versions), nesting deeper than 30, a symlink loop, and a tree larger
  than **4,173,824 sectors (a dual-layer DVD-9, 8.1 GiB)**. The size is
  measured before anything is laid out, and the message, carrying both
  sizes, reaches the shelf's error line through `cdimage.c`'s
  `error_setg`.
- **Warned, then carried on**: deeper than 8 levels (ISO 9660's limit;
  Windows reads the Joliet tree, MSCDEX may not), a file of 2 GiB or
  more (unreliable on Win98), more than 65,535 directories. Symlinks are
  followed for files and directories. Sockets, FIFOs and devices are
  skipped with a warning.
- **CD or DVD.** Up to an 80-minute CD the medium is a CD. Above that
  the drive reports a DVD-ROM (patch 53; `READ DVD STRUCTURE` needed
  nothing), so the CD's MSF ceiling (~878 MiB) no longer bounds a
  folder. Win98 and XP both read marker files at 703 MiB, 878 MiB,
  2 GiB, 4 GiB and 7.8 GiB of one folder disc (`BIG=1
  tools/dirdisc-guest-test.sh`).

**Validate where the disc is built, and make the arithmetic after it
unable to fail.** A 34 GiB folder once got past the builder and panicked
in MSF on the TOC's lead-out (`LIBDISC_EIO` on an unrelated command in
the guest). The `msf` selftest pins both ends of the clamp.

### Tests

- `discx selftest`'s `dirdisc` case covers a hostile tree (an empty
  file, 300 entries in one directory, 9 levels, accents, spaces, `*`,
  colliding 8.3 forms, a 64-character name), every refusal, the
  stale-file `EMEDIUM` case, and a byte-identical second open. `discx
  mktree <dir>` writes the fixture tree (plus files of exactly one
  sector and one sector plus a byte, 3 MB of pseudo-random data and an
  empty directory).
- In the `dirdisc` check, xorriso extracts the exported volume
  identical, and bsdtar with Joliet off reads the 8.3 tree, including
  which of the two colliding names holds which contents. `qemu-img`
  names `isodir` and converts byte-identically to `discx export`.
  SeaBIOS probing the drive under `CDIMAGE_TRACE=1` proves
  `cdimage_disc()` found the model through the `raw` node, against a
  plain `.iso` as the control.
- `guest-dirdisc` (XP), `dirshelf` (the launcher's verbs) and
  `tools/dirdisc-guest-test.sh` (Win98).
- The stale-file rule is checked host-side only; in a guest it would
  race XP's read-ahead. There is no MSCDEX leg (the DOS tools' FreeDOS
  floppy has no CD driver); bsdtar with Joliet off stands in.
