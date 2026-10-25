#!/usr/bin/env python3
"""Random x87 sequences, fast path on against off (doc 13).

tools/x87-guest-test.py checks each instruction alone from a clean
stack. This one checks what only a sequence inside one translation block
can get wrong: the shadow stack and its tags across many ops, a store
read back, and fldcw switching the x87 mode in the middle of a block (a
game's float-to-int: fnstcw, or 0x0c00, fldcw, fistp, fldcw back, around
every conversion; PC=64 next to PC=53 and PC=24). Vice City's player
position turned to NaN on Win98 at PC=64 with the fast path on and never
with it off (2026-10-25).

Each sequence starts from fninit at a random control word, loads a few
values, runs straight-line random x87 code over a 64-byte memory area,
then dumps the control, status and tag words, the eight registers and the
memory area to COM1 in hex: identical with x87-fast on and off, or the
first differing sequences are printed with their code.

    tools/x87-fuzz-guest-test.py [seed] [files]   # 8 .COM files of ~200 sequences by default
"""
import importlib.util
import os
import random
import re
import shutil
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
spec = importlib.util.spec_from_file_location("x87gt", os.path.join(HERE, "x87-guest-test.py"))
gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gt)

OUT = os.path.join(gt.ROOT, "build/x87-fuzz")
PER_FILE = 150
# STRICT=1: no stack under- or overflow, no fincstp, fdecstp or ffree
STRICT = os.environ.get("STRICT") == "1"

# control words: RC nearest PE masked at each precision (the fast modes),
# and chop (mode 0, the float-to-int idiom)
CWS = [0x037F, 0x027F, 0x007F, 0x0F7F, 0x0E7F, 0x0C7F, 0x033F]


def values(rng):
    """80-bit operands: the battery's edge cases and a game's plain numbers"""
    pool = gt.pool_entries()
    for _ in range(64):
        d = rng.choice([rng.uniform(-2000, 2000), rng.uniform(-1, 1), rng.uniform(-1e-3, 1e-3),
                        float(rng.randint(-100, 100)), rng.uniform(0, 80.0)])
        if rng.random() < 0.5:
            d = struct.unpack("<f", struct.pack("<f", d))[0]
        pool.append(gt.d2x80(d))
    for _ in range(16):     # full 64-bit mantissas
        pool.append((rng.getrandbits(64) | (1 << 63), rng.choice([0x3FFF, 0x4005, 0x3FF5, 0xC002, 0x400A])))
    return pool


def mem_bytes(rng):
    """the 64-byte memory area's start: floats, doubles and small ints"""
    b = b""
    while len(b) < 64:
        k = rng.random()
        if k < 0.4:
            b += struct.pack("<f", rng.choice([rng.uniform(-500, 500), rng.uniform(-1, 1), 0.0, 1.0, 80.0]))
        elif k < 0.7:
            b += struct.pack("<d", rng.uniform(-5000, 5000))
        elif k < 0.9:
            b += struct.pack("<i", rng.randint(-1000, 1000))
        else:
            b += struct.pack("<I", rng.getrandbits(32))
    return b[:64]


ARITH = ["fadd", "fsub", "fsubr", "fmul", "fdiv", "fdivr"]


