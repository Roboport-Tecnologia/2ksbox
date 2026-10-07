#!/usr/bin/env python3
"""Self-modifying code under TCG with the same-value store skip and the
run-time reading of patched operands on and off (patches 18 and 24, M9 track):
a DOS program patches its own instructions the ways the software-rendered
games do: an immediate rewritten with new values from
another block and from the block being executed (precise SMC), rewritten
with the value already there, an opcode byte flipped, 16- and 8-bit partial
patches of an imm32, a routine overwritten by `rep movsd` (the probe path)
with new and with identical bytes, an imm32 straddling a page boundary
written by one crossing store, a memory operand's disp32 rewritten per call
(a texture base), a sign-extended imm8, an instruction whose modrm and whose
immediate are both patched, one store that covers the tail of an
immediate *and* the opcode bytes after it, an imm8 shift count and an imm8
rotate count patched per call over every byte value (Build's column loops;
a zero count must leave the carry alone), imul's imm32, and a 16-bit rcr
whose count is reduced modulo 17 and so keeps its constant, and adc / sbb
imm8 fields patched between 0 and other values (a block translated while
one is 0 must not fold it into "add the carry"). It prints a
checksum of what the patched code computed. Boots it on the FreeDOS test floppy (fetched by
tools/x87-guest-test.py on first use) under all four combinations of
`-accel tcg,smc-same-value=on|off,soft-imm=on|off`; every checksum must equal
the architectural result. The soft-imm runs also have to *reach* the path:
the run asserts QEMU traced blocks translated that way and writes absorbed by
them, so a battery that stopped exercising it fails instead of passing.

    tools/smc-guest-test.py            # needs nasm, mtools, build/qemu

Outputs in build/smc-guest/.
"""
import collections
import importlib.util
import os
import re
import shutil
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "build/smc-guest")

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

def _shr_cf(c):
    """shr eax, c of 0F0F0F0F0h after stc, then adc eax, 0."""
    c &= 31
    v = 0xF0F0F0F0
    if c == 0:
        return (v + 1) & M          # a zero count leaves the carry alone
    return ((v >> c) + ((v >> (c - 1)) & 1)) & M


def _rol_cf(c):
    """rol eax, c of 12345678h after clc, then adc eax, 0."""
    c &= 31
    v = 0x12345678
    if c == 0:
        return v
    r = ((v << c) | (v >> (32 - c))) & M
    return (r + (r & 1)) & M


def _rcr16(c):
    """rcr ax, c of 1234h with the carry set: 17 bits, modulo 17."""
    c = (c & 31) % 17
    v = (1 << 16) | 0x1234          # CF:ax
    for _ in range(c):
        v = (v >> 1) | ((v & 1) << 16)
    return v & 0xffff


def _st(i):
    """The imm8 cases S and T patch in: 0 on even calls, so the soft
    translation (after four invalidating writes, call 4) sees a 0."""
    return 0 if i % 2 == 0 else i & 0x7f


