#!/usr/bin/env python3
"""In-guest test of the ATAPI drive on a cdimage disc (patch 51, doc 17 §6.2).

Builds a DOS program (NASM, .COM) that talks to the secondary IDE channel
(0x170/0x376: -cdrom is its master) directly by PIO (PACKET commands, the
DRQ/byte-count loop, REQUEST SENSE on CHECK CONDITION) and hex-dumps every
reply on COM1. Boots it on the FreeDOS test floppy under our
qemu-system-i386 with the selftest's damaged image (lec.cue: sector 1000
unreadable, sector 1010 one wrong byte the drive's P/Q decoder repairs) as
the CD and compares every reply with `discx dump` of the same request: the
guest must see exactly the bytes libdisc computed, at byte-count limits 512
(every reply split into elementary transfers) and 65534. Also checks the
audio position replies around PLAY / PAUSE / RESUME / STOP, both stops
(STOP PLAY/SCAN and START STOP UNIT, the one Windows sends).

The same run covers the disc shelf (patch 52, cdshelf/cdshelf_proto.h): the
drive is given a `shelf=` file, and the vendor opcode 0xD0 is driven through
the same PIO program: LIST at several allocation lengths, a bad subcommand,
a slot that isn't there, then LOAD / EJECT with the medium actually read
before and after, so "the tray changed" is proven by the sectors the guest
sees and not just by a good status.

A second boot then runs the real DOS program (guest-tools/src/cdshelf.asm,
the one shipped on the guest-tools ISO) against the same shelf, with its
output redirected to COM1: the list it prints, and a load it performs, are
checked the same way. That is the DOS half of the in-guest CDSHELF program;
the Win98/XP half is guest-tools/src/cdshelf.c.

    tools/atapi-guest-test.py            # needs nasm, mtools, build/qemu, target/release/discx
    ATAPI_READ_ERROR=1 tools/atapi-guest-test.py

**ATAPI_READ_ERROR=1** (patch 55, the `atapi-read-error` check) runs the
same battery with four sectors of track 2 (2152..2155) unreadable on the
host: tools/read-error-inject.c fails every pread64 of lec.bin there with
EIO, in QEMU alone, so `discx dump` still answers for the whole disc. A
CD player reads through an unreadable audio sector as a dropout, so every
audio check must still pass (the play through 2150..2300 advancing, the
ten-sector play completing at 2160), and QEMU's log must say the sectors
were played as silence. Without patch 55 the play stopped at 2152 with
status 0x14 and the music was off for good (a disc on a Samba share).
The shelf stage is skipped in this mode.

The FreeDOS 1.3 boot floppy (build/images/144m/x86BOOT.img, git-ignored)
is fetched by tools/x87-guest-test.py (same harness).
"""
import os
import shutil
import subprocess
import sys
import qemuhost  # tools/qemuhost.py: the platform's QEMU and QMP address
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = qemuhost.qemu(ROOT)   # this checkout's build, or $QEMU_BIN
DISCX = qemuhost.rust_bin(ROOT, "discx")
FLOPPY = os.path.join(ROOT, "build/images/144m/x86BOOT.img")
OUT = os.path.join(ROOT, "build/atapi-guest")
DISC_DIR = os.path.join(ROOT, "build/test/disc")
DISC = os.path.join(DISC_DIR, "lec.cue")       # mixed.cue, sector 1000 unreadable, 1010 repairable
TOC_DISC = os.path.join(DISC_DIR, "mixed.cue")  # same layout, for the responders
SHELF = os.path.join(OUT, "shelf.txt")         # what -device ide-cd,shelf= reads
CD_ID = "ide1-cd0"                             # the id a medium change addresses

# the selftest disc (discx.rs): track 1 data 0..2000, track 2 audio index 0 at
# 2000, index 1 at 2150, track 3 index 0 at 5150 / index 1 at 5300, lead-out 6800
LEADOUT = 6800
T2 = 2150


def ensure_prereqs():
    missing = [t for t in ("nasm", "mcopy") if shutil.which(t) is None]
    if missing:
        raise SystemExit("missing %s (nasm, mtools)" % ", ".join(missing))
    for f, hint in ((QEMU, "build QEMU"), (DISCX, "cargo build --release -p libdisc"),
                    (FLOPPY, "run tools/x87-guest-test.py once (fetches the FreeDOS floppy)")):
        if not os.path.exists(f):
            raise SystemExit("%s missing: %s" % (f, hint))
    if not os.path.exists(DISC):
        subprocess.run([DISCX, "selftest", DISC_DIR], check=True, stdout=subprocess.DEVNULL)


# ---------------------------------------------------------------- the test list