class Gen:
    def __init__(self, rng, npool):
        self.rng, self.npool, self.label = rng, npool, 0

    def m(self, size):
        step = {2: 2, 4: 4, 8: 8, 10: 16}[size]
        return "[mem+%d]" % (self.rng.randrange(0, 64 - size + 1, step) if size != 10 else self.rng.choice([0, 16, 32, 48]))

    def op(self, d):
        """one instruction for stack depth d -> (text, new depth); now and then
        one that under- or overflows, which must agree too"""
        r, m = self.rng, self.m
        loose = not STRICT and r.random() < 0.03
        i = r.randrange(max(d, 1)) if not loose else r.randrange(8)
        cands = []
        if d < 8 or loose:
            cands += [("fld st%d" % i, 1), ("fld dword " + m(4), 1), ("fld qword " + m(8), 1),
                      ("fld tword " + m(10), 1), ("fld tword [pool+%d]" % (10 * r.randrange(self.npool)), 1),
                      ("fild word " + m(2), 1), ("fild dword " + m(4), 1), ("fild qword " + m(8), 1),
                      (r.choice(["fld1", "fldz", "fldpi", "fldl2e", "fldln2"]), 1)]
        if d >= 1 or loose:
            a = r.choice(ARITH)
            cands += [("fst dword " + m(4), 0), ("fst qword " + m(8), 0), ("fstp dword " + m(4), -1),
                      ("fstp qword " + m(8), -1), ("fstp tword " + m(10), -1),
                      ("fist word " + m(2), 0), ("fist dword " + m(4), 0), ("fistp dword " + m(4), -1),
                      ("fistp word " + m(2), -1), ("fistp qword " + m(8), -1),
                      ("%s dword %s" % (a, m(4)), 0), ("%s qword %s" % (a, m(8)), 0),
                      ("fi%s dword %s" % (a[1:], m(4)), 0), ("fi%s word %s" % (a[1:], m(2)), 0),
                      ("%s st0, st%d" % (a, i), 0), ("fchs", 0), ("fabs", 0), ("ftst", 0), ("fxam", 0),
                      ("fsqrt", 0), ("frndint", 0), ("fst st%d" % i, 0), ("fstp st%d" % i, -1),
                      ("fxch st%d" % i, 0), ("fcom st%d" % i, 0), ("fucom st%d" % i, 0),
                      ("fcomp st%d" % i, -1), ("fcom dword " + m(4), 0), ("fcomp qword " + m(8), -1),
                      ("fnstsw word [mem+62]", 0), ("fnstsw ax\n    mov [mem+60], ax", 0)]
            # the arithmetic again, so it dominates as in a game
            cands += [("%s dword %s" % (r.choice(ARITH), m(4)), 0), ("%s st0, st%d" % (r.choice(ARITH), i), 0)] * 3
        if d >= 2 or loose:
            j = r.randrange(1, max(d, 2)) if not loose else r.randrange(1, 8)
            a = r.choice(ARITH)
            cands += [("%s st%d, st0" % (a, j), 0), ("%sp st%d, st0" % (a, j), -1), ("fcompp", -2),
                      ("fucompp", -2), ("fucomp st%d" % j, -1), ("fscale", 0), ("fprem", 0)] * 2
        cands += [("fincstp", 0), ("fdecstp", 0), ("ffree st%d" % r.randrange(8), 0)] if not STRICT and r.random() < 0.05 else []
        t, dd = r.choice(cands)
        return t, max(0, min(8, d + dd))

    def seq(self, n, cw):
        r = self.rng
        lines, d = [], 0
        for _ in range(r.randint(1, 6)):
            lines.append("fld tword [pool+%d]" % (10 * r.randrange(self.npool)))
            d += 1
        k = 0
        while k < n:
            x = r.random()
            if x < 0.12:
                # the float-to-int idiom, as compilers emit it
                lines += ["fnstcw [cwsave]", "mov ax, [cwsave]", "or ah, 0ch", "mov [cwtmp], ax", "fldcw [cwtmp]"]
                for _ in range(r.randint(1, 2)):
                    if r.random() < 0.3 or (STRICT and d == 0):
                        t, d = self.op(d)
                    else:
                        t = r.choice(["fistp dword " + self.m(4), "fist dword " + self.m(4), "fistp qword " + self.m(8)])
                        d = max(0, d - 1) if t.startswith("fistp") else d
                    lines.append(t)
                lines.append("fldcw [cwsave]")
            elif x < 0.17:
                lines.append("fldcw [cws+%d]" % (2 * r.randrange(len(CWS))))
            elif x < 0.20 and d >= 2:
                # a compare's branch ends the block
                self.label += 1
                lines += ["fcom st1", "fnstsw ax", "sahf", "jb .b%d" % self.label, "fchs", ".b%d:" % self.label]
            else:
                t, d = self.op(d)
                lines.append(t)
            k += 1
        return lines


ASM_HEAD = r"""
org 100h
bits 16
start:
    finit
"""

ASM_TAIL = r"""
    mov si, str_done
    call puts
    mov ax, 4c00h
    int 21h

; dump: control, status and tag words (12 bytes of the 32-bit fnsave), the
; eight registers (80 bytes), the memory area (64 bytes), a newline
dump:
    o32 fnsave [fsbuf]
    mov si, fsbuf
    mov cx, 12
    call hex
    mov si, fsbuf+28
    mov cx, 80
    call hex
    mov si, mem
    mov cx, 64
    call hex
    mov al, 10
    call putc
    ret

hex:                            ; si, cx bytes
.l: lodsb
    push ax
    shr al, 4
    call nib
    pop ax
    and al, 0fh
    call nib
    loop .l
    mov al, ' '
    call putc
    ret
nib:
    add al, '0'
    cmp al, '9'
    jbe putc
    add al, 7
putc:
    push dx
    push ax
    mov dx, 3fdh
.w: in al, dx
    test al, 20h
    jz .w
    pop ax
    mov dx, 3f8h
    out dx, al
    pop dx
    ret
puts:
    lodsb
    test al, al
    jz .e
    call putc
    jmp puts
.e: ret

str_done: db "DONE", 13, 10, 0
align 16
cwsave: dw 0
cwtmp: dw 0
cws: dw {cws}
align 16
fsbuf: times 108 db 0
align 16
mem: times 64 db 0
align 16
pool:
{pool}
seqmem:
{seqmem}
"""


def build(seed, nfiles):
    """the pool's assembly and nfiles * PER_FILE sequences (cw, code, memory)"""
    rng = random.Random(seed)
    pool = values(rng)
    pool_asm = "\n".join("    db " + ", ".join("0%02xh" % x for x in struct.pack("<QH", lo, hi)) for lo, hi in pool)
    gen = Gen(rng, len(pool))
    seqs = []
    for _ in range(nfiles * PER_FILE):
        cw = rng.choice(CWS[:3] + [0x037F] * 4)
        seqs.append((cw, gen.seq(rng.randint(8, 40), cw), mem_bytes(rng)))
    return pool_asm, seqs