N = 1000
M = 0xffffffff
EXPECTED = {
    "A": sum(range(N)) & M,                                    # imm32 patched from another block
    "B": (N * 0x12345678) & M,                                 # the same imm32 rewritten each time
    "C": sum(range(N)) & M,                                    # patched inside the executing block
    "D": (N * 0x77) & M,                                       # same value inside the executing block
    "E": sum(1000 + i if i % 2 == 0 else 1000 - i for i in range(N)) & M,  # opcode add <-> sub
    "F": sum((i & 0xffff) | ((i & 0xff) << 24) for i in range(N)) & M,      # 16-bit + 8-bit partial patches
    "G": (0x2222 + 0x2222 + 0x1111) & M,                       # rep movsd: new body, same body, old body
    "H": sum(range(N)) & M,                                    # imm32 across a page boundary, new values
    "I": (N * 0x5555) & M,                                     # the same, rewritten with the same value
    "J": sum(range(N)) & M,                                    # a memory operand's disp32 patched per call
    "K": sum(((i & 0xff) ^ 0x80) - 0x80 for i in range(N)) & M,  # a sign-extended imm8 patched per call
    "L": sum(i if i % 2 == 0 else -i for i in range(N)) & M,   # modrm and imm32 of one instruction
    "M": sum(((i << 16) + 1) for i in range(N)) & M,           # a store over an immediate's tail and the next opcode
    "N": sum(_shr_cf(i & 0xff) for i in range(N)) & M,         # an imm8 shift count patched per call
    "O": sum(_rol_cf(i & 0xff) for i in range(N)) & M,         # an imm8 rotate count patched per call
    "P": sum(3 * i for i in range(N)) & M,                     # imul's imm32 patched per call
    "Q": sum(_rcr16(i & 0xff) for i in range(N)) & M,          # a 16-bit rcr count (modulo 17: stays a constant)
    "R": sum(2 * i for i in range(N)) & M,                     # one imm32 in two blocks two bytes apart
    "S": sum(1000 + _st(i) + 1 for i in range(N)) & M,         # adc's imm8, 0 on every other call
    "T": sum(1000 - _st(i) - 1 for i in range(N)) & M,         # sbb's imm8, the same
}
# the cases whose patched field has to be *absorbed* in the soft-imm runs, not
# merely computed right: the program prints each field's address ("@N addr")
ABSORBED = "NOPRST"

