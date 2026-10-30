#!/usr/bin/env python3
"""The PIT as a DOS game's clock meets it (patch 34): guest-tools'
QCLOCK.COM (DOS Quake's Sys_FloatTime, the BIOS tick word plus PIT
counter 0 in mode 2, read in a tight loop beside the TSC) under FreeDOS,
twice: on this QEMU as built, and with `-global isa-pit.overdue-irq=off`,
upstream's behaviour, as the control.

What it guards: QEMU computes the counter from the clock at every read,
but raises the IRQ 0 edge at the counter's wrap from a timer the main loop
runs a wakeup late. A read in that gap sees the counter wrapped and the
tick not yet counted (time going backward), and Quake counts a backward
step as nothing while taking it as its new reference, so the whole period
is counted again when the tick lands. Measured without the patch: one
full 55 ms backward step on every tick, 540 of 540, Quake's
clock at exactly 200 % in every window. With it, a port access delivers
the overdue edge first, and that must read as no backward step at all and
every window at 100 %.

The control is reported, not required: whether the gap is ever hit
depends on how late this host's main loop wakes, so a host where it is not
is one where this check cannot fail, and the run says so instead of
failing.

**The rate phase** (patch 65) asks a different question of the same
timer: does every IRQ 0 of a 1 kHz PIT arrive? guest-tools' PITRATE.COM
programs counter 0 to 1 ms (what Windows 9x's multimedia timer does for a
MIDI sequencer), counts IRQ 0 itself and prints a line
per 1000 ticks; the host timestamps the lines, so 1.000 s apart is a
guest clock at 100 %. It runs as built, then with QEMU's waits rounded
to Windows' 15.6 ms timer tick (tools/wait-granularity.c, Linux only),
spinning and halted, and all three must be 97-103 %. The control,
`-global isa-pit.reinject=off` under the same waits, is reported. It
measured 6 %: every transition that came due while the main loop slept
was raised back to back, one interrupt on the edge-triggered 8259.

    tools/pit-guest-test.py        # needs nasm, mtools, build/qemu
    PIT_SECS=30 tools/pit-guest-test.py
    PIT_RATE=0 tools/pit-guest-test.py   # without the rate phase

Outputs in build/pit-guest/. The `pit-guest` check in scripts/test.sh.
"""
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import qemuhost  # tools/qemuhost.py: the platform's QEMU and QMP address
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = qemuhost.qemu(ROOT)   # this checkout's build, or $QEMU_BIN
SRC = os.path.join(ROOT, "guest-tools/src/qclock.asm")
OUT = os.path.join(ROOT, "build/pit-guest")
SECS = int(os.environ.get("PIT_SECS", "10"))

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

WIN = re.compile(rb"^\s*(\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)")


def run(tag, extra):
    log = os.path.join(OUT, "serial-%s.log" % tag)
    if os.path.exists(log):
        os.unlink(log)
    img = os.path.join(OUT, "fd.img")
    p = subprocess.Popen([
        QEMU, "-machine", "pc,hpet=off", *x87gt.tcg_opts(), "-cpu", "pentium3", "-m", "64",
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        "-monitor", "none", "-drive", "file=%s,format=raw,if=floppy" % img, "-boot", "a",
        "-serial", "file:" + log, *extra,
    ])
    t0 = time.time()
    try:
        # the program prints only at the end, so DONE is the whole answer
        while time.time() - t0 < SECS * 4 + 120:
            time.sleep(1)
            if p.poll() is not None:
                break
            if os.path.exists(log) and b"DONE" in open(log, "rb").read():
                break
        else:
            raise SystemExit("FAIL %s: no DONE on COM1 within %d s" % (tag, SECS * 4 + 120))
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    data = open(log, "rb").read().replace(b"\r", b"")
    wins = [WIN.match(l) for l in data.split(b"\n")]
    wins = [tuple(int(x) for x in m.groups()) for m in wins if m]
    total = re.search(rb"total clock_ms=(-?\d+) quake_ms=(-?\d+) gain_ms=(-?\d+) backsteps=(\d+)", data)
    if not wins or not total:
        raise SystemExit("FAIL %s: no report in %s" % (tag, log))
    return wins, tuple(int(x) for x in total.groups()), data


RATE_SRC = os.path.join(ROOT, "guest-tools/src/pitrate.asm")
SHIM_SRC = os.path.join(ROOT, "tools/wait-granularity.c")