def assemble(seqs, pool_asm):
    """.COM sources of PER_FILE sequences each; only the last prints DONE"""
    files = []
    for f in range(0, len(seqs), PER_FILE):
        body, mems = [], []
        for s, (cw, code, mb) in enumerate(seqs[f:f + PER_FILE]):
            mems.append("    db " + ", ".join("0%02xh" % x for x in mb))
            body.append("; seq %d cw %04x" % (f + s, cw))
            body += ["    finit", "    mov word [cwtmp], 0%04xh" % cw, "    fldcw [cwtmp]",
                     "    mov si, seqmem+%d" % (64 * s), "    mov di, mem", "    mov cx, 32", "    rep movsw"]
            # a branch's label once per copy (the shrinker runs many in a file)
            body += [re.sub(r"\.b(\d+)", r".s%d_b\1" % (f + s), "    " + l if not l.startswith(".") else l) for l in code]
            body.append("    call dump")
        tail = ASM_TAIL if f + PER_FILE >= len(seqs) else ASM_TAIL.replace('db "DONE", 13, 10, 0', "db 0")
        files.append(ASM_HEAD + "\n".join(body) + tail.format(
            cws=", ".join("0%04xh" % c for c in CWS), pool=pool_asm, seqmem="\n".join(mems)))
    return files


def run(seqs, pool_asm):
    """each sequence's dump line with the fast path off and on"""
    img = os.path.join(OUT, "fuzz.img")
    shutil.copy(gt.FLOPPY, img)
    names = []
    for k, asm in enumerate(assemble(seqs, pool_asm)):
        name = "FUZZ%02d" % k
        path = os.path.join(OUT, name + ".asm")
        with open(path, "w") as f:
            f.write(asm)
        gt.sh("nasm", "-O0", "-f", "bin", "-o", os.path.join(OUT, name + ".COM"), path)
        gt.sh("mcopy", "-o", "-i", img, os.path.join(OUT, name + ".COM"), "::%s.COM" % name)
        names.append(name)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\n" + "".join("%s.COM\r\n" % n for n in names))
    gt.sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    gt.sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    out = []
    for fast in ("off", "on"):
        data = open(gt.run_qemu(fast, img, os.path.join(OUT, "serial-%s.log" % fast)), "rb").read()
        out.append([l for l in data.replace(b"\r", b"").decode().split("\n") if l and not l.startswith("DONE")])
    if len(out[0]) != len(seqs) or len(out[1]) != len(seqs):
        raise SystemExit("x87 fuzz: expected %d sequences, got %d off and %d on" % (len(seqs), len(out[0]), len(out[1])))
    return out


def shrink(seq, pool_asm):
    """the sequence with lines dropped while it still differs: every
    one-line deletion in one boot, the first that differs kept"""
    cw, code, mb = seq
    while True:
        cands = [code[:i] + code[i + 1:] for i in range(len(code)) if not code[i].startswith(".")]
        # a branch's label goes with its jump
        cands = [c for c in cands if all(("jb " + l[:-1]) in "\n".join(c) for l in c if l.startswith("."))]
        if not cands:
            return cw, code, mb
        a, b = run([(cw, c, mb) for c in cands], pool_asm)
        hit = [i for i in range(len(cands)) if visible(a[i]) != visible(b[i])]
        if not hit:
            return cw, code, mb
        code = cands[hit[0]]


def visible(line):
    """a dump line without the registers tagged empty: what a popped
    register held is stale, kept by an x87 and not by the shadow stack,
    and no instruction but fnsave reads it"""
    env, regs, mem = line.split()
    sw = int(env[10:12] + env[8:10], 16)
    tw = int(env[18:20] + env[16:18], 16)
    top = (sw >> 11) & 7
    keep = [regs[20 * i:20 * i + 20] if (tw >> 2 * ((top + i) & 7)) & 3 != 3 else "-" for i in range(8)]
    return env, keep, mem


def main():
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 1
    nfiles = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    gt.ensure_prereqs()
    gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    pool_asm, seqs = build(seed, nfiles)
    t0 = time.time()
    a, b = run(seqs, pool_asm)
    print("x87 fuzz: %d sequences in %.1f s" % (len(seqs), time.time() - t0))
    bad = [i for i in range(len(seqs)) if visible(a[i]) != visible(b[i])]
    if not bad:
        print("x87 fuzz (seed %d): %d sequences identical with the fast path on and off" % (seed, len(seqs)))
        return 0
    print("x87 fuzz (seed %d): MISMATCH in %d of %d sequences; the first ones shrunk:" % (seed, len(bad), len(seqs)))
    for i in bad[:int(os.environ.get("SHRINK", "3"))]:
        cw, code, mb = shrink(seqs[i], pool_asm)
        x, y = run([(cw, code, mb)], pool_asm)
        print("--- sequence %d, cw %04x, memory %s" % (i, cw, mb.hex()))
        for l in code:
            print("    " + l.replace("\n", "; "))
        print("  off: " + x[0])
        print("  on:  " + y[0])
    return 1


if __name__ == "__main__":
    sys.exit(main())
