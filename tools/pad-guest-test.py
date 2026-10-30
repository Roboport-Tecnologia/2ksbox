#!/usr/bin/env python3
"""A gamepad as a *guest* meets it (M13, docs/tracks/m13-gamepads.md):
both devices, each in the family it is for.

    tools/pad-guest-test.py                 # path B: the gameport, FreeDOS
    tools/pad-guest-test.py xp [image]      # path A: the USB HID pad, XP
    tools/pad-guest-test.py win98 [machine|image]     # path A on Windows 98
    tools/pad-guest-test.py win98 claude98            # ...a launcher machine
    tools/pad-guest-test.py --verbose ...   # ...and every sample line
    UNTHROTTLED=1 tools/pad-guest-test.py   # the DOS control: no -icount

Both paths were confirmed by hand with a real controller (a DualSense).
This re-checks them, because seen to work once is not the same as kept
working, and the two guest ends are exactly the parts no host-side check
can reach.

**It must be the player, not `qemu-system-i386`.** The pad reaches a guest
through the embed library (`qemu_embed_pad_state` -> the input bottom half
-> `usb_gamepad_set_state` / `gameport_set_state`), and only the player
drives that; a bare QEMU has pad devices that nothing ever moves.
`PLAYER_PAD_SCRIPT` stands in for the controller, so this runs on a
machine with nothing plugged in, which is the whole reason that source
exists.

The poses are the same on both paths, and each moves **one** control, so
nothing here has to align the guest's clock with the host's frame counter
and the order the samples arrive in does not matter. What the two guests
are then asked is deliberately *opposite* in one place, which is the point
of running both:

    control          gameport (path B)        USB HID (path A)
    left stick X     axis 1                   X
    the d-pad        axis 1 and 2 (folded)    the POV hat, and nothing else
    right stick X    axis 3                   Z
    face buttons     the four button bits     buttons 1-4

The d-pad row is the one to watch. A gameport has only pots, so a digital
pad has to drive the axes to their ends or a DOS game cannot read it at
all; a HID pad has a hat, and driving the axes from it as well would make
a stick the guest cannot centre. The same host state has to produce both.

On the Windows paths every sample is read through **two** APIs, because a
title of the era can call either: DirectInput, and winmm's `joyGetPosEx`
on top of 9x's VJOYD. That second column closed M13's last item. A
Windows game on 98 would otherwise need the gameport's driver half
("Standard Game Port" through Add New Hardware) to see a joystick at all.
It does not, because the USB pad arrives through winmm as well, which
`check_winmm` asserts with the same force as the DirectInput half.

Outputs in build/pad-guest/. Local only: the DOS run fetches the FreeDOS
floppy `tools/x87-guest-test.py` uses, the Windows runs want one of the
user's own images, and all of them want a display for the player's window.
All three are in `scripts/test.sh`'s guest stage as `pad-guest`,
`pad-guest-xp` and `pad-guest-98`; the last skips rather than fails where
the launcher machine it names does not exist, since no suite can assume
someone's library looks like this one.
"""
import glob
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAYER = os.path.join(ROOT, "player-mitsuami/target/release/player-mitsuami")
QEMU_IMG = os.path.join(ROOT, "build/qemu/qemu-img")
QMPC = os.path.join(ROOT, "tools/qmpc.py")
OUT = os.path.join(ROOT, "build/pad-guest")

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

ASM = os.path.join(ROOT, "guest-tools/src/padtest.asm")

# The pad, in frames of the player's own publish loop, which is what the
# scripted source counts, so these are *not* seconds. The player publishes
# on QEMU's refresh tick (`on_refresh_done`, ~60 a second) whether or not
# the guest drew anything, so the script advances at the same rate on a
# DOS box printing continuously and on an idle Windows desktop.
#
# One control per pose, and one axis per control (see the module
# docstring), so every check below is a statement about values and none of
# them about time.
PHASE = 150             # frames a pose is held: ~2.5 s, about ten samples