def msf(lba):
    a = lba + 150
    return [a // 4500, (a // 75) % 60, a % 75]


def be32(v):
    return [(v >> 24) & 255, (v >> 16) & 255, (v >> 8) & 255, v & 255]


def be16(v):
    return [(v >> 8) & 255, v & 255]


def pkt(*b):
    p = list(b) + [0] * (12 - len(b))
    assert len(p) == 12
    return p


def read_toc(fmt, msf_bit, start, alloc=1024):
    return pkt(0x43, msf_bit << 1, fmt, 0, 0, 0, start, *be16(alloc))


def read_sub(fmt, msf_bit=0, subq=1, track=0, alloc=64):
    return pkt(0x42, msf_bit << 1, subq << 6, fmt, 0, 0, track, *be16(alloc))


def read_cd(lba, n, ty, b9, b10):
    return pkt(0xBE, ty << 2, *be32(lba), (n >> 16) & 255, (n >> 8) & 255, n & 255, b9, b10)


def read_cd_msf(start, end, ty, b9, b10):
    return pkt(0xB9, ty << 2, 0, *msf(start), *msf(end), b9, b10)


def read10(lba, n):
    return pkt(0x28, 0, *be32(lba), 0, *be16(n))


# ---------------------------------------------------------------- the disc shelf

# cdshelf/cdshelf_proto.h. The shelf the drive is given below: slot 0 is the
# disc the machine boots with (lec.cue, sector 1000 deliberately unreadable), slot
# 1 the same layout without the corruption, which is how a LOAD is proven to
# have actually changed the medium and not just returned a good status. Slot 2's
# label is longer than the protocol's 64 bytes (it must come back truncated, not
# split across the next entry) and slot 3's file does not exist (it must be
# listed, flagged MISSING, rather than silently dropped).
#
# The boot disc is the drive's `file=` at start-up, never LOADed by the guest,
# and the first listing must still report slot 0 as the disc in the drive: the
# drive works that out from the medium itself. It is passed under a different
# spelling (BOOT_DISC) so that only the file's identity, not its name, can match.
# Not on Windows: its stat reports no inode, so the drive compares the strings
# there (patch 52), and the launcher writes one spelling on both sides.
# Slots 1 and 2 are one file under two labels, so both are "in the drive" when
# either is loaded.
SHELF_VERSION = 1
SHELF_HDR = 12
SHELF_ENTRY = 68
SHELF_LABEL_MAX = 64
SHELF_NO_SLOT = 0xFFFF
SHELF_FLAG_LOADED = 0x01
SHELF_FLAG_MISSING = 0x02
LONG_LABEL = "A label longer than the protocol's sixty-four bytes, which must come back truncated"
BOOT_DISC = os.path.join(DISC_DIR, "lec.cue") if qemuhost.WINDOWS else os.path.join(DISC_DIR, ".", "lec.cue")
SHELF_DISCS = [
    ("Selftest disc", DISC),
    ("Mixed disc", TOC_DISC),
    (LONG_LABEL, TOC_DISC),
    ("Not on the host", os.path.join(OUT, "not-here.iso")),
]


def cdshelf(sub, slot=0, alloc=0):
    return pkt(0xD0, sub, *be16(slot), 0, 0, 0, *be16(alloc))


# expectations: ("dump", [discx args...] per sector / once, alloc), ("err", key, asc, ascq),
# ("len", n), ("pos", name) for the audio position checks, None = print only
TESTS = [
    ("inquiry", pkt(0x12, 0, 0, 0, 96), None),
    ("read capacity", pkt(0x25), ("bytes", be32(LEADOUT - 1) + be32(2048))),
    ("toc 0 lba", read_toc(0, 0, 0), ("dump", [["toc", "0", "0", "0"]], 1024)),
    ("toc 0 msf", read_toc(0, 1, 0), ("dump", [["toc", "0", "1", "0"]], 1024)),
    ("toc 0 from 3", read_toc(0, 0, 3), ("dump", [["toc", "0", "0", "3"]], 1024)),
    ("toc 0 lead-out", read_toc(0, 0, 0xAA), ("dump", [["toc", "0", "0", "170"]], 1024)),
    ("toc 0 short alloc", read_toc(0, 0, 0, 12), ("dump", [["toc", "0", "0", "0"]], 12)),
    ("toc 0 from 4", read_toc(0, 0, 4), ("err", 5, 0x24, 0)),
    ("toc 1", read_toc(1, 0, 0), ("dump", [["toc", "1", "0", "0"]], 1024)),
    ("toc 2", read_toc(2, 0, 0), ("dump", [["toc", "2", "0", "0"]], 1024)),
    ("toc 2 old-style byte 9", pkt(0x43, 0, 0, 0, 0, 0, 0, 4, 0, 0x80), ("dump", [["toc", "2", "0", "0"]], 1024)),
    ("toc 3", read_toc(3, 0, 0), ("err", 5, 0x24, 0)),
    ("disc information", pkt(0x51, 0, 0, 0, 0, 0, 0, 0, 34), ("dump", [["discinfo"]], 34)),
    ("read cd 0 raw", read_cd(0, 1, 0, 0xF8, 0), ("dump", [["readcd", "0", "0", "0xf8", "0"]], None)),
    ("read cd 16 raw", read_cd(16, 1, 0, 0xF8, 0), ("dump", [["readcd", "16", "0", "0xf8", "0"]], None)),
    ("read cd 16 cooked", read_cd(16, 1, 2, 0x10, 0), ("dump", [["readcd", "16", "2", "0x10", "0"]], None)),
    ("read cd 16 header+user", read_cd(16, 1, 2, 0x30, 0), ("dump", [["readcd", "16", "2", "0x30", "0"]], None)),
    ("read cd 16 raw+sub raw", read_cd(16, 1, 0, 0xF8, 1), ("dump", [["readcd", "16", "0", "0xf8", "1"]], None)),
    ("read cd 16 raw+sub q", read_cd(16, 1, 0, 0xF8, 2), ("dump", [["readcd", "16", "0", "0xf8", "2"]], None)),
    ("read cd 16 sub q only", read_cd(16, 1, 0, 0x00, 2), ("dump", [["readcd", "16", "0", "0x00", "2"]], None)),
    ("read cd 1999 raw", read_cd(1999, 1, 0, 0xF8, 0), ("dump", [["readcd", "1999", "0", "0xf8", "0"]], None)),
    ("read cd 2150 audio", read_cd(T2, 1, 1, 0xF8, 0), ("dump", [["readcd", str(T2), "1", "0xf8", "0"]], None)),
    ("read cd 2150 audio+sub", read_cd(T2, 1, 0, 0xF8, 1), ("dump", [["readcd", str(T2), "0", "0xf8", "1"]], None)),
    ("read cd 2150 as mode 1", read_cd(T2, 1, 2, 0xF8, 0), ("err", 5, 0x64, 0)),
    ("read cd 16 as audio", read_cd(16, 1, 1, 0xF8, 0), ("err", 5, 0x64, 0)),
    ("read cd 14..17 raw", read_cd(14, 3, 0, 0xF8, 0), ("dump", [["readcd", str(l), "0", "0xf8", "0"] for l in (14, 15, 16)], None)),
    ("read cd bad byte 9", read_cd(16, 1, 2, 0x28, 0), ("err", 5, 0x24, 0)),
    ("read cd past the end", read_cd(LEADOUT, 1, 0, 0xF8, 0), ("err", 5, 0x21, 0)),
    ("read cd nothing", read_cd(16, 1, 0, 0x00, 0), ("len", 0)),
    # Sector 1000's damage is past what the decoder can repair, so *every* read
    # of it is a MEDIUM ERROR, the raw one included. A drive does not hand over
    # an unreadable sector just because the CDB asked for its EDC/ECC. That was
    # the bug Crimson Skies' SafeDisc 1.x found (doc 17 §2.6c): it reads its
    # protection band with exactly this CDB and it is the error it is looking
    # for. Asking for C2 error flags is the one shape that does deliver the
    # bytes, because that is how a dumping tool gets them out of a real drive.
    ("read cd 1000 raw (flipped)", read_cd(1000, 1, 2, 0xF8, 0), ("err", 3, 0x11, 5)),
    ("read cd 1000 raw+c2", read_cd(1000, 1, 2, 0xFA, 0), ("dump", [["readcd", "1000", "2", "0xfa", "0"]], None)),
    ("read cd 1000 cooked", read_cd(1000, 1, 2, 0x10, 0), ("err", 3, 0x11, 5)),
    ("read cd msf 16", read_cd_msf(16, 17, 0, 0xF8, 0), ("dump", [["readcd", "16", "0", "0xf8", "0"]], None)),
    ("read cd msf 20..22", read_cd_msf(20, 22, 2, 0x10, 0), ("dump", [["readcd", "20", "2", "0x10", "0"], ["readcd", "21", "2", "0x10", "0"]], None)),
    ("read cd msf empty", read_cd_msf(16, 16, 0, 0xF8, 0), ("len", 0)),
    ("read10 999", read10(999, 1), ("dump", [["readcooked", "999"]], None)),
    ("read10 1001", read10(1001, 1), ("dump", [["readcooked", "1001"]], None)),
    ("read10 1000 (flipped)", read10(1000, 1), ("err", 3, 0x11, 5)),
    # sector 1010 has one wrong byte, which a drive's L-EC decoder repairs before
    # it hands the user data over: the guest must get the sector, not a sense, and
    # a raw request must still deliver the damaged bytes as they are stored
    ("read10 1010 (repaired by the decoder)", read10(1010, 1), ("dump", [["readcooked", "1010"]], None)),
    ("read cd 1010 cooked (repaired)", read_cd(1010, 1, 2, 0x10, 0), ("dump", [["readcooked", "1010"]], None)),
    ("read cd 1010 raw (still as stored)", read_cd(1010, 1, 2, 0xF8, 0), ("dump", [["readcd", "1010", "2", "0xf8", "0"]], None)),
    ("read10 999..1002 (flipped inside)", read10(999, 3), ("err", 3, 0x11, 5)),
    ("read10 20..24", read10(20, 4), ("dump", [["readcooked", str(l)] for l in (20, 21, 22, 23)], None)),
    ("read10 audio", read10(T2, 1), ("err", 5, 0x64, 0)),
    ("read10 past the end", read10(LEADOUT - 1, 2), ("err", 5, 0x21, 0)),
    ("subq 1 after read10 23", read_sub(1), ("dump", [["subq", "23", "1", "0", "0"]], 64)),
    ("subq 1 msf", read_sub(1, msf_bit=1), ("dump", [["subq", "23", "1", "1", "0"]], 64)),
    ("read cd 2200", read_cd(2200, 1, 1, 0xF8, 0), ("dump", [["readcd", "2200", "1", "0xf8", "0"]], None)),
    ("subq 1 after read cd 2200", read_sub(1), ("dump", [["subq", "2200", "1", "0", "0"]], 64)),
    ("subq 2 mcn", read_sub(2), ("dump", [["subq", "2200", "2", "0", "0"]], 64)),
    ("subq 3 isrc track 2", read_sub(3, track=2), ("dump", [["subq", "2200", "3", "0", "2"]], 64)),
    ("subq 3 isrc track 1", read_sub(3, track=1), ("dump", [["subq", "2200", "3", "0", "1"]], 64)),
    ("subq header only", read_sub(1, subq=0), ("bytes", [0, 0x15, 0, 0])),
    ("subq format 4", read_sub(4), ("err", 5, 0x24, 0)),
    ("get configuration", pkt(0x46, 0, 0, 0, 0, 0, 0, 0, 64), ("conf",)),
    ("get configuration from 001E", pkt(0x46, 0, 0, 0x1E, 0, 0, 0, 0, 64), ("bytes", [0, 0, 0, 0x14, 0, 0, 0, 8, 0, 0x1E, 0x0B, 4, 2, 0, 0, 0, 1, 3, 7, 4, 7, 0, 1, 0])),
    ("get configuration 0103 only", pkt(0x46, 2, 1, 3, 0, 0, 0, 0, 64), ("bytes", [0, 0, 0, 0x0C, 0, 0, 0, 8, 1, 3, 7, 4, 7, 0, 1, 0])),
    ("get configuration from 002D", pkt(0x46, 0, 0, 0x2D, 0, 0, 0, 0, 64), ("bytes", [0, 0, 0, 0x0C, 0, 0, 0, 8, 1, 3, 7, 4, 7, 0, 1, 0])),
    ("get configuration from 0200", pkt(0x46, 0, 2, 0, 0, 0, 0, 0, 64), ("bytes", [0, 0, 0, 4, 0, 0, 0, 8])),
    ("get configuration short alloc", pkt(0x46, 0, 0, 0, 0, 0, 0, 0, 12), ("bytes", [0, 0, 0, 0x1C, 0, 0, 0, 8, 0, 0, 3, 4])),
    ("get configuration rt 3", pkt(0x46, 3, 0, 0, 0, 0, 0, 0, 64), ("err", 5, 0x24, 0)),
    ("mode sense 2a", pkt(0x5A, 0, 0x2A, 0, 0, 0, 0, 0, 64), ("page2a",)),
    ("mode sense 0e", pkt(0x5A, 0, 0x0E, 0, 0, 0, 0, 0, 64), ("page0e",)),
    # audio: play 2150..2300 (2 s), positions advance, pause holds, resume, stop
    ("play msf 2150..2300", pkt(0x47, 0, 0, *msf(T2), *msf(T2 + 150)), ("len", 0)),
    ("subq playing 1", read_sub(1), ("pos", "playing1")),
    ("delay", 3, None),
    ("subq playing 2", read_sub(1), ("pos", "playing2")),
    ("pause", pkt(0x4B), ("len", 0)),
    ("subq paused 1", read_sub(1), ("pos", "paused1")),
    ("delay", 2, None),
    ("subq paused 2", read_sub(1), ("pos", "paused2")),
    ("resume", pkt(0x4B, 0, 0, 0, 0, 0, 0, 0, 1), ("len", 0)),
    ("subq resumed", read_sub(1), ("pos", "resumed")),
    ("stop", pkt(0x4E), ("len", 0)),
    ("subq stopped", read_sub(1), ("pos", "stopped")),
    ("play msf 10 sectors", pkt(0x47, 0, 0, *msf(T2), *msf(T2 + 10)), ("len", 0)),
    ("delay", 5, None),
    ("subq completed", read_sub(1), ("pos", "completed")),
    ("play audio(10) on data", pkt(0x45, 0, *be32(16), 0, 0, 10), ("err", 5, 0x64, 0)),
    ("play audio(12) past the end", pkt(0xA5, 0, *be32(LEADOUT - 5), *be32(10)), ("err", 5, 0x21, 0)),
    ("play track 3", pkt(0x48, 0, 0, 0, 3, 1, 0, 3, 1), ("len", 0)),
    ("subq playing track 3", read_sub(1), ("pos", "track3")),
    ("stop", pkt(0x4E), ("len", 0)),
    # A guest's Stop button is START STOP UNIT, not STOP PLAY/SCAN: XP's
    # mcicda sends 0x1b and never 0x4e (traced), so a drive that
    # ends playback on 0x4e alone plays the track out after the button.
    ("play track 3 again", pkt(0x48, 0, 0, 0, 3, 1, 0, 3, 1), ("len", 0)),
    ("subq playing track 3 again", read_sub(1), ("pos", "track3again")),
    ("start stop unit: stop", pkt(0x1B), ("len", 0)),
    ("subq stopped by start stop unit", read_sub(1), ("pos", "sspstopped")),
    # MODE SELECT(10) page 0E: port 0 <- right at half volume, port 1 <- both muted; read back
    ("mode select 0e", (pkt(0x55, 0x10, 0, 0, 0, 0, 0, 0, 24), [0, 22, 0, 0, 0, 0, 0, 0, 0x0E, 14, 4, 0, 0, 0, 0, 0, 2, 128, 3, 0, 0, 0, 0, 0]), ("len", 0)),
    ("mode sense 0e after select", pkt(0x5A, 0, 0x0E, 0, 0, 0, 0, 0, 64), ("bytes-at", 16, [2, 128, 3, 0, 0, 0, 0, 0])),
    ("mode select 0e restore", (pkt(0x55, 0x10, 0, 0, 0, 0, 0, 0, 24), [0, 22, 0, 0, 0, 0, 0, 0, 0x0E, 14, 4, 0, 0, 0, 0, 0, 1, 255, 2, 255, 0, 0, 0, 0]), ("len", 0)),
    ("mode sense 0e restored", pkt(0x5A, 0, 0x0E, 0, 0, 0, 0, 0, 64), ("page0e",)),
    ("mode select short list", (pkt(0x55, 0x10, 0, 0, 0, 0, 0, 0, 4), [0, 0, 0, 0]), ("err", 5, 0x1a, 0)),
    # the disc shelf (patch 52). Listing first, then a real medium change and
    # back again. The sequence has to end on slot 0 (the disc every test above
    # expects), because the whole table runs a second time at the other BCL.
    # the boot disc is in the drive without any LOAD (see SHELF_DISCS)
    ("cdshelf header only", cdshelf(0, alloc=SHELF_HDR), ("shelf", 0, 0)),
    ("cdshelf list", cdshelf(0, alloc=SHELF_HDR + 4 * SHELF_ENTRY), ("shelf", 4, 0)),
    ("cdshelf list one entry", cdshelf(0, alloc=SHELF_HDR + SHELF_ENTRY), ("shelf", 1, 0)),
    ("cdshelf list partial entry", cdshelf(0, alloc=SHELF_HDR + SHELF_ENTRY + 40), ("shelf", 1, 0)),
    ("cdshelf list alloc below the header", cdshelf(0, alloc=4), ("len", 4)),
    ("cdshelf bad subcommand", cdshelf(0x7F), ("err", 5, 0x24, 0)),
    ("cdshelf load past the end", cdshelf(1, slot=4), ("err", 5, 0x24, 0)),
    # a disc the host cannot open is refused outright, not accepted and then
    # dropped in a bottom half where the guest would never hear about it
    ("cdshelf load a disc the host lost", cdshelf(1, slot=3), ("err", 2, 0x3a, 0)),
    ("read10 1000 still the flipped disc", read10(1000, 1), ("err", 3, 0x11, 5)),
    ("cdshelf load slot 1", cdshelf(1, slot=1), ("len", 0)),
    ("delay", 4, None),
    # a medium change is announced as "no medium" and then UNIT ATTENTION, one
    # command each, before the new disc can be read (ide_atapi_cmd())
    ("read10 after load: medium removed", read10(20, 1), ("err", 2, 0x3a, 0)),
    ("read10 after load: unit attention", read10(20, 1), ("err", 6, 0x28, 0)),
    ("read10 1000 on the loaded disc", read10(1000, 1), ("dump", [["readcooked", "1000"]], None, TOC_DISC)),
    ("cdshelf list after load", cdshelf(0, alloc=SHELF_HDR + 4 * SHELF_ENTRY), ("shelf", 4, 1)),
    ("cdshelf eject", cdshelf(2), ("len", 0)),
    ("delay", 4, None),
    ("read10 with an empty tray", read10(20, 1), ("err", 2, 0x3a, 0)),
    ("cdshelf list with an empty tray", cdshelf(0, alloc=SHELF_HDR + 4 * SHELF_ENTRY), ("shelf", 4, SHELF_NO_SLOT)),
    ("cdshelf load slot 0 back", cdshelf(1, slot=0), ("len", 0)),
    ("delay", 4, None),
    ("read10 after reload: medium removed", read10(20, 1), ("err", 2, 0x3a, 0)),
    ("read10 after reload: unit attention", read10(20, 1), ("err", 6, 0x28, 0)),
    ("read10 1000 flipped again", read10(1000, 1), ("err", 3, 0x11, 5)),
]

# ---------------------------------------------------------------- asm

ASM = r"""
org 100h
bits 16
%define BASE 170h
%define CTRL 376h

start:
    mov ax, cs
    add ax, 1000h
    mov [bufseg], ax
    mov si, str_hello
    call puts
    mov word [bcl], 512
    call run_table
    mov word [bcl], 65534
    call run_table
    mov si, str_done
    call puts
    int 20h

run_table:
    mov si, str_bcl
    call puts
    mov ax, [bcl]
    call put_hex16
    call newline
    mov si, table
.next:
    lodsb
    cmp al, 0
    je .done
    cmp al, 2
    je .delay
    cmp al, 3
    je .dataout
    push si
    call print_cmd
    pop si
    push si
    mov word [outlen], 0
    call do_packet
    pop si
    add si, 12
    jmp .next
.dataout:               ; packet, then a byte count and the payload to send
    push si
    call print_cmd
    pop si
    push si
    mov al, [si + 12]
    mov ah, 0
    mov [outlen], ax
    lea ax, [si + 13]
    mov [outptr], ax
    call do_packet
    pop si
    add si, 13
    add si, [outlen]
    jmp .next
.delay:
    lodsb
    call wait_ticks
    jmp .next
.done:
    ret

print_cmd:              ; si = packet
    push si
    mov si, str_cmd
    call puts
    pop si
    mov cx, 12
.l: lodsb
    call put_hex8
    mov al, ' '
    call putc
    loop .l
    call newline
    ret

do_packet:              ; si = packet: LEN + DATA lines, or ERR + SENSE
    call send_packet
    jc .err
    mov si, str_len
    call puts
    mov ax, [total]
    call put_hex16
    call newline
    call print_data
    ret
.err:
    mov si, str_err
    call puts
    mov al, [last_status]
    call put_hex8
    mov al, ' '
    call putc
    mov al, [last_error]
    call put_hex8
    call newline
    mov si, pkt_sense
    call send_packet
    jc .sense_failed
    mov si, str_sense
    call puts
    call print_line_data
    ret
.sense_failed:
    mov si, str_sense_fail
    call puts
    ret

send_packet:            ; ds:si = packet -> carry on error; [total] = bytes in bufseg:0
    mov word [total], 0
    mov dx, BASE+6
    mov al, 0A0h        ; master
    out dx, al
    call wait_bsy_clear
    jc .timeout
    mov dx, CTRL
    mov al, 02h         ; nIEN: no interrupts, we poll
    out dx, al
    mov dx, BASE+1
    xor al, al          ; PIO
    out dx, al
    mov dx, BASE+4
    mov al, [bcl]
    out dx, al
    inc dx
    mov al, [bcl+1]
    out dx, al
    mov dx, BASE+7
    mov al, 0A0h        ; PACKET
    out dx, al
    call wait_drq
    jc .fail
    mov dx, BASE
    mov cx, 6
    rep outsw
    mov ax, [bufseg]
    mov es, ax
    xor di, di
.loop:
    call wait_bsy_clear
    jc .timeout
    mov dx, BASE+7
    in al, dx
    mov [last_status], al
    test al, 01h
    jnz .fail
    test al, 08h
    jz .done
    mov dx, BASE+2      ; interrupt reason: IO set = data to the host, clear = the drive wants data
    in al, dx
    test al, 02h
    jz .out
    mov dx, BASE+4
    in al, dx
    mov cl, al
    inc dx
    in al, dx
    mov ch, al
    add [total], cx
    inc cx
    shr cx, 1
    mov dx, BASE
    rep insw
    jmp .loop
.done:
    clc
    ret
.out:                   ; data-out phase: send [outlen] bytes from [outptr] (the drive's count)
    mov dx, BASE+4
    in al, dx
    mov cl, al
    inc dx
    in al, dx
    mov ch, al
    cmp cx, [outlen]
    jbe .outn
    mov cx, [outlen]
.outn:
    inc cx
    shr cx, 1
    push si
    mov si, [outptr]
    mov dx, BASE
    rep outsw
    pop si
    jmp .loop
.fail:
    mov dx, BASE+1
    in al, dx
    mov [last_error], al
    stc
    ret
.timeout:
    mov byte [last_status], 0FFh
    mov byte [last_error], 0FFh
    stc
    ret

wait_bsy_clear:
    push ecx
    mov ecx, 8000000
.l: mov dx, BASE+7
    in al, dx
    test al, 80h
    jz .ok
    dec ecx
    jnz .l
    pop ecx
    stc
    ret
.ok:
    pop ecx
    clc
    ret

wait_drq:               ; BSY clear and DRQ or ERR; carry on ERR / timeout
    push ecx
    mov ecx, 8000000
.l: mov dx, BASE+7
    in al, dx
    test al, 80h
    jnz .again
    test al, 09h
    jnz .got
.again:
    dec ecx
    jnz .l
    pop ecx
    mov byte [last_status], 0FFh
    stc
    ret
.got:
    pop ecx
    mov [last_status], al
    test al, 01h
    jnz .err
    clc
    ret
.err:
    stc
    ret

print_data:             ; [total] bytes from bufseg:0 as DATA lines of 32
    mov cx, [total]
    mov ax, [bufseg]
    push ds
    mov ds, ax
    xor si, si
.line:
    test cx, cx
    jz .end
    push si
    mov si, str_data
    call puts
    pop si
    mov bx, 32
.b: lodsb
    call put_hex8
    dec cx
    jz .eol
    dec bx
    jnz .b
.eol:
    call newline
    jmp .line
.end:
    pop ds
    ret

print_line_data:        ; [total] bytes on one line
    mov cx, [total]
    mov ax, [bufseg]
    push ds
    mov ds, ax
    xor si, si
.b: lodsb
    call put_hex8
    loop .b
    pop ds
    call newline
    ret

wait_ticks:             ; al = BIOS ticks (18.2 Hz)
    movzx cx, al
    push es
    xor ax, ax
    mov es, ax
.t: mov ax, [es:046Ch]
.w: cmp ax, [es:046Ch]
    je .w
    loop .t
    pop es
    ret

putc:                   ; al -> COM1
    push ax
    push dx
    mov dx, 3FDh
.w: in al, dx
    test al, 20h
    jz .w
    pop dx
    pop ax
    push dx
    mov dx, 3F8h
    out dx, al
    pop dx
    ret
puts:                   ; cs:si asciiz
    cs lodsb
    test al, al
    jz .d
    call putc
    jmp puts
.d: ret
put_hex16:
    push ax
    mov al, ah
    call put_hex8
    pop ax
put_hex8:
    push ax
    shr al, 4
    call put_nib
    pop ax
put_nib:
    and al, 0Fh
    add al, '0'
    cmp al, '9'
    jbe .o
    add al, 'a' - '0' - 10
.o: jmp putc
newline:
    mov al, 10
    jmp putc

str_hello: db "ATAPITST", 10, 0
str_bcl:   db "BCL ", 0
str_cmd:   db "CMD ", 0
str_len:   db "LEN ", 0
str_data:  db "DATA ", 0
str_err:   db "ERR ", 0
str_sense: db "SENSE ", 0
str_sense_fail: db "SENSE failed", 10, 0
str_done:  db "DONE", 10, 0
pkt_sense: db 03h, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0
bcl:       dw 0
outlen:    dw 0
outptr:    dw 0
total:     dw 0
bufseg:    dw 0
last_status: db 0
last_error:  db 0

table:
{table}
    db 0
"""


def build_table():
    lines = []
    for name, p, _ in TESTS:
        if name == "delay":
            lines.append("    db 2, %d" % p)
        elif isinstance(p, tuple):
            packet, payload = p
            lines.append("    db 3, " + ", ".join("0%02xh" % b for b in packet) + ", %d, " % len(payload)
                         + ", ".join("0%02xh" % b for b in payload))
        else:
            lines.append("    db 1, " + ", ".join("0%02xh" % b for b in p))
    return ASM.replace("{table}", "\n".join(lines))


# ---------------------------------------------------------------- run

def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def write_fdconfig():
    """FreeDOS boots straight into FDAUTO.BAT on the test floppy."""
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    return cfg


def write_shelf():
    """The flat shelf file the drive answers CDSHELF from, the same format
    the launcher writes beside a machine's monitor socket (disc_library.rs,
    cdshelf/cdshelf_proto.h)."""
    with open(SHELF, "w", newline="\n") as f:   # LF as the launcher writes it, on Windows too
        for label, path in SHELF_DISCS:
            f.write("%s\t%s\n" % (label, path))


READ_ERROR = os.environ.get("ATAPI_READ_ERROR") == "1"
READ_ERROR_SECTORS = (2152, 2156)   # inside the 2150..2300 and 2150..2160 plays


def read_error_env():
    """QEMU's environment with the host file failing READ_ERROR_SECTORS."""
    so = os.path.join(OUT, "read-error-inject.so")
    sh("cc", "-O2", "-shared", "-fPIC", "-o", so, os.path.join(ROOT, "tools/read-error-inject.c"), "-ldl")
    env = dict(os.environ)
    env.update({"LD_PRELOAD": so, "READ_ERROR_MATCH": "lec.bin",
                "READ_ERROR_FROM": str(READ_ERROR_SECTORS[0] * 2352),
                "READ_ERROR_TO": str(READ_ERROR_SECTORS[1] * 2352)})
    return env


def run_qemu(img, log, done=b"DONE", qemu_log="qemu.log", env=None):
    if os.path.exists(log):
        os.unlink(log)
    # QEMU_TCG_OPTS=<prop>=off runs the battery against an accelerator switch,
    # the way the DOS batteries do (tools/x87-guest-test.py's tcg_opts)
    tcg = [o for o in os.environ.get("QEMU_TCG_OPTS", "").split(",") if o]
    p = subprocess.Popen([
        QEMU, "-machine", "pc", "-cpu", "pentium3", "-m", "64",
        *(["-accel", ",".join(["tcg"] + tcg)] if tcg else []),
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        "-fda", img, "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
        "-drive", "if=none,id=cd0,media=cdrom,file=" + BOOT_DISC,
        # id= so a shelf LOAD has something to address the medium change to,
        # shelf= so the vendor opcode answers at all (patch 52)
        "-device", "ide-cd,bus=ide.1,id=%s,drive=cd0,audiodev=w0,shelf=%s" % (CD_ID, SHELF),
        "-audiodev", "none,id=w0",
    ], stderr=open(os.path.join(OUT, qemu_log), "w"), env=env)
    t0 = time.time()
    try:
        while time.time() - t0 < 300:
            time.sleep(1)
            if p.poll() is not None:
                break
            if os.path.exists(log):
                with open(log, "rb") as f:
                    f.seek(0, 2)
                    n = f.tell()
                    f.seek(max(0, n - 64))
                    if done in f.read():
                        break
        else:
            raise SystemExit("timeout waiting for %s" % done.decode())
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()


_dump_cache = {}


def dump(disc, args):
    key = (disc, tuple(args))
    if key not in _dump_cache:
        r = subprocess.run([DISCX, "dump", disc] + list(args), capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit("discx dump %s failed: %s" % (" ".join(args), r.stderr.strip()))
        data = bytearray()
        for line in r.stdout.splitlines():
            parts = line.split()
            data.extend(int(x, 16) for x in parts[1:])
        _dump_cache[key] = bytes(data)
    return _dump_cache[key]


def parse_log(text):
    """-> {bcl: [(packet bytes, ('len', data) | ('err', status, error, sense)), ...]}"""
    runs = {}
    cur = None
    entries = None
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        l = lines[i].strip()
        i += 1
        if l.startswith("BCL "):
            cur = int(l[4:], 16)
            entries = []
            runs[cur] = entries
        elif l.startswith("CMD "):
            packet = bytes(int(x, 16) for x in l[4:].split())
            nxt = lines[i].strip() if i < len(lines) else ""
            if nxt.startswith("LEN "):
                n = int(nxt[4:], 16)
                i += 1
                data = bytearray()
                while i < len(lines) and lines[i].startswith("DATA "):
                    data.extend(bytes.fromhex(lines[i][5:].strip()))
                    i += 1
                entries.append((packet, ("len", bytes(data), n)))
            elif nxt.startswith("ERR "):
                st, er = (int(x, 16) for x in nxt[4:].split())
                i += 1
                sense = b""
                if i < len(lines) and lines[i].startswith("SENSE "):
                    sense = bytes.fromhex(lines[i][6:].strip())
                    i += 1
                entries.append((packet, ("err", st, er, sense)))
            else:
                entries.append((packet, ("none",)))
    return runs


def check_shelf(name, data, count, loaded):
    """A CDSHELF LIST reply against SHELF_DISCS (cdshelf/cdshelf_proto.h)."""
    out = []
    if len(data) != SHELF_HDR + count * SHELF_ENTRY:
        out.append("%s: %d bytes, want a header + %d entries (%d)"
                   % (name, len(data), count, SHELF_HDR + count * SHELF_ENTRY))
        return out
    ver, entry_size, n, total, got_loaded = (
        int.from_bytes(data[i:i + 2], "little") for i in (0, 2, 4, 6, 8))
    if ver != SHELF_VERSION:
        out.append("%s: protocol version %d, want %d" % (name, ver, SHELF_VERSION))
    if entry_size != SHELF_ENTRY:
        out.append("%s: entry size %d, want %d" % (name, entry_size, SHELF_ENTRY))
    if (n, total) != (count, len(SHELF_DISCS)):
        out.append("%s: count %d total %d, want %d / %d" % (name, n, total, count, len(SHELF_DISCS)))
    if loaded is not None and got_loaded != loaded:
        out.append("%s: loaded slot %#x, want %#x" % (name, got_loaded, loaded))
    for i in range(n):
        e = data[SHELF_HDR + i * SHELF_ENTRY:SHELF_HDR + (i + 1) * SHELF_ENTRY]
        slot = int.from_bytes(e[0:2], "little")
        flags, label_len = e[2], e[3]
        want_label = SHELF_DISCS[i][0][:SHELF_LABEL_MAX].encode()
        want_flags = SHELF_FLAG_MISSING if not os.path.exists(SHELF_DISCS[i][1]) else 0
        # every entry that is the loaded disc's file carries LOADED, not
        # just the slot the header names (slots 1 and 2 share a file)
        if loaded is not None and loaded != SHELF_NO_SLOT and not want_flags \
                and os.path.samefile(SHELF_DISCS[i][1], SHELF_DISCS[loaded][1]):
            want_flags |= SHELF_FLAG_LOADED
        if slot != i:
            out.append("%s: entry %d says slot %d" % (name, i, slot))
        if label_len != len(want_label) or e[4:4 + label_len] != want_label:
            out.append("%s: entry %d label %r (%d bytes), want %r"
                       % (name, i, bytes(e[4:4 + label_len]), label_len, want_label))
        if e[4 + label_len:] != b"\0" * (SHELF_LABEL_MAX - label_len):
            out.append("%s: entry %d has trailing junk after the label: %s"
                       % (name, i, bytes(e[4 + label_len:]).hex()))
        if loaded is not None and flags != want_flags:
            out.append("%s: entry %d flags %#x, want %#x" % (name, i, flags, want_flags))
        elif flags & ~SHELF_FLAG_LOADED != want_flags & ~SHELF_FLAG_LOADED:
            out.append("%s: entry %d flags %#x, want %#x (LOADED not checked here)"
                       % (name, i, flags, want_flags))
    return out


def sub_position(data):
    """(audio status, absolute LBA) of a READ SUB-CHANNEL format 1 reply (LBA form)"""
    return data[1], int.from_bytes(data[8:12], "big")


def check(bcl, entries):
    failures = []
    positions = {}
    tests = [t for t in TESTS if t[0] != "delay"]
    if len(entries) != len(tests):
        failures.append("BCL %d: %d replies for %d commands" % (bcl, len(entries), len(tests)))
    for (name, packet, expect), (got_packet, result) in zip(tests, entries):
        if isinstance(packet, tuple):
            packet = packet[0]
        if bytes(packet) != got_packet:
            failures.append("%s: packet mismatch in the log" % name)
            continue
        if expect is None:
            continue
        kind = expect[0]
        if kind == "err":
            _, key, asc, ascq = expect
            if result[0] != "err":
                failures.append("%s: expected CHECK CONDITION %02x/%02x/%02x, got %d bytes" % (name, key, asc, ascq, len(result[1])))
                continue
            sense = result[3]
            got = (sense[2] & 0x0F, sense[12], sense[13]) if len(sense) >= 14 else None
            if got != (key, asc, ascq) or (result[2] >> 4) != key:
                failures.append("%s: sense %s (error reg %02x), want %02x/%02x/%02x" % (name, got, result[2], key, asc, ascq))
            continue
        if result[0] != "len":
            failures.append("%s: CHECK CONDITION, error reg %02x sense %s" % (name, result[1], result[3].hex()))
            continue
        data, n = result[1], result[2]
        if len(data) != n:
            failures.append("%s: LEN %d but %d data bytes" % (name, n, len(data)))
        if kind == "len":
            if n != expect[1]:
                failures.append("%s: %d bytes, want %d" % (name, n, expect[1]))
        elif kind == "bytes":
            if list(data) != expect[1]:
                failures.append("%s: %s, want %s" % (name, data.hex(), bytes(expect[1]).hex()))
        elif kind == "bytes-at":
            _, at, want = expect
            if list(data[at:at + len(want)]) != want:
                failures.append("%s: %s at %d, want %s" % (name, data[at:at + len(want)].hex(), at, bytes(want).hex()))
        elif kind == "dump":
            _, reqs, alloc = expect[:3]
            # the responders are the same on both images; a fourth element
            # names the disc explicitly, for the sectors read after a shelf
            # LOAD changed the medium under us
            src = expect[3] if len(expect) > 3 else None
            want = b"".join(dump(src or (TOC_DISC if r[0] in ("toc", "subq", "discinfo") else DISC), r)
                            for r in reqs)
            if alloc is not None:
                want = want[:alloc]
            if data != want:
                first = next((i for i, (a, b) in enumerate(zip(data, want)) if a != b), min(len(data), len(want)))
                failures.append("%s: %d bytes differ from discx (%d bytes) at offset %d: %s vs %s" % (
                    name, len(data), len(want), first, data[first:first + 8].hex(), want[first:first + 8].hex()))
        elif kind == "conf":
            if n < 16 or data[6:8] != b"\x00\x08" or data[12:14] != b"\x00\x08" or data[14] != 1:
                failures.append("%s: current profile is not CD-ROM: %s" % (name, data[:16].hex()))
            if b"\x00\x1e" not in data or b"\x01\x03" not in data:
                failures.append("%s: features 001E / 0103 missing: %s" % (name, data.hex()))
        elif kind == "page2a":
            if n < 30 or data[2] != 0x03 or data[8] != 0x2A or data[10] != 0x03 or data[12] != 0x71 or data[13] != 0x7F or data[15] != 0x03:
                failures.append("%s: %s" % (name, data[:30].hex()))
        elif kind == "page0e":
            if n < 24 or data[8] != 0x0E or data[16:20] != bytes([1, 255, 2, 255]):
                failures.append("%s: %s" % (name, data[:24].hex()))
        elif kind == "shelf":
            failures.extend(check_shelf(name, data, expect[1], expect[2]))
        elif kind == "pos":
            positions[expect[1]] = sub_position(data)
    # the audio sequence
    def pos(k):
        return positions.get(k, (None, None))
    st, p1 = pos("playing1")
    if st != 0x11 or p1 is None or not (T2 <= p1 < T2 + 150):
        failures.append("playing1: status %s position %s" % (st, p1))
    st, p2 = pos("playing2")
    if st != 0x11 or p2 is None or p1 is None or not (p2 > p1):
        failures.append("playing2: status %s position %s (before %s): must advance" % (st, p2, p1))
    st, q1 = pos("paused1")
    st2, q2 = pos("paused2")
    if st != 0x12 or st2 != 0x12 or q1 != q2 or q1 is None or p2 is None or q1 < p2:
        failures.append("paused: status %s/%s position %s/%s: must hold" % (st, st2, q1, q2))
    st, r = pos("resumed")
    if st != 0x11 or r is None or q2 is None or r < q2:
        failures.append("resumed: status %s position %s" % (st, r))
    st, z = pos("stopped")
    # Where the head is after a stop, which patch 54 changed on purpose: it
    # stays where playback ended, and no longer falls back to the last sector
    # *read* (a data address that has nothing to do with the head; XP asked
    # for track 3 after a stop and was told "track 01, 00:00:17", its own
    # volume descriptor). So: no audio status, and the head inside track 2
    # at or past where it was last seen playing.
    if st != 0x15 or z is None or r is None or not (r <= z < T2 + 300):
        failures.append("stopped: status %s position %s (want 0x15 with the head "
                        "where playback ended, %s..%d)" % (st, z, r, T2 + 300))
    st, c = pos("completed")
    if st != 0x13 or c != T2 + 10:
        failures.append("completed: status %s position %s (want 0x13 at %d)" % (st, c, T2 + 10))
    st, t3 = pos("track3")
    if st != 0x11 or t3 is None or not (5300 <= t3 < 6800):
        failures.append("track3: status %s position %s" % (st, t3))
    st, t3b = pos("track3again")
    if st != 0x11 or t3b is None or not (5300 <= t3b < 6800):
        failures.append("track3again: status %s position %s" % (st, t3b))
    st, z2 = pos("sspstopped")
    # The same for the stop a Windows guest actually sends, after track 3
    if st != 0x15 or z2 is None or not (5300 <= z2 < 6800):
        failures.append("stopped by start stop unit: status %s position %s "
                        "(want 0x15 with the head in track 3, 5300..6800)" % (st, z2))
    return failures


# ---------------------------------------------------------------- CDSHELF.COM

# What FDAUTO.BAT runs, each line's output redirected to COM1 (DOS function
# 02h goes through STDOUT, so the program needs nothing special for this).
# `list` rather than no argument: with no argument the program is a menu
# that waits for a key, which is the right thing for a person and no use to
# a batch file.
CDSHELF_RUNS = ["list", "1", "list", "E", "list", "0", "list"]


def check_cdshelf(text):
    """The DOS program's own output: guest-tools/src/cdshelf.asm."""
    failures = []
    banner = "CDSHELF - the host's disc shelf"
    runs = [s for s in text.split(banner)[1:]]
    if len(runs) != len(CDSHELF_RUNS):
        return ["CDSHELF.COM: %d runs in the log, want %d" % (len(runs), len(CDSHELF_RUNS))]

    def want(i, *needles):
        for n in needles:
            if n not in runs[i]:
                failures.append("CDSHELF.COM run %d (%r): %r missing from the output"
                                % (i + 1, CDSHELF_RUNS[i] or "no argument", n))

    def unwanted(i, *needles):
        for n in needles:
            if n in runs[i]:
                failures.append("CDSHELF.COM run %d (%r): %r in the output"
                                % (i + 1, CDSHELF_RUNS[i] or "no argument", n))

    def loaded_line(i, label):
        for line in runs[i].splitlines():
            if label in line:
                if "[in the drive]" not in line:
                    failures.append("CDSHELF.COM run %d: %r is not marked as loaded: %r"
                                    % (i + 1, label, line.strip()))
                return
        failures.append("CDSHELF.COM run %d: no line for %r" % (i + 1, label))

    # the plain listing: our drive, every label, the truncation and the flags;
    # the boot disc (slot 0, never LOADed) is the one in the drive
    want(0, "secondary master (170h)", "%d discs." % len(SHELF_DISCS),
         "[missing on the host]", LONG_LABEL[:SHELF_LABEL_MAX])
    unwanted(0, "Mixed disc  [in the drive]", LONG_LABEL[:SHELF_LABEL_MAX + 1])
    for i, (label, _) in enumerate(SHELF_DISCS):
        want(0, "%3d  %s" % (i, label[:SHELF_LABEL_MAX]))
    loaded_line(0, "Selftest disc")
    # a load, seen by the program itself and then in the next listing; the
    # long-labelled entry is the same file, so it is in the drive too
    want(1, "loading slot 1: Mixed disc", "the disc is in the drive.")
    loaded_line(2, "Mixed disc")
    loaded_line(2, LONG_LABEL[:SHELF_LABEL_MAX])
    unwanted(2, "Selftest disc  [in the drive]")
    want(3, "the drive is empty.")
    unwanted(4, "[in the drive]")
    want(5, "loading slot 0: Selftest disc", "the disc is in the drive.")
    loaded_line(6, "Selftest disc")
    return failures


def run_cdshelf_stage():
    com = os.path.join(OUT, "CDSHELF.COM")
    sh("nasm", "-f", "bin", "-o", com, os.path.join(ROOT, "guest-tools/src/cdshelf.asm"))
    img = os.path.join(OUT, "cdshelf.img")
    shutil.copy(FLOPPY, img)
    cfg = write_fdconfig()
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\n")
        for arg in CDSHELF_RUNS:
            f.write(("CDSHELF.COM %s > COM1\r\n" % arg).replace("  ", " "))
        f.write("ECHO CDSHELFDONE>COM1\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::CDSHELF.COM")
    log = os.path.join(OUT, "cdshelf-serial.log")
    run_qemu(img, log, done=b"CDSHELFDONE", qemu_log="qemu-cdshelf.log")
    return check_cdshelf(open(log, "r", errors="replace").read()), log


def main():
    ensure_prereqs()
    os.makedirs(OUT, exist_ok=True)
    write_shelf()
    asm = os.path.join(OUT, "atapitst.asm")
    com = os.path.join(OUT, "ATAPITST.COM")
    with open(asm, "w") as f:
        f.write(build_table())
    sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    img = os.path.join(OUT, "atapitst.img")
    shutil.copy(FLOPPY, img)
    cfg = write_fdconfig()
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nATAPITST.COM\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::ATAPITST.COM")
    log = os.path.join(OUT, "serial.log")
    t0 = time.time()
    run_qemu(img, log, env=read_error_env() if READ_ERROR else None)
    print("guest run: %.1f s" % (time.time() - t0))
    text = open(log, "r", errors="replace").read()
    runs = parse_log(text)
    if sorted(runs) != [512, 65534]:
        print("atapi guest test: FAIL, runs found: %s (%s)" % (sorted(runs), log))
        return 1
    failures = []
    for bcl in (512, 65534):
        for f in check(bcl, runs[bcl]):
            failures.append("BCL %d: %s" % (bcl, f))
    n = sum(len(v) for v in runs.values())
    if failures:
        print("atapi guest test: FAIL, %d of %d replies wrong (%s)" % (len(failures), n, log))
        for f in failures[:40]:
            print("  " + f)
        return 1
    print("atapi guest test: %d replies over two byte-count limits identical to discx; audio positions consistent" % n)
    if READ_ERROR:
        qlog = open(os.path.join(OUT, "qemu.log"), errors="replace").read()
        silent = ["ide-cd: CD audio sector %d unreadable: played as silence" % lba
                  for lba in range(*READ_ERROR_SECTORS)]
        missing = [m for m in silent if m not in qlog]
        if missing or "read-error-inject: failing" not in qlog:
            print("atapi read-error test: FAIL -- the injected errors were not played as silence "
                  "(%s): %s" % (os.path.join(OUT, "qemu.log"), missing or "no read was failed"))
            return 1
        print("atapi read-error test: sectors %d..%d unreadable on the host, played as silence; "
              "the plays over them advanced and completed" % (READ_ERROR_SECTORS[0], READ_ERROR_SECTORS[1] - 1))
        return 0

    # the real DOS program on the same shelf (guest-tools/src/cdshelf.asm)
    t0 = time.time()
    shelf_failures, shelf_log = run_cdshelf_stage()
    print("CDSHELF.COM run: %.1f s" % (time.time() - t0))
    if shelf_failures:
        print("cdshelf guest test: FAIL (%s)" % shelf_log)
        for f in shelf_failures[:20]:
            print("  " + f)
        return 1
    print("cdshelf guest test: CDSHELF.COM listed %d discs, loaded and ejected through the drive"
          % len(SHELF_DISCS))
    return 0


if __name__ == "__main__":
    sys.exit(main())