ASM = r"""
org 100h
bits 16
%define N 1000

start:
    push cs
    pop ds
    push cs
    pop es
    ; hbase: a routine placed so that its imm32 (at +2) straddles a page boundary
    mov ax, cs
    movzx eax, ax
    shl eax, 4
    add eax, area + 16
    and eax, 4095
    neg eax
    and eax, 4095
    add ax, area + 16
    sub ax, 4
    mov [hbase], ax

    ; A: patch the imm32 of routA from another block, values 0..N-1
    xor ebx, ebx
    xor edi, edi
.la:
    mov [routA + 2], ebx
    call routA
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .la
    mov al, 'A'
    call report

    ; B: the same value written before every call
    mov dword [routA + 2], 12345678h
    xor ebx, ebx
    xor edi, edi
.lb:
    mov dword [routA + 2], 12345678h
    call routA
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .lb
    mov al, 'B'
    call report

    ; C: the next instruction of the executing block patched with new values
    xor ebx, ebx
    xor edi, edi
.lc:
    mov [.cins + 2], ebx
.cins:
    mov eax, 0
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .lc
    mov al, 'C'
    call report

    ; D: the same, with the value already there
    mov dword [.dins + 2], 77h
    xor ebx, ebx
    xor edi, edi
.ld:
    mov dword [.dins + 2], 77h
.dins:
    mov eax, 0
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .ld
    mov al, 'D'
    call report

    ; E: the opcode of routE alternates between add eax,ebx (01 D8) and sub (29 D8)
    xor ecx, ecx
    xor edi, edi
.le:
    mov al, 01h
    test ecx, 1
    jz .e1
    mov al, 29h
.e1:
    mov [routE + 1], al
    mov eax, 1000
    mov ebx, ecx
    call routE
    add edi, eax
    inc ecx
    cmp ecx, N
    jb .le
    mov al, 'E'
    call report

    ; F: a 16-bit store into the low half of routA's imm32 and a byte into its top
    mov dword [routA + 2], 0
    xor ecx, ecx
    xor edi, edi
.lf:
    mov [routA + 2], cx
    mov [routA + 5], cl
    call routA
    add edi, eax
    inc ecx
    cmp ecx, N
    jb .lf
    mov al, 'F'
    call report

    ; G: routG overwritten by rep movsd: a new body, the same body again, the old one
    xor ebp, ebp
    cld
    mov si, tmplG2
    mov di, routG
    mov cx, 2
    rep movsd
    call routG
    add ebp, eax
    mov si, tmplG2
    mov di, routG
    mov cx, 2
    rep movsd
    call routG
    add ebp, eax
    mov si, tmplG1
    mov di, routG
    mov cx, 2
    rep movsd
    call routG
    add ebp, eax
    mov edi, ebp
    mov al, 'G'
    call report

    ; H: the routine at hbase (mov eax, imm32; ret) has its imm32 across a page
    ; boundary; one 4-byte store patches it with new values
    mov si, tmplH
    mov di, [hbase]
    mov cx, 7
    rep movsb
    xor ebx, ebx
    xor edi, edi
.lh:
    mov si, [hbase]
    add si, 2
    mov [si], ebx
    call word [hbase]
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .lh
    mov al, 'H'
    call report

    ; I: the same crossing store with the value already there
    mov si, [hbase]
    add si, 2
    mov dword [si], 5555h
    xor ebx, ebx
    xor edi, edi
.li:
    mov si, [hbase]
    add si, 2
    mov dword [si], 5555h
    call word [hbase]
    add edi, eax
    inc ebx
    cmp ebx, N
    jb .li
    mov al, 'I'
    call report

    ; J: the disp32 of a memory operand rewritten before every call, the way a
    ; span loop rewrites its texture base
    xor eax, eax
    xor bx, bx
.jfill:                         ; area[i] = i
    mov [area + bx], eax
    add bx, 4
    inc eax
    cmp eax, N
    jb .jfill
    xor si, si
    xor edi, edi
.lj:
    movzx eax, si
    shl eax, 2
    add eax, area
    mov [routJ_disp], eax
    call routJ
    add edi, eax
    inc si
    cmp si, N
    jb .lj
    mov al, 'J'
    call report

    ; K: a sign-extended imm8 rewritten before every call
    xor si, si
    xor edi, edi
.lk:
    mov ax, si
    mov [routK_imm], al
    call routK
    add edi, eax
    inc si
    cmp si, N
    jb .lk
    mov al, 'K'
    call report

    ; L: one instruction whose modrm (add <-> sub) and whose imm32 are both
    ; patched: the immediate may be read at run time, the modrm may not
    xor si, si
    xor edi, edi
.ll:
    mov al, 0C0h                ; /0 = add eax, imm32
    test si, 1
    jz .l1
    mov al, 0E8h                ; /5 = sub eax, imm32
.l1:
    mov [routL_modrm], al
    movzx eax, si
    mov [routL_imm], eax
    call routL
    add edi, eax
    inc si
    cmp si, N
    jb .ll
    mov al, 'L'
    call report

    ; M: one store over the tail of an immediate *and* the two opcode bytes
    ; after it (rewritten with what is already there): a block cannot survive
    ; that, however soft its immediate is
    mov dword [routM_imm], 0
    xor si, si
    xor edi, edi
.lm:
    movzx eax, si
    or eax, 83660000h           ; bytes +6,+7 back as they were: 66 83
    mov [routM_imm + 2], eax
    call routM
    add edi, eax
    inc si
    cmp si, N
    jb .lm
    mov al, 'M'
    call report

    ; N: an imm8 shift count rewritten before every call, the way Build's
    ; column loops rewrite theirs per texture: every byte value, so the
    ; processor's mask is exercised, and zero counts, which must leave the
    ; carry the routine set (it is folded into the result)
    xor si, si
    xor edi, edi
.ln:
    mov ax, si
    mov [routN_imm], al
    call routN
    add edi, eax
    inc si
    cmp si, N
    jb .ln
    mov al, 'N'
    call report

    ; O: the same with a rotate
    xor si, si
    xor edi, edi
.lo:
    mov ax, si
    mov [routO_imm], al
    call routO
    add edi, eax
    inc si
    cmp si, N
    jb .lo
    mov al, 'O'
    call report

    ; P: imul's imm32 rewritten before every call
    xor si, si
    xor edi, edi
.lp:
    movzx eax, si
    mov [routP_imm], eax
    call routP
    add edi, eax
    inc si
    cmp si, N
    jb .lp
    mov al, 'P'
    call report

    ; Q: a 16-bit rcr's count rewritten before every call: reduced modulo 17
    ; at translation, so it keeps its constant and has to retranslate right
    xor si, si
    xor edi, edi
.lq:
    mov ax, si
    mov [routQ_imm], al
    call routQ
    add edi, eax
    inc si
    cmp si, N
    jb .lq
    mov al, 'Q'
    call report

    ; R: one imm32 inside two blocks whose starts are two bytes apart -- a loop
    ; entered at its top and looping back past its first instruction, the
    ; shape of Blood's span loop. Both blocks must go soft for the patch to be
    ; absorbed; with the invalidation counters hashed by pc >> 2 the two
    ; shared a slot and reset each other on every patch
    xor si, si
    xor edi, edi
.lr:
    movzx eax, si
    mov [routR_imm], eax
    call routR
    add edi, eax
    call routR2
    add edi, eax
    inc si
    cmp si, N
    jb .lr
    mov al, 'R'
    call report

    ; S: adc eax, imm8 rewritten before every call, 0 on even calls: a block
    ; translated while it is 0 must still add what is patched in later, not
    ; only the carry (11.1's ADC shortcut for an immediate 0)
    xor si, si
    xor edi, edi
.ls:
    call st_value
    mov [routS_imm], al
    call routS
    add edi, eax
    inc si
    cmp si, N
    jb .ls
    mov al, 'S'
    call report

    ; T: the same with sbb
    xor si, si
    xor edi, edi
.lt:
    call st_value
    mov [routT_imm], al
    call routT
    add edi, eax
    inc si
    cmp si, N
    jb .lt
    mov al, 'T'
    call report

    ; where the fields the soft-imm runs must absorb are
    mov al, 'N'
    mov bx, routN_imm
    call report_addr
    mov al, 'O'
    mov bx, routO_imm
    call report_addr
    mov al, 'P'
    mov bx, routP_imm
    call report_addr
    mov al, 'R'
    mov bx, routR_imm
    call report_addr
    mov al, 'S'
    mov bx, routS_imm
    call report_addr
    mov al, 'T'
    mov bx, routT_imm
    call report_addr

    mov si, str_done
    call puts
    int 20h

report:                         ; al = case letter, edi = checksum
    call putc
    mov al, ' '
    call putc
    mov eax, edi
    call put_hex32
    mov al, 10
    call putc
    ret

routA:
    mov eax, 0
    ret
routE:
    add eax, ebx
    ret
align 4
routG:                          ; 66 B8 imm32 C3 90: 8 bytes
    mov eax, 1111h
    ret
    nop
tmplG1:
    mov eax, 1111h
    ret
    nop
tmplG2:
    mov eax, 2222h
    ret
    nop
tmplH:                          ; mov eax, imm32; ret — 7 bytes, the imm32 at +2
    db 66h, 0B8h
    dd 0
    db 0C3h

routJ:                          ; a32 mov eax, [disp32]; ret
    db 66h, 67h, 8Bh, 05h
routJ_disp:
    dd 0
    ret
routK:                          ; add eax, imm8 (sign-extended); ret
    xor eax, eax
    db 66h, 83h, 0C0h
routK_imm:
    db 0
    ret
routL:                          ; add/sub eax, imm32; ret
    xor eax, eax
    db 66h, 81h
routL_modrm:
    db 0C0h
routL_imm:
    dd 0
    ret
routM:                          ; mov eax, imm32; add eax, 1; ret
    db 66h, 0B8h
routM_imm:
    dd 0
    db 66h, 83h, 0C0h, 01h
    ret
routN:                          ; shr 0F0F0F0F0h by imm8 after stc; + CF
    mov eax, 0F0F0F0F0h
    stc
    db 66h, 0C1h, 0E8h          ; shr eax, imm8
routN_imm:
    db 0
    adc eax, 0
    ret
routO:                          ; rol 12345678h by imm8 after clc; + CF
    mov eax, 12345678h
    clc
    db 66h, 0C1h, 0C0h          ; rol eax, imm8
routO_imm:
    db 0
    adc eax, 0
    ret
routP:                          ; 3 * imm32
    mov ebx, 3
    db 66h, 69h, 0C3h           ; imul eax, ebx, imm32
routP_imm:
    dd 0
    ret
routQ:                          ; rcr 1234h by imm8 through a set carry, 16-bit
    mov ax, 1234h
    stc
    db 0C1h, 0D8h               ; rcr ax, imm8
routQ_imm:
    db 0
    movzx eax, ax
    ret
align 4
routR:                          ; xor bx, bx (2 bytes); then the second entry
    db 31h, 0DBh
routR2:                         ; mov eax, imm32; ret
    db 66h, 0B8h
routR_imm:
    dd 0
    ret

routS:                          ; 1000 + imm8 + CF(1)
    mov eax, 1000
    stc
    db 66h, 83h, 0D0h           ; adc eax, imm8
routS_imm:
    db 0
    ret
routT:                          ; 1000 - imm8 - CF(1)
    mov eax, 1000
    stc
    db 66h, 83h, 0D8h           ; sbb eax, imm8
routT_imm:
    db 0
    ret
st_value:                       ; al = 0 if si is even, else si & 7Fh
    xor al, al
    test si, 1
    jz .z
    mov ax, si
    and al, 7Fh
.z:
    ret

report_addr:                    ; al = case letter, bx = a field's offset in CS
    push ax
    mov al, '@'
    call putc
    pop ax
    call putc
    mov al, ' '
    call putc
    xor eax, eax
    mov ax, cs
    shl eax, 4
    movzx ebx, bx
    add eax, ebx
    call put_hex32
    mov al, 10
    call putc
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

str_done: db "DONE", 10, 0
hbase: dw 0
times (1000h - ($ - $$)) db 0
area: times 8192 db 0
"""


