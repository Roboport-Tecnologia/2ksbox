#!/usr/bin/env python3
"""Duke Nukem 3D's music through the machine's own devices (doc 20, M12).

The `midi-guest` battery proves a DOS program can drive an OPL3 and an
MPU-401 that a test wrote by hand. This proves the thing that matters:
that a **real game of the era**, through its own sound system, finds them
and plays its score on them. Duke Nukem 3D is a good one to ask. The
Apogee Sound System it uses supports all three of this milestone's paths
(the Sound Blaster's FM, General MIDI on an MPU-401, and the Gravis's own
wavetable), and its title screen starts the music with nothing to click.

The disc is the user's own and is only ever **read**: the Atomic Edition
CD carries the whole DOS build uncompressed in `ATOMINST\\`, which is
copied out to a FAT hard disk this tool makes. Nothing is written to the
image, and the game never needs the disc at run time.

    tools/duke-guest-test.py                    # General MIDI, the default
    tools/duke-guest-test.py --disc <cue|iso> --seconds 60
    tools/duke-guest-test.py --run "DIR D:\\"      # a DOS command instead of the game
    tools/duke-guest-test.py --setup --keys …   # drive SETUP.EXE by hand

**What is proven, and what is not**:

* `--music gm` runs from nothing. It extracts the game, builds the
  disk, drives SETUP.EXE once for a config, and the game plays: the
  device reports ~1000 bytes and 300-450 note-ons per 5 s across 5 to 8
  MIDI channels, and 70 s of the recording is audible.
* The OPL3 plays this game too, measured the same way (2466 register
  writes and 516 key-ons in 5 s), but **only when SETUP.EXE launches the
  game itself** after picking its *AdLib* entry. Started from a batch
  file with the same config patched in, the game answers "Couldn't find
  selected sound card", with `MusicDevice = 2`, with `MidiPort` at
  0x388 as its own SETUP writes it, and with `BLASTER` exported. That is
  the game's configuration and not the device (the `midi-guest` battery
  and the `music` check both drive the same OPL3 from a standing start),
  and it is why `--music fm` is not a one-command mode here. Its *Sound
  Blaster* music entry, as opposed to *AdLib*, refuses on every path
  tried, even with the FM chip mirrored at the card's own 2x8 where a
  real SB Pro or 16 decodes it.

Outputs in build/duke-guest/. Local only (it needs the disc), never in
scripts/test.sh.
"""
import argparse
import os
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(ROOT, "build/qemu/qemu-system-i386")
SYNTHX = os.path.join(ROOT, "target/release/synthx")
SOUNDFONT = os.path.join(ROOT, "soundfonts/TimGM6mb.sf2")
OUT = os.path.join(ROOT, "build/duke-guest")
DISC = os.path.expanduser("~/vms/DUKE_ATOMIC.CUE")
GAME_DIR = "ATOMINST"           # the DOS build, uncompressed, on the disc
DISK_MB = 96                    # the game is ~46 MB

# The music device numbers in the game's own DUKE3D.CFG, learned by
# driving its SETUP.EXE and reading the file back:
#
#   MusicDevice = 4  General Midi, on `MidiPort`: our mpu401 at 0x330.
#   MusicDevice = 2  AdLib: the OPL3 at 0x388.
#   MusicDevice = 1  "Sound Blaster" music, which this game refuses to
#                    initialize here ("Couldn't find selected sound
#                    card") even with the FM chip mirrored at the card's
#                    own base, where a real SB Pro or 16 decodes it. Its
#                    AdLib entry is the FM path that works, and the one a
#                    period user would have picked anyway. Unexplained,
#                    and possibly QEMU's sb16 rather than our OPL3.
# `MidiPort` goes with the device and is not only the MPU's: the AdLib
# entry probes the *FM* chip at whatever this says, so a config that
# names 0x330 while asking for AdLib finds nothing and the game says
# "Couldn't find selected sound card", which is what patching only the
# device number produces.
MUSIC_DEVICE = {"gm": (4, "0x330"), "fm": (2, "0x388")}

