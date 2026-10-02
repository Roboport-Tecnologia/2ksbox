#!/usr/bin/env python3
"""In-guest regression test for the x87 fast path (patch 05).

Builds a DOS program (NASM, .COM) that runs a battery of x87 instructions
over a pool of edge-case operands under several control words (each of
them twice, the second time with the inexact flag set before every case,
which is the state the inline path translates differently, patches 37 and
45, and which fninit before each case would otherwise never let it see),
and streams every result (80-bit value + status word) to the serial port.
Boots it on the FreeDOS test floppy under our qemu-system-i386 twice, with
`-cpu pentium3,x87-fast=on` and `=off`, and requires the two serial logs to
be identical: the fast path must be indistinguishable from softfloat.

    tools/x87-guest-test.py            # needs nasm, mtools, build/qemu; fetches the FreeDOS floppy

Prerequisites: `brew install nasm mtools` (macOS) or `pacman -S nasm mtools`
(Arch). The FreeDOS 1.3 boot floppy (build/images/144m/x86BOOT.img,
git-ignored) is downloaded from ibiblio on first use.

The host-side oracle (tools/x87-fast-test.c) proves the fast path matches a
real x87; this proves the helper glue in fpu_helper.c preserves values and
flags end to end under TCG.
"""
import os
import shutil
import struct
import subprocess
import sys
import qemuhost  # tools/qemuhost.py: the platform's QEMU and QMP address
import time
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = qemuhost.qemu(ROOT)   # this checkout's build, or $QEMU_BIN
FLOPPY = os.path.join(ROOT, "build/images/144m/x86BOOT.img")
FLOPPY_ZIP = os.path.join(ROOT, "build/images/FD13-FloppyEdition.zip")
FLOPPY_URL = ("https://www.ibiblio.org/pub/micro/pc-stuff/freedos/files/"
              "distributions/1.3/official/FD13-FloppyEdition.zip")
OUT = os.path.join(ROOT, "build/x87-guest")


def ensure_prereqs():
    missing = [t for t in ("nasm", "mcopy") if shutil.which(t) is None]
    if missing:
        raise SystemExit("missing %s: brew install nasm mtools (macOS) / "
                         "pacman -S nasm mtools (Arch)" % ", ".join(missing))
    if not os.path.exists(QEMU):
        raise SystemExit("%s not built (scripts/prepare-qemu.sh && "
                         "scripts/configure-qemu.sh && ninja -C build/qemu "
                         "qemu-system-i386)" % QEMU)


def ensure_floppy():
    """FreeDOS 1.3 floppy edition boot disk, fetched from ibiblio once."""
    if os.path.exists(FLOPPY):
        return
    os.makedirs(os.path.dirname(FLOPPY_ZIP), exist_ok=True)
    if not os.path.exists(FLOPPY_ZIP):
        print("downloading %s (21 MB)" % FLOPPY_URL)
        urllib.request.urlretrieve(FLOPPY_URL, FLOPPY_ZIP + ".part")
        os.rename(FLOPPY_ZIP + ".part", FLOPPY_ZIP)
    with zipfile.ZipFile(FLOPPY_ZIP) as z:
        z.extract("144m/x86BOOT.img", os.path.dirname(FLOPPY_ZIP))
    print("extracted", FLOPPY)

# ---------------------------------------------------------------- pool

def d2x80(d):
    """IEEE double -> 80-bit (low, high) exactly (normal/zero/inf/nan; denormals normalized)."""
    bits = struct.unpack("<Q", struct.pack("<d", d))[0]
    sign = (bits >> 63) << 15
    e = (bits >> 52) & 0x7FF
    frac = bits & ((1 << 52) - 1)
    if e == 0:
        if frac == 0:
            return 0, sign
        # denormal: normalize
        shift = 0
        while not (frac >> 52) & 1:
            frac <<= 1
            shift += 1
        return (frac << 11) & ((1 << 64) - 1), sign | (16383 - 1022 - shift)
    if e == 0x7FF:
        return (1 << 63) | (frac << 11), sign | 0x7FFF
    return (1 << 63) | (frac << 11), sign | (e - 1023 + 16383)