def run_qemu(opts, img, log, trace=None, events="soft_imm_block,soft_imm_absorb"):
    p = x87gt.subprocess.Popen([
        x87gt.QEMU, "-machine", "pc", *x87gt.tcg_opts(*opts),
        "-cpu", "pentium3", "-m", "64",
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        "-fda", img, "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
        *(["-d", ",".join("trace:" + e for e in events.split(",")), "-D", trace] if trace else []),
    ])
    t0 = x87gt.time.time()
    try:
        while x87gt.time.time() - t0 < 600:
            x87gt.time.sleep(0.5)
            if p.poll() is not None:
                break
            if os.path.exists(log) and b"DONE" in open(log, "rb").read()[-16:]:
                break
        else:
            raise SystemExit("timeout waiting for DONE (%s)" % ",".join(opts))
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    return open(log, "rb").read().decode("ascii", "replace").split("\n")


def main():
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    asm = os.path.join(OUT, "smctest.asm")
    com = os.path.join(OUT, "SMCTEST.COM")
    with open(asm, "w") as f:
        f.write(ASM)
    x87gt.sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    img = os.path.join(OUT, "smctest.img")
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nSMCTEST.COM\r\n")
    x87gt.sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    x87gt.sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    x87gt.sh("mcopy", "-o", "-i", img, com, "::SMCTEST.COM")

    bad = 0
    # every combination of the two switches: each is an oracle for the other
    for same in ("on", "off"):
        for soft in ("on", "off"):
            name = "same-%s-soft-%s" % (same, soft)
            log = os.path.join(OUT, "serial-%s.log" % name)
            trace = os.path.join(OUT, "trace-%s.log" % name) if soft == "on" else None
            for f in (log, trace):
                if f and os.path.exists(f):
                    os.unlink(f)
            lines = run_qemu(["smc-same-value=" + same, "soft-imm=" + soft],
                             img, log, trace)
            got, where = {}, {}
            for l in lines:
                parts = l.split()
                if len(parts) == 2 and parts[0] in EXPECTED and len(parts[1]) == 8:
                    got[parts[0]] = int(parts[1], 16)
                elif len(parts) == 2 and parts[0][:1] == "@" and len(parts[1]) == 8:
                    where[parts[0][1:]] = int(parts[1], 16)
            for k in sorted(EXPECTED):
                if k not in got:
                    print("FAIL: %s case %s missing" % (name, k))
                    bad += 1
                elif got[k] != EXPECTED[k]:
                    print("FAIL: %s case %s: got %08x expected %08x"
                          % (name, k, got[k], EXPECTED[k]))
                    bad += 1
            right = sum(1 for k in EXPECTED if got.get(k) == EXPECTED[k])
            note = ""
            if trace:
                # the path has to be reached, or this battery passes by not testing it
                text = open(trace).read() if os.path.exists(trace) else ""
                blocks = text.count("soft_imm_block")
                absorbed = text.count("soft_imm_absorb")
                note = "  (%d blocks read their operands at run time, %d writes absorbed)" % (
                    blocks, absorbed)
                if blocks == 0 or absorbed == 0:
                    print("FAIL: %s never reached the soft-imm path" % name)
                    bad += 1
                # and the fields these cases patch must be among them: a case
                # can compute the right sum while its block retranslates on
                # every patch, which is the thing the feature exists to stop
                at = collections.Counter(
                    int(g, 16) for g in re.findall(r"soft_imm_absorb addr:0x([0-9a-f]+)", text))
                for k in ABSORBED:
                    n = at.get(where.get(k, -1), 0)
                    if n < N // 2:
                        print("FAIL: %s case %s: its field was absorbed %d times, "
                              "wanted at least %d" % (name, k, n, N // 2))
                        bad += 1
            print("%s: %d/%d cases right%s" % (name, right, len(EXPECTED), note))
    # The same battery in a code buffer small enough to fill over and over
    # (patch 85). With one vCPU QEMU empties a full buffer in the middle of
    # translating, and the block that ran before the flush was chained to
    # the new one from freed memory; the next invalidation of that block
    # walked into garbage. A crash or a wrong sum fails, and so does a run
    # that never flushed, since then it tested nothing. It did not crash the
    # build without patch 85 (314 flushes), so it is coverage of the path,
    # not a reproducer: that is Duke Nukem 3D in a Win98 DOS box
    # (tools/win98-game-test.sh), which needs the user's disc.
    name = "tiny-buffer"
    log = os.path.join(OUT, "serial-%s.log" % name)
    trace = os.path.join(OUT, "trace-%s.log" % name)
    for f in (log, trace):
        if os.path.exists(f):
            os.unlink(f)
    lines = run_qemu(["smc-same-value=off", "soft-imm=off", "tb-size=1"], img, log, trace, "tb_flush")
    got = {}
    for l in lines:
        parts = l.split()
        if len(parts) == 2 and parts[0] in EXPECTED and len(parts[1]) == 8:
            got[parts[0]] = int(parts[1], 16)
    right = sum(1 for k in EXPECTED if got.get(k) == EXPECTED[k])
    flushes = open(trace).read().count("tb_flush") if os.path.exists(trace) else 0
    if right != len(EXPECTED):
        print("FAIL: %s: %d/%d cases right (QEMU crashed or computed wrong)"
              % (name, right, len(EXPECTED)))
        bad += len(EXPECTED) - right
    if flushes == 0:
        print("FAIL: %s never flushed the code buffer" % name)
        bad += 1
    print("%s: %d/%d cases right, %d code buffer flushes" % (name, right, len(EXPECTED), flushes))
    if bad:
        print("FAIL: %d mismatches" % bad)
        return 1
    print("PASS: %d cases, every smc-same-value / soft-imm combination "
          "== the architecture" % len(EXPECTED))
    return 0


if __name__ == "__main__":
    sys.exit(main())