def rate_run(tag, img, lines, extra, env_extra):
    """PITRATE.COM's lines as they arrive; the seconds per 1000 ticks."""
    env = dict(os.environ)
    env.update(env_extra)
    p = subprocess.Popen([
        QEMU, "-machine", "pc,hpet=off", *x87gt.tcg_opts(), "-cpu", "pentium3", "-m", "64",
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        "-monitor", "none", "-drive", "file=%s,format=raw,if=floppy" % img, "-boot", "a",
        "-serial", "stdio", *extra,
    ], stdout=subprocess.PIPE, stdin=subprocess.DEVNULL, env=env)
    stamps, line = [], b""
    deadline = time.monotonic() + 60 + lines * 20
    try:
        while time.monotonic() < deadline:
            c = p.stdout.read(1)
            if not c:
                break
            if c != b"\n":
                line += c
                continue
            line, got = b"", line.strip()
            if got == b"T":
                stamps.append(time.monotonic())
            elif got == b"DONE":
                break
    finally:
        p.terminate()
        p.wait()
    gaps = [b - a for a, b in zip(stamps, stamps[1:])]
    if not gaps:
        raise SystemExit("FAIL rate %s: %d lines from PITRATE.COM" % (tag, len(stamps)))
    mean = sum(gaps) / len(gaps)
    print("rate %-28s %2d lines, %.3f s per 1000 ticks (%.3f..%.3f): guest clock at %.0f %%"
          % (tag, len(stamps), mean, min(gaps), max(gaps), 100 / mean))
    return 100 / mean


def rate_phase():
    com = os.path.join(OUT, "PITRATE.COM")
    subprocess.run(["nasm", "-f", "bin", "-o", com, RATE_SRC], check=True)
    img = {}
    for mode, lines in (("spin", SECS), ("halt", SECS), ("ctl", 3)):
        img[mode] = os.path.join(OUT, "fd-rate-%s.img" % mode)
        shutil.copy(os.path.join(OUT, "fd.img"), img[mode])
        bat = os.path.join(OUT, "FDAUTO.BAT")
        with open(bat, "w") as f:
            f.write("@echo off\r\nPITRATE.COM %s%d\r\n" % ("H " if mode == "halt" else "", lines + 1))
        for src, dst in ((bat, "::FDAUTO.BAT"), (com, "::PITRATE.COM")):
            subprocess.run(["mcopy", "-o", "-i", img[mode], src, dst], check=True)
    ok = True
    good = lambda pct: 97 <= pct <= 103
    if not good(rate_run("as built", img["spin"], SECS, [], {})):
        ok = False
    shim = os.path.join(OUT, "wait-granularity.so")
    if not sys.platform.startswith("linux") or shutil.which("cc") is None:
        print("rate: SKIP the Windows-wait runs (tools/wait-granularity.c is a Linux LD_PRELOAD)")
        return ok
    subprocess.run(["cc", "-O2", "-shared", "-fPIC", "-o", shim, SHIM_SRC, "-ldl"], check=True)
    coarse = {"LD_PRELOAD": shim}
    for mode in ("spin", "halt"):
        if not good(rate_run("15.6 ms waits, %s" % mode, img[mode], SECS, [], coarse)):
            ok = False
    rate_run("15.6 ms waits, reinject=off", img["ctl"], 3, ["-global", "isa-pit.reinject=off"], coarse)
    if not ok:
        print("FAIL: a 1 kHz guest clock off 100 % -- IRQ 0 edges lost at the 8259")
    return ok


def main():
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    com = os.path.join(OUT, "QCLOCK.COM")
    subprocess.run(["nasm", "-f", "bin", "-o", com, SRC], check=True)
    img = os.path.join(OUT, "fd.img")
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nQCLOCK.COM %d\r\n" % SECS)
    for src, dst in ((cfg, "::FDCONFIG.SYS"), (bat, "::FDAUTO.BAT"), (com, "::QCLOCK.COM")):
        subprocess.run(["mcopy", "-o", "-i", img, src, dst], check=True)

    ok = True
    wins, (clock, quake, gain, back), data = run("on", [])
    speeds = [w[4] for w in wins]
    print("overdue-irq=on:  %d windows, speed%% %d..%d, backsteps %d, Quake %d ms over %d ms (gain %+d)"
          % (len(wins), min(speeds), max(speeds), back, quake, clock, gain))
    if back:
        print("FAIL: %d backward readings — a tick still reaches the guest after its counter wrapped" % back)
        ok = False
    bad = [w for w in wins if not 97 <= w[4] <= 103]
    if bad:
        print("FAIL: windows off 100 %%: %s" % ", ".join("%d:%d%%" % (w[0], w[4]) for w in bad))
        ok = False
    if abs(gain) * 100 > clock:
        print("FAIL: Quake's clock is %+d ms off the tick clock's %d ms" % (gain, clock))
        ok = False
    if not ok:
        sys.stdout.write(data.decode("ascii", "replace"))

    cwins, (cclock, cquake, cgain, cback), _ = run("off", ["-global", "isa-pit.overdue-irq=off"])
    cspeeds = [w[4] for w in cwins]
    print("overdue-irq=off: %d windows, speed%% %d..%d, backsteps %d, Quake %d ms over %d ms (gain %+d)"
          % (len(cwins), min(cspeeds), max(cspeeds), cback, cquake, cclock, cgain))
    if not cback:
        print("note: the control never hit the gap on this host — it cannot show the "
              "difference here, so a PASS above proves less than it does elsewhere")
    if os.environ.get("PIT_RATE", "1") != "0" and not rate_phase():
        ok = False
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