# **The config is the game's own.** A hand-written DUKE3D.CFG with the
# right `MusicDevice` in it is not enough (the game reads its own
# sections and falls back to no sound card at all), and 3D Realms' file
# is not something to check into this repository. So SETUP.EXE writes
# one, once per staged disk, driven blind through the menus below, and
# every later run patches one line of it. These are the steps: Sound
# Setup, Choose Sound FX Card, down twice to Sound Blaster (whose
# defaults are already our sb16's 0x220 / 5 / 1 / 5), its follow-up
# dialogs, Choose Music Card, eight down to General Midi, the MIDI port
# (0x330 is already highlighted), out with two escapes, and nine down to
# "Save and launch Duke Nukem 3D".
SETUP_KEYS = ("8:ret,11:ret,13:down,14:down,15:ret,17:ret,20:down,21:ret,24:ret,"
              "27:ret,30:ret,33:down,35:ret,"
              + ",".join(["37:down"] * 8) + ",44:ret,48:esc,53:esc,"
              + ",".join(["56:down"] * 9) + ",64:ret")

CDROM_INI = "D:\\SUPPORT\\\n"


# --------------------------------------------------------------- the disc

class Iso:
    """Enough ISO 9660 to copy one directory out of a CD image, whatever
    the sectors look like: a raw MODE1/2352 or MODE2/2352 track (a
    cue/bin dump, which is what a mixed-mode disc like this one is) or a
    plain 2048-byte .iso. The user data's offset inside the sector is
    found by looking for the volume descriptor rather than by parsing the
    cue, so a .bin named in one and a .iso both work.

    Read-only, and host-side on purpose: what a *guest* sees of a disc is
    the M5 tools' business (`tools/atapi-guest-test.py`), not this one's.
    """

    def __init__(self, path):
        if path.lower().endswith(".cue"):
            path = self._bin_of(path)
        self.f = open(path, "rb")
        self.path = path
        for size, offset in ((2352, 24), (2352, 16), (2048, 0)):
            self.size, self.offset = size, offset
            if self.sector(16)[1:6] == b"CD001":
                break
        else:
            raise SystemExit("%s: no ISO 9660 volume descriptor at sector 16" % path)

    @staticmethod
    def _bin_of(cue):
        for line in open(cue, "r", errors="replace"):
            line = line.strip()
            if line.upper().startswith("FILE "):
                name = line.split('"')[1] if '"' in line else line.split()[1]
                return os.path.join(os.path.dirname(cue), name)
        raise SystemExit("%s: no FILE line" % cue)

    def sector(self, lba):
        self.f.seek(lba * self.size + self.offset)
        return self.f.read(2048)

    def read(self, lba, length):
        out = bytearray()
        for i in range((length + 2047) // 2048):
            out += self.sector(lba + i)
        return bytes(out[:length])

    def listdir(self, lba, length):
        """[(name, size, lba, is_dir)]: one directory's records."""
        data = self.read(lba, length)
        out, i = [], 0
        while i < len(data):
            rec_len = data[i]
            if rec_len == 0:                      # padding to the sector's end
                i = (i // 2048 + 1) * 2048
                if i >= len(data):
                    break
                continue
            rec = data[i:i + rec_len]
            e_lba, size, flags = (struct.unpack("<I", rec[2:6])[0],
                                  struct.unpack("<I", rec[10:14])[0], rec[25])
            name = rec[33:33 + rec[32]].decode("latin-1").split(";")[0]
            if name not in ("\x00", "\x01"):
                out.append((name, size, e_lba, bool(flags & 2)))
            i += rec_len
        return out

    def root(self):
        pvd = self.sector(16)
        rec = pvd[156:190]
        return struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0]

    def find(self, name):
        for entry in self.listdir(*self.root()):
            if entry[0].upper() == name.upper():
                return entry
        raise SystemExit("%s: no %s directory on this disc — is it the Atomic Edition?"
                         % (self.path, name))


def extract(disc, dest):
    """The game's directory out of the disc and into `dest`."""
    iso = Iso(disc)
    _, size, lba, isdir = iso.find(GAME_DIR)
    if not isdir:
        raise SystemExit("%s is not a directory on %s" % (GAME_DIR, disc))
    os.makedirs(dest, exist_ok=True)
    total = 0
    for name, fsize, flba, fdir in iso.listdir(lba, size):
        if fdir:
            continue
        with open(os.path.join(dest, name), "wb") as f:
            f.write(iso.read(flba, fsize))
        total += fsize
    print("staged %d files, %.1f MB from %s" % (len(os.listdir(dest)), total / 1e6, disc))
    return dest


# ---------------------------------------------------------------- the disk

def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def make_disk(path, mb, files, extra):
    """A partitioned FAT hard disk with the game on it. mtools formats
    inside the partition but writes no table for a disk it did not make,
    so the table is written here, `tools/xp-cdimage-test.sh`'s recipe,
    with FAT16 and a DOS partition type because the guest is FreeDOS."""
    if os.path.exists(path):
        os.unlink(path)
    with open(path, "wb") as f:
        f.truncate(mb * 1024 * 1024)
    start, total = 2048, mb * 1024 * 1024 // 512
    mbr = bytearray(512)
    mbr[0x1be:0x1be + 16] = struct.pack("<B3sB3sII", 0x80, b"\xfe\xff\xff", 0x06,
                                        b"\xfe\xff\xff", start, total - start)
    mbr[510:512] = b"\x55\xaa"
    with open(path, "r+b") as f:
        f.write(bytes(mbr))
    img = "%s@@%d" % (path, start * 512)
    # -H 2048: the BPB's hidden-sectors field is the partition's start LBA.
    sh("mformat", "-i", img, "-h", "16", "-s", "63", "-H", str(start),
       "-T", str(total - start), "::")
    sh("mmd", "-i", img, "::/DUKE")
    for f in sorted(os.listdir(files)):
        sh("mcopy", "-o", "-i", img, os.path.join(files, f), "::/DUKE/" + f)
    for name, text in extra.items():
        tmp = os.path.join(OUT, name)
        with open(tmp, "w", newline="\r\n") as f:
            f.write(text)
        sh("mcopy", "-o", "-i", img, tmp, "::/DUKE/" + name)
    return path


def write_file(path, name, text):
    """One small file onto the staged disk. The config and the CD path
    are written on *every* run, `--keep-disk` or not: they are two
    hundred bytes, and a disk staged for one music device with the
    config of another is a machine that refuses to start."""
    tmp = os.path.join(OUT, name)
    with open(tmp, "w", newline="\r\n") as f:
        f.write(text)
    sh("mcopy", "-o", "-i", "%s@@%d" % (path, 2048 * 512), tmp, "::/DUKE/" + name)


def read_back(path, name, dest):
    """One file off the staged disk, for looking at what the game wrote."""
    img = "%s@@%d" % (path, 2048 * 512)
    r = subprocess.run(["mcopy", "-n", "-o", "-i", img, "::/DUKE/" + name, dest])
    return dest if r.returncode == 0 and os.path.exists(dest) else None


# ------------------------------------------------------------- the guest

# What the FreeDOS *boot* floppy does not carry and this needs, from the
# same distribution's package repository, fetched once beside the floppy
# `tools/x87-guest-test.py` fetches. Nothing here redistributes them.
#
#   HIMEMX   DUKE3D.EXE is a DOS/4GW program, and DOS/4GW needs XMS.
#   UDVD2    a CD-ROM driver, with SHSUCDX as the MSCDEX half. The game
#            checks for its own CD on the way out, and a machine with no
#            CD *letter* fails that check however good its drive is.
REPO = "https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/repositories/1.3/"
DOS_TOOLS = {
    "HIMEMX.EXE": ("base/himemx.zip", "BIN/HIMEMX.EXE"),
    "UDVD2.SYS": ("drivers/udvd2.zip", "BIN/UDVD2.SYS"),
    "SHSUCDX.COM": ("base/shsucdx.zip", "BIN/SHSUCDX.COM"),
}


def ensure_dos_tool(name):
    path = os.path.join(ROOT, "build/images", name)
    if os.path.exists(path):
        return path
    import io
    import urllib.request
    import zipfile
    pkg, member = DOS_TOOLS[name]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    print("downloading", REPO + pkg)
    with urllib.request.urlopen(REPO + pkg) as r:
        data = r.read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        with open(path, "wb") as f:
            f.write(z.read(member))
    return path


def boot_floppy(run_line):
    """A copy of the FreeDOS floppy that loads XMS and runs one command."""
    img = os.path.join(OUT, "boot.img")
    import shutil
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w", newline="\r\n") as f:
        f.write("!LASTDRIVE=Z\n!BUFFERS=20\n!FILES=40\n"
                "DEVICE=\\HIMEMX.EXE\n"
                "DEVICE=\\UDVD2.SYS /D:CDROM1\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w", newline="\r\n") as f:
        # SHSUCDX is the MSCDEX half: without it the driver is loaded and
        # there is still no D:, which is exactly what the game's CD check
        # complains about.
        # BLASTER is how every DOS program of the era is told where the
        # card is (doc 20 §6), and the game's FM music needs it: its
        # AdLib entry initialized only when SETUP.EXE launched the game
        # itself (SETUP puts BLASTER in the environment) and not from a
        # plain batch file, until this line existed. `P330` is the MIDI
        # port, which is why it names our mpu401 too.
        f.write("@echo off\nSET BLASTER=A220 I5 D1 H5 P330 T6\n"
                "SHSUCDX.COM /D:CDROM1\nC:\nCD \\DUKE\n%s\n" % run_line)
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    for name in ("HIMEMX.EXE", "UDVD2.SYS", "SHSUCDX.COM"):
        sh("mcopy", "-o", "-i", img, ensure_dos_tool(name), "::" + name)
    return img


class Qmp:
    """Just enough QMP to take a screendump and quit."""

    def __init__(self, path):
        import socket
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(100):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise SystemExit("no QMP socket at %s" % path)
        self.f = self.sock.makefile("rwb")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, execute, **args):
        import json
        msg = {"execute": execute}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return None
            reply = json.loads(line)
            if "return" in reply or "error" in reply:
                return reply

    def key(self, name):
        """One keystroke, by QKeyCode name (`ret`, `esc`, `down`, `y`…).
        SETUP.EXE is a menu program and this is the only way to answer
        it headlessly."""
        self.cmd("send-key", keys=[{"type": "qcode", "data": name}])

    def shot(self, path):
        self.cmd("screendump", filename=path)
        return path if os.path.exists(path) else None