def raw(low, high):
    return low, high


def pool_entries():
    p = []
    p += [raw(0, 0), raw(0, 0x8000)]                                   # ±0
    p += [d2x80(1.0), d2x80(-1.0), d2x80(0.5), d2x80(1.5), d2x80(3.0)]
    p += [d2x80(1234567.0), d2x80(-42.25), d2x80(1e-5), d2x80(0.1), d2x80(2.5)]
    p += [d2x80(1.0000000000000002), d2x80(0.3), d2x80(-0.7), d2x80(1e10)]
    p += [d2x80(2.0 ** 900), d2x80(2.0 ** 901), d2x80(2.0 ** -900), d2x80(2.0 ** -901)]
    p += [d2x80(2.2250738585072014e-308), d2x80(1.7976931348623157e308)]  # DBL_MIN/MAX
    p += [d2x80(5e-324)]                                               # subnormal double
    p += [raw(0x8000000000000001, 0x3FFF), raw(0xFFFFFFFFFFFFFFFF, 0x3FFF)]  # 64-bit mantissas
    p += [raw(0xC90FDAA22168C235, 0x4000)]                             # pi (64-bit)
    p += [raw(0x8000000000000000, 0x7FFF), raw(0x8000000000000000, 0xFFFF)]  # ±inf
    p += [raw(0xC000000000000000, 0x7FFF), raw(0xA000000000000000, 0x7FFF)]  # qnan, snan
    p += [raw(0x0000000000000001, 0x0000), raw(0x8000000000000000, 0x0000)]  # denormal, pseudo-denormal
    p += [raw(0xB400000000000000, 0x3FFF), raw(0xB4000000000000FF, 0x3FFF)]  # float-exact, not
    # PC=64 (patch 48's integer arithmetic): 1 + 2^-64 and (1 + 2^-63) + 2^-64
    # are ties at 64 bits, 1.5 * 2^-64 is past one, 1 + 3 * 2^-63 cancels
    # against 1 to 3 * 2^-63, and two arbitrary full mantissas for the
    # carries and quotients
    p += [raw(0x8000000000000000, 0x3FBF), raw(0xC000000000000000, 0x3FBF)]
    p += [raw(0x8000000000000003, 0x3FFF)]
    p += [raw(0x9E3779B97F4A7C15, 0x4005), raw(0xD1B54A32D192ED03, 0x3FF0)]
    # a few float32-exact values with float-range exponents
    for f in (1.0 / 3.0, 7.0e-20, 3.0e20, -6.5e-3):
        bits = struct.unpack("<I", struct.pack("<f", f))[0]
        p.append(d2x80(struct.unpack("<f", struct.pack("<I", bits))[0]))
    return p


# ---------------------------------------------------------------- asm