POSES = [
    "lx=-1.0",              # the stick, to one end
    "lx=1.0",               # ...and the other
    "lx=0.0",               # centred
    "dpad_up=1",            # the hat: north
    "dpad_up=0,dpad_down=1",  # ...and south
    "dpad_down=0,rx=1.0",   # the second stick
    "rx=0.0,south=1",       # button 1
    "south=0,east=1",       # button 2
    "east=0",
]


def script(offset, cycles):
    """`PLAYER_PAD_SCRIPT` for `cycles` passes, starting at `offset`.

    Two shapes, and both were learned by running this. **It starts late**
    on DOS: FreeDOS is at the prompt in about four seconds but the script
    starts at frame 1, so the first poses played to a machine that was
    still booting and one end of the stick was never sampled. **It
    repeats**, which is what makes a Windows guest possible at all. That
    one takes a minute to boot and log in, and no offset would be a
    reliable guess, so the poses keep coming round until the guest has
    seen them all and the run stops itself.
    """
    steps = []
    frame = offset
    for _ in range(cycles):
        for pose in POSES:
            for item in pose.split(","):
                steps.append("%d:%s" % (frame, item))
            frame += PHASE
    return ",".join(steps)


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def watch(p, log, done, satisfied, timeout=600):
    """Wait for the guest, and stop as soon as it has shown everything.

    `satisfied` is the run's own checks, asked quietly of the log so far:
    a pass ends the run instead of sitting through the rest of the guest's
    sampling. `done` is the guest's own last word, which is the other end:
    a run that never satisfies the checks plays out in full and then
    fails with the numbers in front of it.
    """
    t0 = time.time()
    while time.time() - t0 < timeout:
        time.sleep(1)
        if p.poll() is not None:
            break
        if not os.path.exists(log):
            continue
        text = open(log, "rb").read().decode("latin-1")
        if satisfied(text) or done in text:
            break
    else:
        raise SystemExit("timeout waiting for %s (see %s)" % (done, log))
    return open(log, "rb").read().decode("latin-1")


def samples(text, keys):
    """The `K=v ...` lines that carry every key in `keys`, as dicts.

    A line that does not parse is skipped in silence: the log is read while
    the guest is still writing it and the last line is routinely half a
    sample.
    """
    out = []
    first = keys[0] + "="
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith(first):
            continue
        try:
            f = dict(p.split("=", 1) for p in line.split(" ") if "=" in p)
            # Button masks are hex in both columns, `B` from DirectInput
            # and `WB` from winmm. Reading one of them as decimal does not
            # fail loudly: `int("00a")` raises, the line is skipped as a
            # half-written sample, and a run quietly loses every sample
            # with a button above 9 held.
            row = {k: int(v, 16 if k in ("B", "WB") else 10) for k, v in f.items()}
        except ValueError:
            continue
        if set(row) >= set(keys):
            out.append(row)
    return out


def span(rows, axis):
    vs = [r[axis] for r in rows]
    return min(vs), max(vs)


# ------------------------------------------------------------------ DOS

def build_dos():
    """PADTEST.COM on a FreeDOS floppy that runs it from AUTOEXEC."""
    com = os.path.join(OUT, "PADTEST.COM")
    img = os.path.join(OUT, "pad.img")
    sh("nasm", "-f", "bin", "-o", com, ASM)
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nPADTEST.COM\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::PADTEST.COM")
    return img


def run_dos(img, log, plog):
    """The player, with a gameport and a scripted pad, on the floppy."""
    for f in (log, plog):
        if os.path.exists(f):
            os.unlink(f)
    env = dict(os.environ, PLAYER_PAD="gameport",
               PLAYER_PAD_SCRIPT=script(offset=600, cycles=2))
    # A period-paced machine, because that is what the port is for: with
    # `-icount shift=N,align=on` the guest's counting loop and our
    # one-shot are paced by one clock, which is the arrangement a game
    # with a fixed timeout needs (docs/tracks/m13-gamepads.md, path B's
    # risk). The unthrottled control is what UNTHROTTLED=1 runs.
    icount = [] if os.environ.get("UNTHROTTLED") else ["-icount", "shift=7,align=on"]
    with open(plog, "wb") as out:
        p = subprocess.Popen([
            PLAYER, "--",
            "-L", os.path.join(ROOT, "qemu/pc-bios"),
            "-machine", "pc", "-cpu", "486", "-m", "16",
            "-vga", "cirrus", "-net", "none",
            "-audiodev", "none,id=a0",
            "-device", "gameport",
            "-drive", "file=%s,if=floppy,index=0,format=raw" % img,
            "-boot", "a", "-serial", "file:" + log, *icount,
        ], env=env, stdout=out, stderr=subprocess.STDOUT)
    try:
        return watch(p, log, "DONE", lambda t: check_dos(t, quiet=True), timeout=300)
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()