def ppm_to_png(ppm, png):
    """The screendumps QMP writes are PPM; this makes them viewable
    without ImageMagick, which the icons check already skips for."""
    import zlib
    with open(ppm, "rb") as f:
        data = f.read()
    parts, i = [], 0
    while len(parts) < 4:                 # P6, width, height, maxval
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while not data[j:j + 1].isspace():
            j += 1
        parts.append(data[i:j])
        i = j
    i += 1
    w, h = int(parts[1]), int(parts[2])
    rows = b"".join(b"\x00" + data[i + y * w * 3: i + (y + 1) * w * 3] for y in range(h))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF))

    with open(png, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(rows, 6)))
        f.write(chunk(b"IEND", b""))
    return png


def music_device(kind):
    """The machine's music hardware, as `bundle::Sound`/`bundle::Music`
    would give it: a DOS machine is an SB16 with its OPL3, and the MIDI
    port is what the picker's General MIDI entry adds."""
    args = ["-device", "sb16,audiodev=w", "-device", "opl3,audiodev=w,sbbase=0x220"]
    if kind == "gm":
        args += ["-device", "mpu401,audiodev=w,synth=gm,soundfont=" + SOUNDFONT]
    elif kind == "gus":
        args = ["-device", "gus,audiodev=w"]
    return args