ASM = r"""
org 100h
bits 16

%define POOL_N {pool_n}
%define NUM_BINOPS 37
%define NUM_OPS 68

start:
    mov si, cw_table
.cw_loop:
    mov ax, [si]
    cmp ax, 0FFFFh
    je .done
    mov [cur_cw], ax
    add si, 2
    push si
    push ax
    mov si, str_cw
    call puts
    pop ax
    call put_hex16
    call newline
    xor bx, bx
.i_loop:
    xor bp, bp
.j_loop:
    mov cl, 0
.op_loop:
    call do_op
    call print_result
    inc cl
    cmp cl, NUM_BINOPS
    jb .op_loop
    add bp, 10
    cmp bp, POOL_N * 10
    jb .j_loop
    mov cl, NUM_BINOPS
.un_loop:
    call do_op
    call print_result
    inc cl
    cmp cl, NUM_OPS
    jb .un_loop
    add bx, 10
    cmp bx, POOL_N * 10
    jb .i_loop
    pop si
    jmp .cw_loop
.done:
    mov si, str_done
    call puts
    int 20h

; bx = a offset, bp = b offset, cl = op. Result in res (10 bytes) + res_sw.
; Bit 15 of the control word entry (not a control word bit) asks for the
; inexact flag to be set before the case: the inline path translates a
; block whose PE is already sticky differently (patch 37 drops the
; bookkeeping, patch 45 computes in binary32 at PC=24), and fninit before
; every case would otherwise never let that variant run here.
do_op:
    push cx
    fninit
    mov ax, [cur_cw]
    and ax, 7FFFh
    mov [cw_load], ax
    fldcw [cw_load]
    test byte [cur_cw + 1], 80h
    jz .pe_done
    fld1
    fdiv dword [c_three]        ; 1/3: inexact, PE set
    fstp st0
.pe_done:
    xor ax, ax
    mov [res], ax
    mov [res+2], ax
    mov [res+4], ax
    mov [res+6], ax
    mov [res+8], ax
    mov [res_sw], ax
    mov ch, 0
    mov si, cx
    shl si, 1
    jmp [op_table + si]

; ---- binary ops (a = ST0, b = ST1 after the two loads) ----
op_faddp:   fld tword [bp + pool]
            fld tword [bx + pool]
            faddp st1, st0
            jmp store80
op_fsubp:   fld tword [bp + pool]
            fld tword [bx + pool]
            fsubp st1, st0
            jmp store80
op_fsubrp:  fld tword [bp + pool]
            fld tword [bx + pool]
            fsubrp st1, st0
            jmp store80
op_fmulp:   fld tword [bp + pool]
            fld tword [bx + pool]
            fmulp st1, st0
            jmp store80
op_fdivp:   fld tword [bp + pool]
            fld tword [bx + pool]
            fdivp st1, st0
            jmp store80
op_fdivrp:  fld tword [bp + pool]
            fld tword [bx + pool]
            fdivrp st1, st0
            jmp store80
op_fadd_st: fld tword [bp + pool]
            fld tword [bx + pool]
            fadd st0, st1
            jmp store80
op_fmul_st: fld tword [bp + pool]
            fld tword [bx + pool]
            fmul st0, st1
            jmp store80
op_fadd_m:  fld tword [bx + pool]
            fadd qword [bp + pool]          ; b's low 8 bytes as a double
            jmp store80
op_fmul_ms: fld tword [bx + pool]
            fmul dword [bp + pool]          ; b's low 4 bytes as a float
            jmp store80
op_fdiv_m:  fld tword [bx + pool]
            fdiv qword [bp + pool]
            jmp store80
op_fcompp:  fld tword [bp + pool]
            fld tword [bx + pool]
            fcompp
            jmp store_sw
op_fucompp: fld tword [bp + pool]
            fld tword [bx + pool]
            fucompp
            jmp store_sw
op_fcomip:  fld tword [bp + pool]
            fld tword [bx + pool]
            fcomip st1
            pushf
            pop word [res]
            fstp st0
            jmp store_sw
; ---- more binary forms (inline TCG fast path coverage) ----
op_fsub_m:  fld tword [bx + pool]
            fsub qword [bp + pool]
            jmp store80
op_fsubr_m: fld tword [bx + pool]
            fsubr qword [bp + pool]
            jmp store80
op_fmul_m:  fld tword [bx + pool]
            fmul qword [bp + pool]
            jmp store80
op_fdivr_m: fld tword [bx + pool]
            fdivr qword [bp + pool]
            jmp store80
op_fsub_st: fld tword [bp + pool]
            fld tword [bx + pool]
            fsub st0, st1
            jmp store80
op_fsubr_st: fld tword [bp + pool]
            fld tword [bx + pool]
            fsubr st0, st1
            jmp store80
op_fdiv_st: fld tword [bp + pool]
            fld tword [bx + pool]
            fdiv st0, st1
            jmp store80
op_fdivr_st: fld tword [bp + pool]
            fld tword [bx + pool]
            fdivr st0, st1
            jmp store80
op_fadd_stn: fld tword [bp + pool]
            fld tword [bx + pool]
            fadd st1, st0
            fstp st0
            jmp store80
op_fsub_stn: fld tword [bp + pool]
            fld tword [bx + pool]
            fsub st1, st0
            fstp st0
            jmp store80
op_fsubr_stn: fld tword [bp + pool]
            fld tword [bx + pool]
            fsubr st1, st0
            fstp st0
            jmp store80
op_fmul_stn: fld tword [bp + pool]
            fld tword [bx + pool]
            fmul st1, st0
            fstp st0
            jmp store80
op_fdiv_stn: fld tword [bp + pool]
            fld tword [bx + pool]
            fdiv st1, st0
            fstp st0
            jmp store80
op_fdivr_stn: fld tword [bp + pool]
            fld tword [bx + pool]
            fdivr st1, st0
            fstp st0
            jmp store80
op_fxch:    fld tword [bp + pool]
            fld tword [bx + pool]
            fxch st1
            fadd st0, st1
            jmp store80
; ---- chains inside one block: shadows stay dirty across instructions ----
op_chain1:  fld qword [bp + pool]           ; low 8 bytes as doubles
            fld qword [bx + pool]
            fadd st1, st0
            fxch st1
            fmulp st1, st0
            jmp store80
op_chain2:  fld qword [bx + pool]
            fld st0
            fmul st0, st1
            fstp qword [res]
            fld qword [res]
            fsubrp st1, st0
            fadd qword [bp + pool]
            jmp store80
op_chain3:  fld qword [bp + pool]
            fld qword [bx + pool]
            fdiv st0, st1
            fst st1
            fchs
            fabs
            fsqrt
            faddp st1, st0
            jmp store80
op_fcmove:  fld tword [bp + pool]
            fld tword [bx + pool]
            xor ax, ax                      ; ZF = 1
            fcmove st0, st1
            fstp st1
            jmp store80
op_fcmovnb: fld tword [bp + pool]
            fld tword [bx + pool]
            cmp ax, 1                       ; CF = 1 -> not taken
            fcmovnb st0, st1
            fstp st1
            jmp store80
op_fucom:   fld tword [bp + pool]
            fld tword [bx + pool]
            fucom st1
            fnstsw ax
            mov [res], ax
            jmp store_sw
op_fucomp:  fld tword [bp + pool]
            fld tword [bx + pool]
            fucomp st1
            fnstsw ax
            mov [res], ax
            jmp store_sw
op_fcom_m:  fld tword [bx + pool]
            fcom qword [bp + pool]
            fnstsw ax
            mov [res], ax
            jmp store_sw
; ---- unary ops on a ----
op_fsqrt:   fld tword [bx + pool]
            fsqrt
            jmp store80
op_frndint: fld tword [bx + pool]
            frndint
            jmp store80
op_fistw:   fld tword [bx + pool]
            fistp word [res]
            jmp store_sw
op_fistl:   fld tword [bx + pool]
            fistp dword [res]
            jmp store_sw
op_fistq:   fld tword [bx + pool]
            fistp qword [res]
            jmp store_sw
op_fstl:    fld tword [bx + pool]
            fstp qword [res]
            jmp store_sw
op_fsts:    fld tword [bx + pool]
            fstp dword [res]
            jmp store_sw
op_fildw:   fild word [bx + pool]
            jmp store80
op_fildl:   fild dword [bx + pool]
            jmp store80
op_fildq:   fild qword [bx + pool]
            jmp store80
op_flds:    fld dword [bx + pool]
            jmp store80
op_fldl:    fld qword [bx + pool]
            jmp store80
op_fchs:    fld tword [bx + pool]
            fchs
            jmp store80
op_fsin:    fld tword [bx + pool]
            fsin
            jmp store80
op_fptan:   fld tword [bx + pool]
            fptan
            fstp st0
            jmp store80
op_fscale:  fld tword [bp + pool]
            fld tword [bx + pool]
            fscale
            jmp store80
op_fld_st:  fld tword [bx + pool]
            fld1
            fld st1
            fmulp st1, st0
            fstp st0
            jmp store80
op_fst_st:  fld tword [bx + pool]
            fld1
            fst st1
            fmulp st1, st0
            jmp store80
op_fst_m:   fld tword [bx + pool]
            fst qword [res]
            fstp st0
            jmp store_sw
op_fsts_m:  fld tword [bx + pool]
            fst dword [res]
            fstp st0
            jmp store_sw
op_ftst:    fld tword [bx + pool]
            ftst
            fnstsw ax
            mov [res], ax
            jmp store_sw
op_fabs:    fld tword [bx + pool]
            fabs
            jmp store80
op_fldz:    fld tword [bx + pool]
            fldz
            faddp st1, st0
            jmp store80
op_incstp:  fld tword [bx + pool]
            fld1
            fincstp
            fincstp
            fdecstp
            fnstsw ax
            mov [res], ax
            fstp st0
            fstp st0
            jmp store_sw
op_ffree:   fld tword [bx + pool]
            fld1
            ffree st1
            fnstsw ax
            mov [res], ax
            fstp st0
            fstp st0
            jmp store_sw
op_fist_ch: fld qword [bx + pool]
            fld1
            faddp st1, st0
            fistp dword [res]
            jmp store_sw
op_fild_ch: fild dword [bx + pool]
            fild word [bx + pool + 4]
            fmulp st1, st0
            jmp store80
op_frnd_ch: fld qword [bx + pool]
            frndint
            fld1
            faddp st1, st0
            jmp store80
op_fnstsw:  fld tword [bx + pool]
            fld1
            fnstsw ax
            mov [res], ax
            fstp st0
            fstp st0
            jmp store_sw
op_fstp_st: fld tword [bx + pool]
            fld1
            fstp st1
            jmp store80

store80:
    fstp tword [res]
store_sw:
    fnstsw [res_sw]
    pop cx
    ret

op_table:
    dw op_faddp, op_fsubp, op_fsubrp, op_fmulp, op_fdivp, op_fdivrp
    dw op_fadd_st, op_fmul_st, op_fadd_m, op_fmul_ms, op_fdiv_m
    dw op_fcompp, op_fucompp, op_fcomip
    dw op_fsub_m, op_fsubr_m, op_fmul_m, op_fdivr_m
    dw op_fsub_st, op_fsubr_st, op_fdiv_st, op_fdivr_st
    dw op_fadd_stn, op_fsub_stn, op_fsubr_stn, op_fmul_stn, op_fdiv_stn
    dw op_fdivr_stn, op_fxch
    dw op_chain1, op_chain2, op_chain3, op_fcmove, op_fcmovnb
    dw op_fucom, op_fucomp, op_fcom_m
    dw op_fsqrt, op_frndint, op_fistw, op_fistl, op_fistq, op_fstl, op_fsts
    dw op_fildw, op_fildl, op_fildq, op_flds, op_fldl, op_fchs, op_fsin
    dw op_fptan, op_fscale, op_fld_st, op_fst_st, op_fstp_st, op_fst_m
    dw op_fsts_m, op_ftst, op_fabs, op_fldz, op_incstp, op_ffree
    dw op_fist_ch, op_fild_ch, op_frnd_ch, op_fnstsw, op_fstp_st

; ---- output ----
print_result:
    push cx
    mov cx, 10
    mov si, res + 9
.l: mov al, [si]
    call put_hex8
    dec si
    loop .l
    mov al, ' '
    call putc
    mov ax, [res_sw]
    call put_hex16
    call newline
    pop cx
    ret

putc:                       ; al = char -> COM1 (0x3F8), poll THR empty
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

puts:                       ; si = asciiz
    lodsb
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

str_cw:   db "CW ", 0
str_done: db "DONE", 10, 0

; control words: exceptions masked (0x3F) | PC<<8 | RC<<10
cw_table:
    dw 023Fh        ; PC=53 RNE      (Windows default)
    dw 003Fh        ; PC=24 RNE      (Direct3D)
    dw 033Fh        ; PC=64 RNE
    dw 0E3Fh        ; PC=53 trunc    (MSVC _ftol)
    dw 063Fh        ; PC=53 down
    dw 083Fh        ; PC=24 up
    dw 021Fh        ; PC=53 RNE, PE unmasked (inline path must stay off)
    ; the same with PE already set before each case (bit 15, see do_op)
    dw 823Fh        ; PC=53 RNE, PE sticky
    dw 803Fh        ; PC=24 RNE, PE sticky
    dw 833Fh        ; PC=64 RNE, PE sticky
    dw 8E3Fh        ; PC=53 trunc, PE sticky
    dw 863Fh        ; PC=53 down, PE sticky
    dw 883Fh        ; PC=24 up, PE sticky
    dw 0FFFFh

cur_cw:  dw 0
cw_load: dw 0
c_three: dd 3.0
res:    times 10 db 0
res_sw: dw 0

align 2
pool:
{pool}
"""