def check_dos(text, quiet=False):
    ok = True
    say = (lambda *a: None) if quiet else print
    rows = samples(text, ("X", "Y", "Z", "R", "B"))
    if "NOPORT" in text:
        say("FAIL the guest found no gameport at 0x201 (an idle read had an axis bit set)")
        return False
    if len(rows) < 20:
        say("FAIL only %d samples came back; the guest never really ran" % len(rows))
        return False
    if "CAPPED" in text:
        say("FAIL a counting loop hit its cap: an armed one-shot never expired")
        ok = False

    xlo, xhi = span(rows, "X")
    ylo, yhi = span(rows, "Y")
    zlo, zhi = span(rows, "Z")
    rlo, rhi = span(rows, "R")
    say("counts over %d samples: X %d..%d  Y %d..%d  Z %d..%d  R %d..%d"
        % (len(rows), xlo, xhi, ylo, yhi, zlo, zhi, rlo, rhi))
    say("button nibbles seen: %s" % " ".join(sorted({"%02x" % r["B"] for r in rows})))

    # The fourth axis is the one nothing in the script touches, so its own
    # spread is this guest's measurement noise, and every other axis is
    # judged against *that* rather than against a number chosen here.
    #
    # It has to be, because the noise is a property of the machine and not
    # of the port: a count is loop iterations, so it depends on how fast
    # the guest happens to be running. Paced (`-icount align=on`, what a
    # DOS machine gets) the same stick position comes back within a few
    # per cent; unpaced it wanders by half, because the host is deciding
    # the guest's speed from moment to moment. Both are seen here, and
    # only the second needs slack.
    jitter = rhi / max(rlo, 1)
    say("the undriven axis's own spread — this guest's loop jitter: %.2fx" % jitter)
    if jitter > 3.0:
        say("FAIL R spans %d..%d with nothing driving it: that is not jitter" % (rlo, rhi))
        ok = False
    # The stick, on X. Its two extremes are a 24 us pulse and a 1124 us
    # one, so the true ratio is about 46; ten is slack, and it must also
    # stand well clear of the noise floor above, since a model that drove all
    # four one-shots off one axis would move X and R together and pass a
    # fixed threshold.
    if xhi < max(10, 4 * jitter) * max(xlo, 1):
        say("FAIL X barely moved (%d..%d): the stick is not reaching the port" % (xlo, xhi))
        ok = False
    # ...and the centre, which is what a game calibrates against: 576 of
    # 1124 us, so about half of the long end.
    if not any(0.35 * xhi <= r["X"] <= 0.7 * xhi for r in rows):
        say("FAIL no sample had X near the middle of its range: a centred stick did not read centred")
        ok = False
    # The d-pad, on Y. Nothing in the script moves `ly`, so this is the
    # fold in gameport_set_state and nothing else.
    if yhi < max(10, 4 * jitter) * max(ylo, 1):
        say("FAIL Y barely moved (%d..%d): the d-pad is not reaching the axes" % (ylo, yhi))
        ok = False
    # The second stick, on Z: centre to the long end only, so ~1124/576.
    if zhi < 1.4 * jitter * max(zlo, 1):
        say("FAIL Z barely moved (%d..%d): the second stick is not reaching the port" % (zlo, zhi))
        ok = False

    # Buttons: low while held, and all four high with nothing pressed.
    seen = {r["B"] & 0xf0 for r in rows}
    for bit, name in ((0x10, "button 1 (south)"), (0x20, "button 2 (east)")):
        if not any(b & bit == 0 for b in seen):
            say("FAIL %s never read as pressed" % name)
            ok = False
    if 0xf0 not in seen:
        say("FAIL no sample had every button released")
        ok = False
    return ok