def parse_keys(spec):
    """`--keys 3:ret,6:down,7:ret`: a keystroke at a second, in the
    shape `tools/xp-driver-test.sh`'s SHOT_KEYS uses. A screendump is
    taken right after each one, which is how a menu program is driven
    without watching it."""
    out = []
    for item in filter(None, spec.split(",")):
        when, _, key = item.partition(":")
        out.append((float(when), key))
    return sorted(out)


def run(disk, floppy, disc, wav, seconds, music, shots, keys=(), pause=0):
    sock = os.path.join(OUT, "qmp.sock")
    if os.path.exists(sock):
        os.unlink(sock)
    for f in (wav,):
        if os.path.exists(f):
            os.unlink(f)
    log = open(os.path.join(OUT, "qemu.log"), "wb")
    p = subprocess.Popen([
        QEMU, "-machine", "pc", "-cpu", "pentium3", "-m", "64",
        # doc 06's DOS machine: a real VGA/VESA BIOS of the period. Never
        # `d3dpt-vga`, which is driven by a *Windows* display driver of
        # ours.
        "-vga", "cirrus",
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        "-drive", "file=%s,if=floppy,index=0,format=raw" % floppy,
        "-drive", "file=%s,if=ide,index=0,media=disk,format=raw" % disk,
        # The user's own disc, **read-only** and never written: the game
        # checks for it on the way out. A `.cue` goes through our own
        # cdimage driver (doc 17), which is what a mixed-mode Mode 2 disc
        # like this one needs.
        "-drive", "file=%s,if=ide,index=2,media=cdrom,readonly=on" % disc,
        "-boot", "a", "-monitor", "none",
        "-audiodev", "wav,id=w,path=" + wav,
        *music_device(music),
        "-qmp", "unix:%s,server,nowait" % sock,
    ], stdout=log, stderr=subprocess.STDOUT)
    try:
        qmp = Qmp(sock)
        t0 = time.time()
        taken, pressed, paused_at = 0, 0, 0.0
        pending = list(keys)
        def shot(tag):
            ppm = os.path.join(OUT, "shot-%s.ppm" % tag)
            if qmp.shot(ppm):
                ppm_to_png(ppm, ppm[:-4] + ".png")
                os.unlink(ppm)
        while time.time() - t0 < seconds:
            time.sleep(0.5)
            if p.poll() is not None:
                break
            now = time.time() - t0
            if pause and now - paused_at >= pause:
                # the player's Pause: a stop, a second, and a cont
                paused_at = now
                qmp.cmd("stop")
                time.sleep(1)
                qmp.cmd("cont")
            while pending and pending[0][0] <= now:
                when, key = pending.pop(0)
                pressed += 1
                qmp.key(key)
                time.sleep(0.5)
                shot("key%02d-%s" % (pressed, key))
            if shots and now >= (taken + 1) * (seconds / shots):
                taken += 1
                shot("%02d" % taken)
        if p.poll() is not None:
            print("QEMU died: exit status %d after %.0f s" % (p.returncode, time.time() - t0))
        else:
            qmp.cmd("quit")
        for _ in range(50):
            if p.poll() is not None:
                break
            time.sleep(0.2)
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
        log.close()
    return open(os.path.join(OUT, "qemu.log"), "rb").read().decode("latin-1")