# 64 x87 instructions in one basic block (patch 06 keeps the stack as
# shadow doubles across all of them): the block must translate as one TB and
# give the same value and status word with the fast path on and off.
LONGBLK_ASM = r"""
org 100h
bits 16
start:
    fninit
    fldcw [cw]
    mov ecx, 1000
    fld qword [x]
.loop:
%rep 16
    fmul qword [y]
    fadd qword [z]
    fld st0
    fsubp st1, st0
    fadd qword [x]
    fld qword [z]
    fxch st1
    fdivrp st1, st0
%endrep
    dec ecx
    jnz .loop
    fstp qword [w]
    fnstsw [sw]
    mov si, str_long
    call puts
    mov eax, [w + 4]
    call put_hex32
    mov eax, [w]
    call put_hex32
    mov al, ' '
    call putc
    mov ax, [sw]
    call put_hex16
    mov al, 10
    call putc
    int 20h

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

str_long: db "LONG ", 0
cw: dw 027Fh
times (2000h - ($ - $$)) db 0
x:  dq 1.0000001
y:  dq 0.9999999
z:  dq 0.5
w:  dq 0.0
sw: dw 0
"""


BENCH_ASM = r"""
org 100h
bits 16
; x87 throughput: ITER iterations of a typical double-precision inner loop
; (fld/fmul/fadd/fstp on memory doubles) at PC=53 (X87BENCH) or PC=24
; (X87BEN24), and the same on memory floats at PC=24 (X87BEN2S: the shape
; of Direct3D-era game code), timed with the BIOS tick counter (18.2 Hz).
; Prints "BENCH <iterations> <ticks>" (BENCH24 / BENCH24S) on COM1. The loop is one
; block chained to itself, so it runs for ever in the variant it was
; translated for: PE is set before it (below) so that is the sticky one.
%define ITER 20000000
start:
    fninit
    fldcw [cw]
    fld1                        ; 1 / 0.9999999 is inexact: PE set before the
    fdiv qword [y]              ; loop, so its block is translated with PE
    fstp st0                    ; sticky (patch 37), the state a game's loop runs in
    xor ax, ax
    mov es, ax
    mov si, [es:046Ch]
.sync:                          ; align to a tick edge
    mov ax, [es:046Ch]
    cmp ax, si
    je .sync
    mov [t0], ax
    mov ecx, ITER
.loop:
    fld qword [x]
    fmul qword [y]
    fadd qword [z]
    fstp qword [w]
    fld qword [w]
    fdiv qword [y]
    fistp dword [iv]
    dec ecx
    jnz .loop
    mov ax, [es:046Ch]
    sub ax, [t0]
    push ax
    mov si, str_bench
    call puts
    mov eax, ITER
    call put_hex32
    mov al, ' '
    call putc
    pop ax
    call put_hex16
    mov al, 10
    call putc
    int 20h

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

str_bench: db "BENCH ", 0
cw: dw 023Fh
t0: dw 0
; data on its own page: a store into the page holding translated code makes
; QEMU invalidate and retranslate it (self-modifying-code path), which would
; dominate the measurement
times (2000h - ($ - $$)) db 0
x:  dq 1.0000001
y:  dq 0.9999999
z:  dq 0.5
w:  dq 0.0
xs: dd 1.0000001
ys: dd 0.9999999
zs: dd 0.5
ws: dd 0.0
iv: dd 0
"""


