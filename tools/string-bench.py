#!/usr/bin/env python3
"""String-instruction throughput under TCG: a DOS program times rep movsd /
movsb / stosb and repne scasb over 8 KB buffers with the BIOS tick counter
(18.2 Hz) and prints one line per test on COM1. Boots it on the FreeDOS test
floppy (fetched by tools/x87-guest-test.py on first use) under each QEMU
binary given, so two builds can be compared side by side:

    tools/string-bench.py                                  # build/qemu/qemu-system-i386
    tools/string-bench.py --qemu old/qemu-system-i386 --qemu build/qemu/qemu-system-i386

Prints ns per element for each test and binary, plus the ratio against the
first binary. Backs the decision on patch 09 (upstream's repeated-string
series, QEMU 10.0): docs/00-status.md, patches/qemu/README.md.
"""
import argparse
import importlib.util
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/string-bench")
TICK_NS = 1e9 / 18.2065

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

ASM = r"""
org 100h
bits 16
; each test: align to a tick edge, ITER passes over an 8 KB buffer, print
; "<NAME> <elements> <ticks>" (hex) on COM1. Data lives on its own pages so
; the stores never invalidate the page holding the code.
%define BUF 8192

start:
    xor ax, ax
    mov es, ax
    push cs
    pop es
    push cs
    pop ds
    mov al, 1
    mov di, src
    mov cx, BUF
    rep stosb                   ; src: nonzero bytes, scasb terminator at the end
    mov byte [src + BUF - 1], 0

    ; --- rep movsd, 2048 dwords x 400000
    mov si, str_movsd
    call puts
    call tick_sync
    mov ebx, 400000
.l1:
    mov si, src
    mov di, dst
    mov cx, BUF / 4
    rep movsd
    dec ebx
    jnz .l1
    mov eax, 400000 * (BUF / 4)
    call report

    ; --- rep movsb, 8192 bytes x 100000
    mov si, str_movsb
    call puts
    call tick_sync
    mov ebx, 100000
.l2:
    mov si, src
    mov di, dst
    mov cx, BUF
    rep movsb
    dec ebx
    jnz .l2
    mov eax, 100000 * BUF
    call report

    ; --- rep stosb, 8192 bytes x 100000
    mov si, str_stosb
    call puts
    call tick_sync
    mov ebx, 100000
.l3:
    mov di, dst
    mov cx, BUF
    mov al, 7
    rep stosb
    dec ebx
    jnz .l3
    mov eax, 100000 * BUF
    call report

    ; --- rep stosd, 2048 dwords x 400000
    mov si, str_stosd
    call puts
    call tick_sync
    mov ebx, 400000
.l4:
    mov di, dst
    mov cx, BUF / 4
    mov eax, 07070707h
    rep stosd
    dec ebx
    jnz .l4
    mov eax, 400000 * (BUF / 4)
    call report

    ; --- repne scasb (strlen over 8 KB) x 100000
    mov si, str_scasb
    call puts
    call tick_sync
    mov ebx, 100000
.l5:
    mov di, src
    mov cx, BUF
    xor al, al
    repne scasb
    dec ebx
    jnz .l5
    mov eax, 100000 * BUF
    call report

    mov si, str_done
    call puts
    int 20h

tick_sync:                      ; wait for a tick edge, remember it
    push ax
    push si
    push es
    xor ax, ax
    mov es, ax
    mov si, [es:046Ch]
.s: mov ax, [es:046Ch]
    cmp ax, si
    je .s
    mov [cs:t0], ax
    pop es
    pop si
    pop ax
    ret

report:                         ; eax = elements; prints "<elements> <ticks>\n"
    push bx
    push es
    xor bx, bx
    mov es, bx
    mov bx, [es:046Ch]
    sub bx, [cs:t0]
    call put_hex32
    mov al, ' '
    call putc
    mov ax, bx
    call put_hex16
    mov al, 10
    call putc
    pop es
    pop bx
    ret

putc:
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
puts:
    lodsb
    test al, al
    jz .d
    call putc
    jmp puts
.d: ret
put_hex32:
    push eax
    shr eax, 16
    call put_hex16
    pop eax
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

str_movsd: db "MOVSD ", 0
str_movsb: db "MOVSB ", 0
str_stosb: db "STOSB ", 0
str_stosd: db "STOSD ", 0
str_scasb: db "SCASB ", 0
str_done:  db "DONE", 10, 0
t0: dw 0
times (2000h - ($ - $$)) db 0
src: times BUF db 0
dst: times BUF db 0
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--qemu", action="append", default=[], help="qemu-system-i386 to run (repeatable; default build/qemu/qemu-system-i386)")
    ap.add_argument("--cpu", default="pentium3", help="-cpu model (default pentium3)")
    a = ap.parse_args()
    qemus = a.qemu or [x87gt.QEMU]
    if shutil.which("nasm") is None or shutil.which("mcopy") is None:
        raise SystemExit("needs nasm and mtools")
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    asm = os.path.join(OUT, "strbench.asm")
    com = os.path.join(OUT, "STRBENCH.COM")
    with open(asm, "w") as f:
        f.write(ASM)
    x87gt.sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    img = os.path.join(OUT, "strbench.img")
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nSTRBENCH.COM\r\n")
    x87gt.sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    x87gt.sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    x87gt.sh("mcopy", "-o", "-i", img, com, "::STRBENCH.COM")

    results = []
    for i, q in enumerate(qemus):
        log = os.path.join(OUT, "serial-%d.log" % i)
        if os.path.exists(log):
            os.unlink(log)
        p = subprocess.Popen([
            q, "-machine", "pc", "-cpu", a.cpu, "-m", "64",
            "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
            "-fda", img, "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
        ])
        t0 = time.time()
        try:
            while time.time() - t0 < 600:
                time.sleep(0.5)
                if p.poll() is not None:
                    break
                if os.path.exists(log) and b"DONE" in open(log, "rb").read():
                    break
            else:
                raise SystemExit("timeout (%s)" % q)
        finally:
            if p.poll() is None:
                p.terminate()
                p.wait()
        r = {}
        for line in open(log, "rb").read().split(b"\n"):
            parts = line.split()
            if len(parts) == 3 and parts[0] in (b"MOVSD", b"MOVSB", b"STOSB", b"STOSD", b"SCASB"):
                elems, ticks = int(parts[1], 16), int(parts[2], 16)
                r[parts[0].decode()] = ticks * TICK_NS / elems if ticks else float("nan")
        if len(r) != 5:
            raise SystemExit("%s: incomplete output in %s" % (q, log))
        results.append((q, r))

    names = ["MOVSD", "MOVSB", "STOSB", "STOSD", "SCASB"]
    print("ns per element (lower is better)")
    print("%-8s" % "" + "".join("%14s" % n for n in names))
    base = results[0][1]
    for q, r in results:
        print("%-8s" % ("#%d" % results.index((q, r))) + "".join("%14.2f" % r[n] for n in names) + "   " + q)
        if r is not base:
            print("%-8s" % "ratio" + "".join("%13.2fx" % (base[n] / r[n]) for n in names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
