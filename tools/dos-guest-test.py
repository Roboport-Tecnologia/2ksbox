#!/usr/bin/env python3
"""The DOS machine family (doc 06) end to end.

A bundle the launcher writes, through `launcher --print-args`, into our
own qemu-system-i386, booting a real FreeDOS floppy. What it proves:

  1. `Machine::reference(Dos)` is doc 06's DOS machine: 64 MB, emulated,
     no NIC, and a processor chosen for it rather than "as fast as
     possible";
  2. the machine boots **from its floppy**, which is the `floppy` and
     `boot` fields doing their job (nothing else on the machine can
     print anything: the hard disk is blank);
  3. the processor combo is real. A fixed 200M-instruction loop is timed
     inside the guest's own run, and the rate it comes out at is compared
     with the rate the chosen CPU asks for. This is the check that
     matters: `-icount` without `align=on` only makes the guest *believe*
     it is slow, and that mistake is invisible from the outside;
  4. a throttled machine is emulated even when its bundle says KVM,
     because QEMU cannot do both.

Local only (it needs the FreeDOS floppy, fetched once from ibiblio like
the other DOS batteries) and not in scripts/test.sh, which stays free of
downloads. Outputs in build/dos-guest/.

  tools/dos-guest-test.py
"""
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/dos-guest")
QEMU = os.path.join(ROOT, "build/qemu/qemu-system-i386")
QEMU_IMG = os.path.join(ROOT, "build/qemu/qemu-img")
LAUNCHER = os.path.join(ROOT, "target/release/launcherx")

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

ITERS = 100_000_000          # of a two-instruction loop
INSNS = ITERS * 2

# (cpu_speed in the bundle, instructions/s it promises, tolerance)
# The promised rate is 2^-shift per ns; above ~30 M the alignment only
# corrects a guest that has fallen *behind*, so the fast settings are a
# ceiling rather than a rate and are checked as such (see CpuSpeed).
CASES = [
    ("unthrottled", None, None),
    ("486dx2-66", 31.25e6, 0.25),
    ("386dx-33", 7.8125e6, 0.25),
]

SPIN_ASM = """
        org 0x100
        mov dx, msg_start
        call puts
        mov ecx, %d
%%if %d > 0
.loop:  dec ecx
        jnz .loop
%%endif
        mov dx, msg_done
        call puts
        mov ax, 0x4c00
        int 0x21
puts:   mov ah, 0x09
        int 0x21
        ret
msg_start db 'SPINSTART',13,10,'$'
msg_done  db 'SPINDONE',13,10,'$'
"""


def sh(*cmd):
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)


def build_floppy():
    """The FreeDOS boot floppy plus the timing program, booting into it."""
    asm, com = os.path.join(OUT, "spin.asm"), os.path.join(OUT, "SPIN.COM")
    with open(asm, "w") as f:
        f.write(SPIN_ASM % (ITERS, ITERS))
    sh("nasm", "-f", "bin", "-o", com, asm)
    img = os.path.join(OUT, "dos.img")
    shutil.copy(x87gt.FLOPPY, img)
    cfg, bat = os.path.join(OUT, "FDCONFIG.SYS"), os.path.join(OUT, "FDAUTO.BAT")
    with open(cfg, "w") as f:
        # the SHELL= line is what boots FreeDOS straight into FDAUTO.BAT
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    with open(bat, "w") as f:
        f.write("@echo off\r\nSPIN.COM > COM1\r\nECHO SPINEXIT>COM1\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::SPIN.COM")
    return img


def write_bundle(path, disk, floppy, cpu_speed, accel="tcg"):
    with open(path, "w") as f:
        f.write('name = "DOS test box"\nfamily = "dos"\nram_mb = 64\n'
                'accel = "%s"\nnetwork = false\n'
                'disk = "%s"\nfloppy = "%s"\nboot = "floppy"\ncpu_speed = "%s"\n'
                % (accel, disk, floppy, cpu_speed))


def print_args(bundle):
    out = subprocess.run([LAUNCHER, "--print-args", bundle], check=True,
                         capture_output=True, text=True).stdout.strip()
    return out.split(" ")