# -------------------------------------------------------------- Windows

IMAGES = {"xp": "~/vms/winxp.qcow2", "win98": "~/vms/win98.qcow2"}

# The display adapter to boot a Windows image on. It has to be the one the
# image's Windows already has a driver for: booting an image that runs our
# `d3dpt-vga` (doc 19) on a Cirrus instead is a hardware change, and the
# New Hardware wizard it brings up at start-up is a modal dialog sitting
# exactly where this harness wants to type. A named machine answers this
# out of its own bundle; `VGA=` overrides either way.


def resolve(arg):
    """A launcher machine's name, or a path to a disk image.

    Naming a machine is the useful form and it is what someone actually
    has: the pad's driver is installed in a *machine*, and the bundle
    already knows which disk that is and which display adapter its
    Windows has a driver for. Guessing either of those from the outside is
    how a run ends up typing into a New Hardware wizard.
    """
    if os.sep not in arg and not arg.endswith((".qcow2", ".raw", ".img")):
        toml = os.path.expanduser(
            "~/.local/share/2ksbox/machines/%s/machine.toml" % arg)
        if not os.path.exists(toml):
            raise SystemExit("no launcher machine %r (%s)" % (arg, toml))
        import tomllib
        with open(toml, "rb") as f:
            m = tomllib.load(f)
        # A relative disk is the bundle's own (`Machine::load`).
        return os.path.join(os.path.dirname(toml), m["disk"]), m.get("video", "cirrus")
    return os.path.expanduser(arg), "cirrus"


def vga_args(video):
    video = os.environ.get("VGA", video)
    if video in ("d3dpt", "d3dpt-vga"):
        return ["-vga", "none", "-device", "d3dpt-vga"]
    return ["-vga", video]


def drive_arg(image):
    """The image, never written to.

    `snapshot=on` rather than an overlay of our own (the same thing
    `scripts/test.sh`'s guest stage does with the same files), and an
    explicit format, because a raw image with no header to probe is
    otherwise a warning and a guess.
    """
    fmt = "raw" if image.endswith((".raw", ".img")) else "qcow2"
    return "file=%s,if=ide,index=0,media=disk,format=%s,snapshot=on" % (image, fmt)


def find_iso():
    isos = sorted(glob.glob(os.path.join(ROOT, "guest-tools/out/guest-tools-*.iso")),
                  key=os.path.getmtime)
    if not isos:
        raise SystemExit("no guest-tools ISO — guest-tools/build-wrappers.sh")
    return isos[-1]