def build_asm(entries):
    lines = []
    for low, high in entries:
        b = struct.pack("<QH", low, high)
        lines.append("    db " + ", ".join("0%02xh" % x for x in b))
    return ASM.format(pool_n=len(entries), pool="\n".join(lines))


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def tcg_opts(*own):
    """`-accel tcg,<own>,<QEMU_TCG_OPTS>`: the accelerator properties of a run.
    QEMU_TCG_OPTS=pinned-regs=off runs every DOS battery with the doc 18
    oracle (the register file in env, as upstream); the batteries then
    compare their own on/off pair under it."""
    opts = list(own) + [o for o in os.environ.get("QEMU_TCG_OPTS", "").split(",") if o]
    return ["-accel", ",".join(["tcg"] + opts)] if opts else []


def run_qemu(fast, img, log):
    if os.path.exists(log):
        os.unlink(log)
    p = subprocess.Popen([
        QEMU, "-machine", "pc", *tcg_opts(),
        "-cpu", "pentium3,x87-fast=" + fast, "-m", "64",
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        "-fda", img, "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
    ])
    t0 = time.time()
    try:
        while time.time() - t0 < 600:
            time.sleep(1)
            if p.poll() is not None:
                break
            if os.path.exists(log):
                with open(log, "rb") as f:
                    f.seek(0, 2)
                    n = f.tell()
                    f.seek(max(0, n - 16))
                    if b"DONE" in f.read():
                        break
        else:
            raise SystemExit("timeout waiting for DONE (x87-fast=%s)" % fast)
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    return log