def run(args, log, budget):
    """Boot with `args` and time the loop from the guest's own markers.

    The loop is timed between SPINSTART and SPINDONE appearing on COM1,
    not from the start of the run, so the BIOS and the FreeDOS boot are
    outside the measurement and each configuration is compared on the
    same work.
    """
    if os.path.exists(log):
        os.unlink(log)
    # `audiodev=embed0` is the player's own audio backend, which exists
    # inside the embed library and not out here: give the same name to a
    # null one so the machine's real device line can be used verbatim.
    p = subprocess.Popen([QEMU] + args + [
        "-audiodev", "none,id=embed0", "-display", "none",
        "-serial", "file:" + log, "-monitor", "none"],
        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    t0 = t_start = None
    deadline = time.time() + budget
    try:
        while time.time() < deadline:
            time.sleep(0.01)
            if os.path.exists(log):
                with open(log, "rb") as f:
                    text = f.read()
                if t_start is None and b"SPINSTART" in text:
                    t_start = time.time()
                if b"SPINDONE" in text:
                    return t_start, time.time() - t_start
            if p.poll() is not None:
                break
        return t_start, None
    finally:
        p.kill()
        p.wait()
    return t0, None


def main():
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(LAUNCHER):
        raise SystemExit("%s not built (cargo build --release -p launcher-core --bin launcherx)" % LAUNCHER)

    failures = []

    def check(ok, name, detail=""):
        print("  %s %s%s" % ("PASS" if ok else "FAIL", name, ("  " + detail) if detail else ""))
        if not ok:
            failures.append(name)

    floppy = build_floppy()
    disk = os.path.join(OUT, "blank.qcow2")
    if os.path.exists(disk):
        os.unlink(disk)
    sh(QEMU_IMG, "create", "-q", "-f", "qcow2", disk, "16M")

    # 1. the family's own reference defaults, through the launcher that
    #    writes them (a scratch library: never the user's own)
    lib = os.path.join(OUT, "library")
    shutil.rmtree(lib, ignore_errors=True)
    env = dict(os.environ, LAUNCHER_LIBRARY_DIR=lib)
    ref = subprocess.run([LAUNCHER, "--new", "dos", "DOS reference", disk],
                         check=True, capture_output=True, text=True, env=env).stdout.strip()
    toml = open(ref).read()
    print("== the DOS family's reference machine (%s)" % os.path.relpath(ref, ROOT))
    for line in toml.strip().splitlines():
        print("   " + line)
    check('family = "dos"' in toml, "the bundle is a DOS machine")
    check("ram_mb = 64" in toml, "64 MB (doc 06)")
    check('accel = "tcg"' in toml, "emulated, because a throttle needs TCG")
    check("network = false" in toml, "no network card")
    check('cpu_speed = "486dx2-66"' in toml, "a period processor, not full speed")

    # 2. a throttled machine is emulated even when the bundle says KVM
    kvm_bundle = os.path.join(OUT, "kvm.toml")
    write_bundle(kvm_bundle, disk, floppy, "486dx2-66", accel="kvm")
    args = print_args(kvm_bundle)
    # The accelerator is spelled `-accel`, not `-machine accel=`, because
    # the TCG-side optimizations are properties of the accelerator object
    # and the two spellings cannot be mixed; a machine that has changed
    # none of them carries a bare `tcg` here.
    accel = [args[i + 1] for i, a in enumerate(args[:-1]) if a == "-accel"]
    check(accel == ["tcg"], "a throttled machine runs emulated even when it asks for KVM",
          " ".join("-accel %s" % a for a in accel) or "(no -accel)")

    # 3. the guest itself, once per processor setting
    print("== a real FreeDOS guest, %d instructions of loop" % INSNS)
    rates = {}
    for speed, want, tol in CASES:
        bundle = os.path.join(OUT, "%s.toml" % speed)
        write_bundle(bundle, disk, floppy, speed)
        args = print_args(bundle)
        icount = [a for a in args if a.startswith("shift=")]
        budget = 60 if want is None else INSNS / want * 3 + 60
        t_start, secs = run(args, os.path.join(OUT, "serial-%s.log" % speed), budget)
        booted = t_start is not None
        check(booted, "%-12s booted from its floppy" % speed)
        if not booted or secs is None:
            check(False, "%-12s finished the loop" % speed)
            continue
        rate = INSNS / secs
        rates[speed] = rate
        detail = "%.2f s, %.1f M instructions/s%s" % (
            secs, rate / 1e6, (" (%s)" % icount[0]) if icount else "")
        if want is None:
            check(rate > 50e6, "%-12s runs at emulation speed" % speed, detail)
        else:
            # a ceiling with a floor: the throttle must actually bite, and
            # must not undershoot so far that the machine is unusable
            ok = rate <= want * (1 + tol) and rate >= want * (1 - tol)
            check(ok, "%-12s runs at the rate it promises (%.1f M)" % (speed, want / 1e6), detail)

    if "unthrottled" in rates and "386dx-33" in rates:
        ratio = rates["unthrottled"] / rates["386dx-33"]
        check(ratio > 10, "the slowest processor is far slower than no throttle",
              "%.0fx" % ratio)

    print()
    if failures:
        print("dos guest test: FAIL (%s)" % ", ".join(failures))
        return 1
    print("dos guest test: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