def run_win(mode, image, video, log, plog):
    """The player with a `usb-gamepad`, and PADWIN.EXE off the ISO.

    `snapshot=on` rather than an overlay of our own: the user's images are
    read-only for a session, and this is the same thing `scripts/test.sh`'s
    guest stage does with the same file.
    """
    for f in (log, plog):
        if os.path.exists(f):
            os.unlink(f)
    iso = find_iso()
    sockdir = tempfile.mkdtemp(prefix="padq")     # short: AF_UNIX has 108 bytes
    sock = os.path.join(sockdir, "q")
    # Enough cycles that the poses are still coming round long after the
    # guest is up, and then some. Twelve cycles is about four and a half
    # minutes of frames, XP can take about as long to boot and start the
    # program, and then every sample comes back at the device's reset
    # state (a pad at rest, because the script has just finished). Sixty cycles is twenty-odd
    # minutes and costs nothing (540 steps in a list), and the run stops
    # the moment every pose has been seen.
    env = dict(os.environ, PLAYER_PAD="usb",
               PLAYER_PAD_SCRIPT=script(offset=0, cycles=60))
    # Windows 98 under KVM loses Explorer at startup (CLAUDE.md), and
    # without a shell there is no Run dialog to type into.
    accel = ["-accel", "tcg"] if mode == "win98" else ["-accel", "kvm"]
    with open(plog, "wb") as out:
        p = subprocess.Popen([
            PLAYER, "--",
            "-L", os.path.join(ROOT, "qemu/pc-bios"),
            "-machine", "pc", "-cpu", "pentium3", "-m", "512", *accel,
            *vga_args(video), "-net", "none",
            "-audiodev", "none,id=a0",
            # The controller and the pad, and **nothing else on the bus**.
            #
            # Measured on Windows 98, same image, same everything else:
            # with a `-device usb-tablet` beside it, DirectInput enumerated
            # *no* joystick at all; without it, the pad came up and every
            # check passed. XP did not care either way.
            #
            # The mechanism is not established. The obvious guess, that a
            # second HID device the image had never seen leaves the guest
            # in a modal New Hardware wizard, does not survive the owner
            # saying that machine has had a tablet before. What is
            # established is the A/B, and the pad needs a controller
            # rather than a pointer regardless, so there is nothing to
            # trade off here.
            "-usb", "-device", "usb-gamepad",
            "-drive", drive_arg(image),
            "-cdrom", iso, "-boot", "c",
            "-serial", "file:" + log,
            "-qmp", "unix:%s,server,nowait" % sock,
        ], env=env, stdout=out, stderr=subprocess.STDOUT)

    def qmp(*args):
        subprocess.run([sys.executable, QMPC, sock, *args],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.2)
        # Knock on the Run dialog until the guest says it started the
        # program. Nothing else proves a shell is there (a screendump
        # cannot tell a desktop from a dead machine), and the drive letter
        # is a property of the image, so both likely ones are tried.
        deadline = time.time() + 420
        started = False
        while time.time() < deadline and p.poll() is None and not started:
            for drive in ("D:", "E:"):
                # Esc first, twice: the previous attempt may have left a
                # "Windows cannot find" dialog up (the CD's letter is a
                # property of the image and not something this can know),
                # and one Esc goes to whatever had focus before it.
                qmp("keys", "esc")
                qmp("keys", "esc")
                qmp("keys", "meta_l+r")
                time.sleep(1)
                qmp("keys", "ctrl+a")
                qmp("type", r"%s\TESTS\PADWIN.EXE" % drive)
                qmp("keys", "ret")
                for _ in range(12):
                    time.sleep(1)
                    if os.path.exists(log) and b"padwin:" in open(log, "rb").read():
                        started = True
                        break
                if started:
                    break
        if not started:
            qmp("screendump", os.path.join(OUT, "%s-screen.png" % mode))
            raise SystemExit("the guest never started PADWIN.EXE (see %s and %s)"
                             % (log, os.path.join(OUT, "%s-screen.png" % mode)))
        text = watch(p, log, "DONE", lambda t: check_win(t, quiet=True), timeout=300)
        # A failing run leaves a picture of the desktop behind. What goes
        # wrong here is usually a *dialog* (Windows found new hardware and
        # is asking for its source files, and no log says so), and the
        # screen is the one place that shows it.
        if not check_win(text, quiet=True):
            qmp("screendump", os.path.join(OUT, "%s-screen.png" % mode))
        return text
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
        shutil.rmtree(sockdir, ignore_errors=True)