def main():
    ensure_prereqs()
    ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    entries = pool_entries()
    asm = os.path.join(OUT, "x87test.asm")
    com = os.path.join(OUT, "X87TEST.COM")
    with open(asm, "w") as f:
        f.write(build_asm(entries))
    sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    basm = os.path.join(OUT, "x87bench.asm")
    bcom = os.path.join(OUT, "X87BENCH.COM")
    with open(basm, "w") as f:
        f.write(BENCH_ASM)
    sh("nasm", "-O0", "-f", "bin", "-o", bcom, basm)
    b24asm = os.path.join(OUT, "x87ben24.asm")
    b24com = os.path.join(OUT, "X87BEN24.COM")
    with open(b24asm, "w") as f:
        f.write(BENCH_ASM.replace("cw: dw 023Fh", "cw: dw 003Fh")
                .replace('db "BENCH ", 0', 'db "BENCH24 ", 0'))
    sh("nasm", "-O0", "-f", "bin", "-o", b24com, b24asm)
    b24sasm = os.path.join(OUT, "x87ben2s.asm")
    b24scom = os.path.join(OUT, "X87BEN2S.COM")
    with open(b24sasm, "w") as f:
        f.write(open(b24asm).read()
                .replace('db "BENCH24 ", 0', 'db "BENCH24S ", 0')
                .replace("fld qword [x]", "fld dword [xs]")
                .replace("fmul qword [y]", "fmul dword [ys]")
                .replace("fadd qword [z]", "fadd dword [zs]")
                .replace("fstp qword [w]", "fstp dword [ws]")
                .replace("fld qword [w]", "fld dword [ws]")
                .replace("fdiv qword [y]\n    fistp", "fdiv dword [ys]\n    fistp"))
    sh("nasm", "-O0", "-f", "bin", "-o", b24scom, b24sasm)
    lasm = os.path.join(OUT, "longblk.asm")
    lcom = os.path.join(OUT, "LONGBLK.COM")
    with open(lasm, "w") as f:
        f.write(LONGBLK_ASM)
    sh("nasm", "-O0", "-f", "bin", "-o", lcom, lasm)

    img = os.path.join(OUT, "x87test.img")
    shutil.copy(FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nX87BENCH.COM\r\nX87BEN24.COM\r\nX87BEN2S.COM\r\n"
                "LONGBLK.COM\r\n"
                "X87TEST.COM\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::X87TEST.COM")
    sh("mcopy", "-o", "-i", img, bcom, "::X87BENCH.COM")
    sh("mcopy", "-o", "-i", img, b24com, "::X87BEN24.COM")
    sh("mcopy", "-o", "-i", img, b24scom, "::X87BEN2S.COM")
    sh("mcopy", "-o", "-i", img, lcom, "::LONGBLK.COM")

    logs = {}
    for fast in ("off", "on"):
        t0 = time.time()
        logs[fast] = run_qemu(fast, img, os.path.join(OUT, "serial-%s.log" % fast))
        print("x87-fast=%s: %.1f s" % (fast, time.time() - t0))
    a = open(logs["off"], "rb").read()
    b = open(logs["on"], "rb").read()
    ticks = {}
    for fast, data in (("off", a), ("on", b)):
        for line in data.split(b"\n"):
            if line.startswith(b"BENCH"):
                tag, it, tk = line.split()
                ticks[(tag.decode(), fast)] = (int(it, 16), int(tk, 16))
    for tag, what in (("BENCH", "PC=53"), ("BENCH24", "PC=24"),
                      ("BENCH24S", "PC=24, m32")):
        if (tag, "off") in ticks and (tag, "on") in ticks:
            off, on = ticks[(tag, "off")], ticks[(tag, "on")]
            ratio = off[1] / max(1, on[1])
            print("bench %s: %d iterations x 7 x87 ops: off %.2f s, on %.2f s (%.1fx)"
                  % (what, off[0], off[1] / 18.2, on[1] / 18.2, ratio))
            if ratio < 1.5:
                print("WARNING: the fast path does not seem active at %s "
                      "(expected 5x or more): stale build/qemu/qemu-system-i386? "
                      "re-run scripts/prepare-qemu.sh, configure, ninja" % what)
    # the bench lines are timing, not results: compare everything else
    a = b"\n".join(l for l in a.split(b"\n") if not l.startswith(b"BENCH"))
    b = b"\n".join(l for l in b.split(b"\n") if not l.startswith(b"BENCH"))
    n_lines = a.count(b"\n")
    if a == b and b"DONE" in a:
        print("x87 guest test: %d result lines identical with the fast path on and off" % n_lines)
        return 0
    al, bl = a.split(b"\n"), b.split(b"\n")
    diffs = [(i, x, y) for i, (x, y) in enumerate(zip(al, bl)) if x != y]
    print("x87 guest test: MISMATCH, %d differing lines of %d" % (len(diffs), n_lines))
    for i, x, y in diffs[:20]:
        print("  line %d: off=%s on=%s" % (i, x.decode(), y.decode()))
    return 1


if __name__ == "__main__":
    sys.exit(main())