import importlib.util   # noqa: E402  (after the module-level constants above)

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)


def ensure_cfg(disk, disc, music):
    """The game's own config, with this run's music device in it. Written
    by SETUP.EXE the first time (75 s in the guest, once per staged
    disk), patched by one line every time after."""
    path = os.path.join(OUT, "DUKE3D.CFG")
    if read_back(disk, "DUKE3D.CFG", path) is None:
        print("no DUKE3D.CFG yet: driving the game's own SETUP.EXE")
        run(disk, boot_floppy("SETUP.EXE"), disc, os.path.join(OUT, "setup.wav"),
            80, "gm", 0, parse_keys(SETUP_KEYS))
        if read_back(disk, "DUKE3D.CFG", path) is None:
            raise SystemExit("SETUP.EXE wrote no DUKE3D.CFG — see the screendumps in " + OUT)
    device, port = MUSIC_DEVICE[music]
    out = []
    for line in open(path, "r", errors="replace").read().splitlines():
        if line.startswith("MusicDevice"):
            line = "MusicDevice = %d" % device
        elif line.startswith("MidiPort"):
            line = "MidiPort = %s" % port
        out.append(line)
    write_file(disk, "DUKE3D.CFG", "\n".join(out) + "\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disc", default=DISC)
    ap.add_argument("--music", default="gm", choices=("gm", "fm", "gus"))
    ap.add_argument("--seconds", type=int, default=45)
    ap.add_argument("--shots", type=int, default=4)
    ap.add_argument("--setup", action="store_true", help="run the game's own SETUP.EXE")
    ap.add_argument("--run", default=None, help="a DOS command line to run instead of the game")
    ap.add_argument("--cfg", default=None, help="a DUKE3D.CFG to stage with the game")
    ap.add_argument("--keep-disk", action="store_true", help="reuse the staged disk")
    ap.add_argument("--pause", type=float, default=0,
                    help="every this many seconds, pause the guest for one (QMP stop/cont)")
    ap.add_argument("--keys", default="", help="<seconds>:<key>,… sent over QMP, each with a screendump")
    args = ap.parse_args()

    os.makedirs(OUT, exist_ok=True)
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    disk = os.path.join(OUT, "duke.img")
    if not (args.keep_disk and os.path.exists(disk)):
        game = extract(args.disc, os.path.join(OUT, "game"))
        make_disk(disk, DISK_MB, game, {})
    write_file(disk, "CDROM.INI", CDROM_INI)
    if args.cfg:
        write_file(disk, "DUKE3D.CFG", open(args.cfg).read())
    elif not args.setup:
        ensure_cfg(disk, args.disc, args.music)
    floppy = boot_floppy(args.run or ("SETUP.EXE" if args.setup else "DUKE3D.EXE"))
    wav = os.path.join(OUT, "duke-%s.wav" % args.music)
    text = run(disk, floppy, args.disc, wav, args.seconds, args.music, args.shots,
               parse_keys(args.keys), args.pause)

    # What the device saw. This is the half a recording cannot give:
    # whether the *game* drove the port, and how hard.
    device = "mpu401" if args.music == "gm" else "opl3"
    seen = [l.strip().split("info: ")[-1] for l in text.splitlines() if device + ":" in l]
    for line in seen[:6]:
        print("   ", line)
    if any(w in text for w in ("Couldn't find", "put Duke Nukem")):
        print("    the game refused to start — see the screendumps in", OUT)
    ok = bool(seen)
    if not ok:
        print("FAIL %-8s the game never wrote to the %s" % (args.music, device))
        if args.music == "fm":
            print("    (the FM path needs SETUP.EXE to launch the game — see the"
                  " module docstring; `--music gm` is the mode that runs from nothing)")
    if os.path.exists(SYNTHX):
        ok = subprocess.run([SYNTHX, "wavlevel", wav]).returncode == 0 and ok
    cfg = read_back(disk, "DUKE3D.CFG", os.path.join(OUT, "DUKE3D.CFG"))
    if cfg and args.setup:
        print("the config SETUP wrote is in", cfg)
    print("duke-guest-test: %s %s" % (args.music, "passed" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main() or 0)