def check_win(text, quiet=False):
    ok = True
    say = (lambda *a: None) if quiet else print
    rows = samples(text, ("X", "Y", "Z", "Rz", "POV", "B"))
    if "FAIL no joystick enumerated" in text:
        say("FAIL the guest enumerated no joystick at all. Two things this image has to")
        say("     have had installed once, each of which asks Windows 98 for its own")
        say("     source files the first time and has nobody here to answer:")
        say("       * the USB *controller* — a machine whose bundle has neither the")
        say("         seamless mouse nor `pad = usb` has never had `-usb` on its command")
        say("         line at all, so adding one here is new hardware before the pad is;")
        say("       * the HID pad itself.")
        say("     Run the machine from the launcher once with the pad set to USB, answer")
        say("     the wizard, and this check has an image. See %s-screen.png for what the"
            % os.path.join(OUT, "win98"))
        say("     guest was actually showing.")
        return False
    for line in text.splitlines():
        if line.startswith("FAIL ") and not quiet:
            say("guest: " + line.strip())
    if len(rows) < 20:
        say("FAIL only %d samples came back; the guest never really ran" % len(rows))
        return False
    if not quiet:
        for line in text.splitlines():
            if line.startswith("device ") or line.startswith("winmm:") or line.startswith("DirectInput "):
                print("   " + line.strip())

    xlo, xhi = span(rows, "X")
    ylo, yhi = span(rows, "Y")
    zlo, zhi = span(rows, "Z")
    rzlo, rzhi = span(rows, "Rz")
    povs = {r["POV"] for r in rows}
    buttons = {r["B"] for r in rows}
    say("axes over %d samples (0..255, the report's own range): X %d..%d  Y %d..%d  Z %d..%d  Rz %d..%d"
        % (len(rows), xlo, xhi, ylo, yhi, zlo, zhi, rzlo, rzhi))
    say("POV values seen: %s" % " ".join(str(v) for v in sorted(povs)))
    say("button masks seen: %s" % " ".join("%03x" % b for b in sorted(buttons)))

    # The axes are on 0..255 because the probe puts them there, which is
    # the range the report itself carries, so these are the bytes
    # gamepad::hid_axis() made, not a fraction of something unknown.
    if xlo > 16 or xhi < 239:
        say("FAIL X reached %d..%d, not both ends of its range" % (xlo, xhi))
        ok = False
    if not any(96 <= r["X"] <= 160 for r in rows):
        say("FAIL no sample had X centred")
        ok = False
    if zhi - zlo < 100:
        say("FAIL Z barely moved (%d..%d): the second stick is not reaching the guest" % (zlo, zhi))
        ok = False
    # The two axes nothing drives. Y is the interesting one: on this path
    # the d-pad is the *hat*, and a device that drove the axes from it as
    # well (exactly what the gameport must do) would leave a stick the
    # guest cannot centre.
    for name, lo, hi in (("Y", ylo, yhi), ("Rz", rzlo, rzhi)):
        if hi - lo > 32:
            say("FAIL %s moved (%d..%d) with nothing driving it" % (name, lo, hi))
            ok = False
    # The hat, including its null state: -1 is "centred", and a descriptor
    # without the null state reads 0 (north) with nothing pressed.
    for want, name in ((0, "north"), (18000, "south"), (-1, "centred")):
        if want not in povs:
            say("FAIL the POV hat never read %s (%d)" % (name, want))
            ok = False
    for mask, name in ((0x001, "button 1 (south)"), (0x002, "button 2 (east)")):
        if not any(b & mask for b in buttons):
            say("FAIL %s never read as pressed" % name)
            ok = False
    if 0 not in buttons:
        say("FAIL no sample had every button released")
        ok = False
    return check_winmm(rows, quiet) and ok


def check_winmm(rows, quiet=False):
    """The same pad through winmm, and the reason M13 has no step 7.

    `joyGetPosEx` on top of 9x's VJOYD is what a great many mid-90s Windows
    titles call, and whether a USB HID pad arrives through it decides
    whether Windows 98 needs the gameport's *driver* half at all. It does
    arrive, so "Standard Game Port" through Add New Hardware was dropped by
    decision (the track doc's next steps), and this is the check that
    decision rests on. A claim that closes a milestone item has to be a
    check that can fail.

    Deliberately the same shape as the DirectInput assertions above rather
    than a weaker "it enumerated": an API that lists the pad and reads it
    centred for ever would pass the enumeration test and fail every game.
    """
    ok = True
    say = (lambda *a: None) if quiet else print
    if "WX" not in rows[0]:
        say("FAIL the samples carry no winmm columns — PADWIN.EXE predates"
            " this check, so the guest-tools ISO is stale"
            " (guest-tools/build-wrappers.sh)")
        return False
    # -1 in every field is the probe saying winmm has no joystick at all,
    # which is a different finding from one that reads centred and is the
    # one that would put step 7 back.
    if all(r["WX"] < 0 for r in rows):
        say("FAIL winmm enumerated no joystick: the pad reaches DirectInput"
            " and not the multimedia API, so a title that calls"
            " joyGetPosEx finds nothing")
        return False

    wxlo, wxhi = span(rows, "WX")
    wylo, wyhi = span(rows, "WY")
    wzlo, wzhi = span(rows, "WZ")
    wrlo, wrhi = span(rows, "WR")
    wpovs = {r["WPOV"] for r in rows}
    wbuttons = {r["WB"] for r in rows}
    if not quiet:
        say("winmm axes (rescaled to the same 0..255): X %d..%d  Y %d..%d  Z %d..%d  R %d..%d"
            % (wxlo, wxhi, wylo, wyhi, wzlo, wzhi, wrlo, wrhi))
        say("winmm POV values seen: %s (65535 = centred)"
            % " ".join(str(v) for v in sorted(wpovs)))
        say("winmm button masks seen: %s" % " ".join("%03x" % b for b in sorted(wbuttons)))

    if wxlo > 16 or wxhi < 239:
        say("FAIL winmm X reached %d..%d, not both ends of its range" % (wxlo, wxhi))
        ok = False
    if not any(96 <= r["WX"] <= 160 for r in rows):
        say("FAIL no sample had winmm X centred")
        ok = False
    if wzhi - wzlo < 100:
        say("FAIL winmm Z barely moved (%d..%d)" % (wzlo, wzhi))
        ok = False
    for name, lo, hi in (("Y", wylo, wyhi), ("R", wrlo, wrhi)):
        if hi - lo > 32:
            say("FAIL winmm %s moved (%d..%d) with nothing driving it" % (name, lo, hi))
            ok = False
    # winmm keeps the hat in its own units: hundredths of a degree, and
    # 65535 rather than -1 for centred. Left as the driver reports it: a
    # value this harness did not invent is the one worth asserting.
    for want, name in ((0, "north"), (18000, "south"), (65535, "centred")):
        if want not in wpovs:
            say("FAIL the winmm POV hat never read %s (%d)" % (name, want))
            ok = False
    for mask, name in ((0x001, "button 1 (south)"), (0x002, "button 2 (east)")):
        if not any(b & mask for b in wbuttons):
            say("FAIL winmm never read %s as pressed" % name)
            ok = False

    # The two columns are the same pad, and saying so is worth more than
    # two independent passes: an axis that moved in one API and not the
    # other would satisfy every check above. Rounding across two rescalings
    # is worth a couple of counts, and a pose changing between the two
    # reads a few more, so this is a majority statement rather than an
    # every-sample one.
    for di, wm in (("X", "WX"), ("Y", "WY"), ("Z", "WZ"), ("Rz", "WR")):
        agree = sum(1 for r in rows if abs(r[di] - r[wm]) <= 8)
        if agree * 10 < len(rows) * 9:
            say("FAIL %s and %s agree on only %d of %d samples: the two APIs"
                " are not reading the same axis"
                % (di, wm, agree, len(rows)))
            ok = False
    return ok


# ------------------------------------------------------------------ main

def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    mode = args[0] if args and args[0] in ("dos", "xp", "win98") else "dos"
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(PLAYER):
        raise SystemExit("no %s — scripts/build.sh mitsuami" % PLAYER)
    log = os.path.join(OUT, "%s-serial.log" % mode)
    plog = os.path.join(OUT, "%s-player.log" % mode)
    t0 = time.time()

    if mode == "dos":
        # nasm, mtools and the FreeDOS floppy, the same ones the x87 and
        # MIDI batteries use (fetched once into build/images/).
        x87gt.ensure_prereqs()
        x87gt.ensure_floppy()
        text = run_dos(build_dos(), log, plog)
        check = check_dos
    else:
        image, video = resolve(args[1] if len(args) > 1 else IMAGES[mode])
        if not os.path.exists(image):
            raise SystemExit("no image %s — name a launcher machine or pass a path" % image)
        print("%s: %s (snapshot), video %s, %s"
              % (mode, image, os.environ.get("VGA", video), os.path.basename(find_iso())))
        text = run_win(mode, image, video, log, plog)
        check = check_win

    print("%.0f s in the guest; %s" % (time.time() - t0, log))
    if "--verbose" in sys.argv:
        for line in text.splitlines():
            if line.strip():
                print("   ", line.strip())
    ok = check(text)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
