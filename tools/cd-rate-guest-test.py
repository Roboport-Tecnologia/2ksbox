#!/usr/bin/env python3
"""Does what the guest reads off the CD depend on how fast it asks for it?

The question behind Max Payne's "Corrupt JPEG data" boxes and a Warcraft 3
video that stops in the middle: both are a program streaming a big file off
the disc, and both fail in a way that a wrong byte or a refused sector would
produce. This asks the drive directly, from a DOS program with no operating
system in the way, whether the *bytes* depend on the *rate*.

It boots the FreeDOS test floppy under our qemu-system-i386 with a disc in
the drive and runs a table of passes over the same range of sectors. Each
pass reads that range with one combination of

  * transfer path: PIO (the 0x170 register file, the byte-count-limit loop)
    or **bus-master DMA** (the PIIX3 BMIDE engine found through PCI config
    space). DMA is the path Windows takes, and on a cdimage disc it is our
    own code (`ide_atapi_disc_read_dma_cb`, patch 51), not stock QEMU's,
  * request size: 1, 8 or ~31 sectors per command,
  * byte-count limit (PIO): 2048, 8192, 65534,
  * sector size: 2048 (READ(10)) or 2352 (READ CD, sync + header + EDC/ECC),
  * **rate**: unpaced, or paced to 1x (150 KB/s), 4x, 16x, by a delay after
    every request. The delay loop is calibrated against the BIOS tick at
    boot and each pass reports the ticks it really took, so the achieved
    rate is measured, never assumed.

and folds every byte it receives into a 32-bit rolling checksum
(`sum = rol(sum,1) + word`, little-endian words). Two verdicts come out of
that:

  * **every pass over the same range must produce the same checksum.** A
    difference means the data the guest gets depends on the rate, the
    request size or the transfer path: an emulator bug, and exactly the
    shape that corrupts one JPEG in a hundred.
  * **the 2048-byte passes must equal the host's own checksum** of the same
    range (the cooked image), so "all passes agree" cannot be all passes
    being wrong the same way.

A sector the drive *refuses* is the other half. Any request that fails is
retried one sector at a time, so the failing LBA is named exactly, with its
sense bytes; the good sectors around it still go into the checksum, so a
disc with a bad sector still cross-checks. That is the case to look for
when a video stops in the middle: on a .cue/.ccd/.mds disc the read goes
through libdisc, which **refuses** a data sector whose EDC/ECC does not
verify (`read_cooked` -> Error::Medium -> -EIO), where a real drive
corrects it and hands the bytes over.

    tools/cd-rate-guest-test.py                 # generated discs: iso + cue
    tools/cd-rate-guest-test.py <image>         # a real disc image
    SCAN=1 tools/cd-rate-guest-test.py <image>  # every sector of it, once

By default it builds its own 16 MB disc twice, as an `.iso` (which QEMU
serves from the raw file driver, stock code) and as a MODE1/2352 `.cue`
(which goes through libdisc and our ATAPI model), and runs the sweep on
both. The two discs hold the same user data, so the cue's 2048-byte
checksums must equal the iso's: that A/B is the whole cdimage path against
stock QEMU, at every rate.

THROTTLE=<bytes/s> holds the drive itself to a speed through QEMU's block
throttle, the only speed knob there is, and it reaches a `.iso`
but not a `.cue` / `.ccd` / `.mds` (those reads bypass the block layer).

Knobs: LBA= start sector, WINDOW= sectors per pass (default 512 = 1 MB),
RATES= comma-separated KB/s targets (0 = unpaced), SCAN=1 whole disc,
SIZE_MB= the generated disc, TIMEOUT= seconds, KEEP=1 keeps the run's files.

Needs nasm, mtools, build/qemu, target/release/discx, and the FreeDOS
floppy tools/x87-guest-test.py fetches.
"""
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = os.path.join(ROOT, "build/qemu/qemu-system-i386")
DISCX = os.path.join(ROOT, "target/release/discx")
FLOPPY = os.path.join(ROOT, "build/images/144m/x86BOOT.img")
OUT = os.path.join(ROOT, "build/cd-rate")

PASS_SIZE = 20
SECTOR = 2048
TO_END = 0xFFFFFFFF

# 1x CD = 150 KB/s. 0 = unpaced (as fast as the guest can ask).
RATES = [int(r) for r in os.environ.get("RATES", "0,150,600,2400").split(",")]
WINDOW = int(os.environ.get("WINDOW", "512"))
START_LBA = int(os.environ.get("LBA", "16"))
SIZE_MB = int(os.environ.get("SIZE_MB", "16"))
TIMEOUT = int(os.environ.get("TIMEOUT", "900"))
SCAN = os.environ.get("SCAN") == "1"
# Bytes per second to hold the *drive* to, through QEMU's block-layer
# throttle (1x CD = 153600). It reaches a .iso, which QEMU serves from the
# raw file driver; it does not reach a .cue / .ccd / .mds, whose reads go
# straight from hw/ide/atapi.c into libdisc and never touch a BlockBackend,
# which the A/B between the two generated discs shows directly.
THROTTLE = int(os.environ.get("THROTTLE", "0"))


# ---------------------------------------------------------------- the passes

def sizes_for(secsz):
    """Sectors per request. The largest is the biggest whole number of
    sectors under 64 KB: a PRD region may not cross a 64 KB boundary and the
    PIO byte-count limit is 16-bit, so one request stays inside one."""
    return [1, 8, (65536 // secsz) - (1 if secsz == 2048 else 0)]


def pace_us(secsz, persec, kbps):
    """Microseconds to idle after a request so the pass runs at ~kbps."""
    if kbps <= 0:
        return 0
    return min(int(secsz * persec * 1000000 / (kbps * 1024)), 500000)


def build_passes(lba, count):
    """(mode, persec, secsz, bcl, pace_us, lba, count) tuples.

    Rate x request size x transfer path over 2048-byte reads is the main
    sweep; the byte-count limit is varied once at one size (it only shapes
    the PIO loop), and the raw 2352 passes repeat the same range through
    READ CD, where the sector carries its own EDC/ECC."""
    out = []
    for mode in (0, 1):                       # PIO, DMA
        for persec in sizes_for(2048):
            for kbps in RATES:
                out.append((mode, persec, 2048, 65534, pace_us(2048, persec, kbps), lba, count))
    for bcl in (2048, 8192):                  # PIO only: DMA ignores the BCL
        out.append((0, 8, 2048, bcl, 0, lba, count))
    for mode in (0, 1):
        for persec in (1, sizes_for(2352)[2]):
            for kbps in (0, 150):
                out.append((mode, persec, 2352, 65534, pace_us(2352, persec, kbps), lba, count))
    return out


def scan_passes(lba):
    """One sequential read of the whole disc, the way a game streams it."""
    return [(1, sizes_for(2048)[2], 2048, 65534, 0, lba, TO_END)]


def table_asm(passes):
    lines = []
    for mode, persec, secsz, bcl, pace, lba, count in passes:
        lines.append("    db %d, %d\n    dw %d, %d\n    dd %d, %d, %d\n    dw 0"
                     % (mode, persec, secsz, bcl, pace, lba, count & 0xFFFFFFFF))
    lines.append("    db 0FFh\n    times 19 db 0")
    return "\n".join(lines)


# ---------------------------------------------------------------- the guest

ASM = r"""
; A DOS program that reads the CD-ROM on the secondary channel by PIO and by
; bus-master DMA at a controlled rate, and checksums every byte it gets.
; Built by tools/cd-rate-guest-test.py; the pass table is patched in below.
org 100h
bits 16
cpu 486                                 ; bswap

%define BASE 170h                       ; secondary channel: -cdrom is its master
%define CTRL 376h

start:
    mov ax, cs
    add ax, 1000h
    add ax, 0FFFh
    and ax, 0F000h                      ; 64 KB-aligned physically: a PRD
    mov [bufseg], ax                    ; region may not cross a 64 KB bound
    sub ax, 100h                        ; 4 KB below it: the PRD table
    mov [prdseg], ax
    mov si, str_hello
    call puts
    call find_bm
    mov si, str_bm
    call puts
    mov ax, [bmbase]
    call put_hex16
    call newline
    call calibrate
    mov si, str_cal
    call puts
    mov eax, [loops_per_ms]
    call put_hex32
    call newline
    call read_capacity
    mov si, str_cap
    call puts
    mov eax, [cap_sectors]
    call put_hex32
    call newline
    mov word [p_idx], 0
    mov si, passes
.next:
    cmp byte [si], 0FFh
    je .done
    call run_pass
    add si, 20
    inc word [p_idx]
    jmp .next
.done:
    mov si, str_done
    call puts
    int 20h

; ------------------------------------------------------------------ PCI/BMIDE

pci_read:                               ; al = register, [pci_dev] = selector -> eax
    push dx
    movzx eax, al
    and eax, 0FCh
    or eax, [pci_dev]
    mov dx, 0CF8h
    out dx, eax
    mov dx, 0CFCh
    in eax, dx
    pop dx
    ret

pci_write:                              ; al = register, ebx = value
    push dx
    movzx eax, al
    and eax, 0FCh
    or eax, [pci_dev]
    mov dx, 0CF8h
    out dx, eax
    mov eax, ebx
    mov dx, 0CFCh
    out dx, eax
    pop dx
    ret

find_bm:                                ; the IDE controller's BAR4 (I/O)
    mov word [bmbase], 0
    mov word [pci_devno], 0
.dev:
    mov word [pci_fnno], 0
.fn:
    mov eax, 80000000h
    movzx ebx, word [pci_devno]
    shl ebx, 11
    or eax, ebx
    movzx ebx, word [pci_fnno]
    shl ebx, 8
    or eax, ebx
    mov [pci_dev], eax
    xor al, al
    call pci_read
    cmp ax, 0FFFFh
    je .nextfn
    mov al, 08h
    call pci_read
    shr eax, 16
    cmp ax, 0101h                       ; class 01 (storage) subclass 01 (IDE)
    jne .nextfn
    mov al, 20h
    call pci_read
    test al, 1                          ; an I/O BAR
    jz .nextfn
    and ax, 0FFFCh
    test ax, ax
    jz .nextfn
    mov [bmbase], ax
    ; Bus mastering has to be turned on in PCI config space: with
    ; PCI_COMMAND_MASTER clear the device's DMA address space is empty and
    ; every transfer lands nowhere, with a clean status and no error --
    ; the engine runs, the drive is happy and the buffer stays untouched.
    ; Windows' IDE driver does this; the BIOS and DOS do not.
    mov al, 04h
    call pci_read
    mov ebx, eax
    or ebx, 5                           ; I/O space + bus master
    mov al, 04h
    call pci_write
    ret
.nextfn:
    inc word [pci_fnno]
    cmp word [pci_fnno], 8
    jb .fn
    inc word [pci_devno]
    cmp word [pci_devno], 32
    jb .dev
    ret

; ------------------------------------------------------------------ the clock

ticks:                                  ; -> eax = BIOS tick count (18.2 Hz)
    push es
    push bx
    xor bx, bx
    mov es, bx
    mov eax, [es:046Ch]
    pop bx
    pop es
    ret

calibrate:                              ; iterations of the delay loop per ms
    mov ebx, 1000000
.try:
    call ticks
    mov edx, eax
.edge:
    call ticks
    cmp eax, edx
    je .edge
    mov edx, eax
    mov ecx, ebx
.spin:
    dec ecx
    jnz .spin
    call ticks
    sub eax, edx
    cmp eax, 8                          ; enough ticks to divide by
    jae .got
    shl ebx, 1
    jmp .try
.got:
    mov ecx, eax
    imul ecx, ecx, 54925                ; microseconds per tick, x1000
    mov eax, ebx
    xor edx, edx
    mov edi, 1000
    mul edi
    div ecx
    mov [loops_per_ms], eax
    ret

delay_us:                               ; eax = microseconds
    test eax, eax
    jz .d
    mul dword [loops_per_ms]
    mov ecx, 1000
    div ecx
    mov ecx, eax
    jecxz .d
.l: dec ecx
    jnz .l
.d: ret

; ------------------------------------------------------------------ PIO packet

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

wait_drq:                               ; BSY clear and DRQ or ERR
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

pio_packet:                             ; ds:si = 12-byte packet -> bufseg:0, [total]
    mov word [total], 0
    mov dx, BASE+6
    mov al, 0A0h
    out dx, al
    call wait_bsy_clear
    jc .timeout
    mov dx, CTRL
    mov al, 02h                         ; nIEN: we poll
    out dx, al
    mov dx, BASE+1
    xor al, al                          ; PIO
    out dx, al
    mov dx, BASE+4
    mov al, [p_bcl]
    out dx, al
    inc dx
    mov al, [p_bcl+1]
    out dx, al
    mov dx, BASE+7
    mov al, 0A0h                        ; PACKET
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

; ------------------------------------------------------------------ DMA packet

dma_packet:                             ; ds:si = packet, [req_bytes] = the transfer
    push si
    mov es, [prdseg]                    ; one PRD entry: the whole buffer
    movzx eax, word [bufseg]
    shl eax, 4
    mov [es:0], eax
    mov ax, [req_bytes]
    mov [es:4], ax                      ; 0 would mean 64 KB; we stay under it
    mov word [es:6], 8000h              ; end of table
    mov dx, [bmbase]
    add dx, 8                           ; secondary channel's command register
    xor al, al
    out dx, al                          ; engine stopped
    add dx, 2
    mov al, 06h
    out dx, al                          ; clear interrupt + error
    add dx, 2
    movzx eax, word [prdseg]
    shl eax, 4
    out dx, eax                         ; PRD table pointer
    pop si
    mov dx, BASE+6
    mov al, 0A0h
    out dx, al
    call wait_bsy_clear
    jc .timeout
    mov dx, CTRL
    mov al, 02h
    out dx, al
    mov dx, BASE+1
    mov al, 01h                         ; DMA
    out dx, al
    mov dx, BASE+4
    mov al, 0FEh
    out dx, al
    inc dx
    mov al, 0FFh
    out dx, al
    mov dx, BASE+7
    mov al, 0A0h
    out dx, al
    call wait_drq
    jc .fail
    mov dx, BASE
    mov cx, 6
    rep outsw
    mov dx, [bmbase]
    add dx, 8
    mov al, 09h                         ; start, device -> memory
    out dx, al
    mov ecx, 20000000
.w: mov dx, [bmbase]
    add dx, 0Ah
    in al, dx
    mov [bm_status], al
    test al, 02h                        ; the engine faulted
    jnz .bmerr
    test al, 01h                        ; still active
    jz .stopped
    dec ecx
    jnz .w
    jmp .timeout
.stopped:
    mov dx, [bmbase]
    add dx, 8
    xor al, al
    out dx, al
    call wait_bsy_clear
    jc .timeout
    mov dx, BASE+7
    in al, dx
    mov [last_status], al
    test al, 01h
    jnz .fail
    mov ax, [req_bytes]
    mov [total], ax
    clc
    ret
.bmerr:
    mov dx, [bmbase]
    add dx, 8
    xor al, al
    out dx, al
    mov byte [last_error], 0FEh
    stc
    ret
.fail:
    mov dx, [bmbase]
    add dx, 8
    xor al, al
    out dx, al
    mov dx, BASE+1
    in al, dx
    mov [last_error], al
    stc
    ret
.timeout:
    mov dx, [bmbase]
    add dx, 8
    xor al, al
    out dx, al
    mov byte [last_status], 0FFh
    mov byte [last_error], 0FFh
    stc
    ret

; ------------------------------------------------------------------ one request

build_packet:                           ; [p_lba], [p_n], [p_secsz] -> packet
    push es
    push ds
    pop es
    mov di, packet
    mov cx, 12
    xor al, al
    rep stosb
    pop es
    mov eax, [p_lba]
    bswap eax
    mov [packet+2], eax
    cmp word [p_secsz], 2048
    jne .readcd
    mov byte [packet], 28h              ; READ(10)
    mov ax, [p_n]
    mov [packet+8], al
    mov [packet+7], ah
    ret
.readcd:
    mov byte [packet], 0BEh             ; READ CD, everything: sync..EDC/ECC
    mov ax, [p_n]
    mov [packet+8], al
    mov [packet+7], ah
    mov byte [packet+9], 0F8h
    ret

fill_poison:                            ; so a short transfer cannot pass
    push es
    mov es, [bufseg]
    xor di, di
    mov eax, 0A5A5A5A5h
    mov cx, 4000h
    rep stosd
    pop es
    ret

try_read:                               ; [p_n] sectors at [p_lba]; carry on error
    mov ax, [p_n]
    mul word [p_secsz]
    mov [req_bytes], ax                 ; always < 64 KB (see sizes_for)
    call fill_poison
    call build_packet
    mov si, packet
    cmp byte [p_mode], 1
    je .dma
    call pio_packet
    ret
.dma:
    call dma_packet
    ret

sum_result:                             ; fold [req_bytes] bytes into [p_sum]
    mov ax, [total]
    cmp ax, [req_bytes]
    je .len_ok
    inc dword [p_short]
.len_ok:
    push es                             ; the tail of the buffer still poisoned
    mov es, [bufseg]                    ; = a transfer that did not happen (the
    mov si, [req_bytes]                 ; DMA engine reports nothing when it
    sub si, 4                           ; writes into a disabled address space)
    mov eax, [es:si]
    pop es
    cmp eax, 0A5A5A5A5h
    jne .filled
    inc dword [p_short]
.filled:
    mov cx, [req_bytes]
    shr cx, 1
    mov edx, [p_sum]
    push es
    mov es, [bufseg]
    xor si, si
.l: mov ax, [es:si]
    add si, 2
    rol edx, 1
    movzx eax, ax
    add edx, eax
    dec cx
    jnz .l
    pop es
    mov [p_sum], edx
    ret

note_error:                             ; the sector at [p_lba] was refused
    inc dword [p_errs]
    cmp dword [p_first], 0FFFFFFFFh
    jne .done
    mov eax, [p_lba]
    mov [p_first], eax
    movzx eax, byte [last_status]
    mov ah, [last_error]
    mov [p_stat], eax
    push word [p_bcl]
    mov word [p_bcl], 512
    mov si, pkt_sense
    call pio_packet
    pop word [p_bcl]
    jc .done
    push es
    mov es, [bufseg]
    movzx eax, byte [es:2]              ; sense key
    and al, 0Fh
    shl eax, 8
    mov al, [es:12]                     ; ASC
    shl eax, 8
    mov al, [es:13]                     ; ASCQ -> key:asc:ascq
    pop es
    mov [p_sense], eax
.done:
    ret

do_request:                             ; the request, then one sector at a time
    call try_read
    jc .retry
    call sum_result
    ret
.retry:
    mov word [rt_i], 0
.r: mov ax, [rt_i]
    cmp ax, [p_n]
    jae .rdone
    push dword [p_lba]
    push word [p_n]
    movzx eax, word [rt_i]
    add [p_lba], eax
    mov word [p_n], 1
    call try_read
    jc .rerr
    call sum_result
    jmp .rnext
.rerr:
    call note_error
.rnext:
    pop word [p_n]
    pop dword [p_lba]
    inc word [rt_i]
    jmp .r
.rdone:
    ret

; ------------------------------------------------------------------ one pass

run_pass:                               ; si -> a 20-byte record
    push si
    xor eax, eax
    mov [p_mode], eax
    mov [p_persec], eax
    mov [p_secsz], eax
    mov [p_bcl], eax
    mov al, [si]
    mov [p_mode], al
    mov al, [si+1]
    mov ah, 0
    mov [p_persec], ax
    mov ax, [si+2]
    mov [p_secsz], ax
    mov ax, [si+4]
    mov [p_bcl], ax
    mov eax, [si+6]
    mov [p_pace], eax
    mov eax, [si+10]
    mov [p_lba], eax
    mov [p_lba0], eax
    mov eax, [si+14]
    cmp eax, 0FFFFFFFFh
    jne .have
    mov eax, [cap_sectors]
    sub eax, [p_lba]
.have:
    mov [p_count], eax
    mov [p_count0], eax
    xor eax, eax
    mov [p_sum], eax
    mov [p_errs], eax
    mov [p_short], eax
    mov [p_sense], eax
    mov [p_stat], eax
    mov dword [p_first], 0FFFFFFFFh
    call ticks
    mov [p_t0], eax
.loop:
    mov eax, [p_count]
    test eax, eax
    jz .end
    movzx ecx, word [p_persec]
    cmp eax, ecx
    jae .n_ok
    mov ecx, eax
.n_ok:
    mov [p_n], cx
    call do_request
    movzx ebx, word [p_n]
    mov eax, [p_lba]
    add eax, ebx
    mov [p_lba], eax
    mov eax, [p_count]
    sub eax, ebx
    mov [p_count], eax
    mov eax, [p_pace]
    call delay_us
    jmp .loop
.end:
    call ticks
    sub eax, [p_t0]
    mov [p_ticks], eax
    call print_pass
    pop si
    ret

put_field:                              ; si = label, [pf_val] = value
    call puts
    mov eax, [pf_val]
    call put_hex32
    mov al, ' '
    jmp putc

%macro FIELD 2                          ; label, dword variable
    mov eax, %2
    mov [pf_val], eax
    mov si, %1
    call put_field
%endmacro

print_pass:
    mov si, str_pass
    call puts
    movzx eax, word [p_idx]
    mov [pf_val], eax
    mov si, str_idx
    call put_field
    FIELD str_mode, [p_mode]
    FIELD str_sz, [p_secsz]
    FIELD str_n, [p_persec]
    FIELD str_bcl, [p_bcl]
    FIELD str_pace, [p_pace]
    FIELD str_lba, [p_lba0]
    FIELD str_cnt, [p_count0]
    FIELD str_sum, [p_sum]
    FIELD str_errs, [p_errs]
    FIELD str_short, [p_short]
    FIELD str_first, [p_first]
    FIELD str_sense, [p_sense]
    FIELD str_stat, [p_stat]
    FIELD str_ticks, [p_ticks]
    call newline
    ret

; ------------------------------------------------------------------ capacity

read_capacity:
    mov word [p_bcl], 512
    mov si, pkt_capacity
    call pio_packet
    jc .fail
    push es
    mov es, [bufseg]
    mov eax, [es:0]
    pop es
    bswap eax
    inc eax
    mov [cap_sectors], eax
    ret
.fail:
    mov dword [cap_sectors], 0
    ret

; ------------------------------------------------------------------ output

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
    cs lodsb
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
    call put_hex16
    ret
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

str_hello: db "CDRATE", 10, 0
str_bm:    db "BM ", 0
str_cal:   db "CAL ", 0
str_cap:   db "CAP ", 0
str_done:  db "DONE", 10, 0
str_pass:  db "PASS ", 0
str_idx:   db "idx=", 0
str_mode:  db "mode=", 0
str_sz:    db "sz=", 0
str_n:     db "n=", 0
str_bcl:   db "bcl=", 0
str_pace:  db "pace=", 0
str_lba:   db "lba=", 0
str_cnt:   db "cnt=", 0
str_sum:   db "sum=", 0
str_errs:  db "errs=", 0
str_short: db "short=", 0
str_first: db "first=", 0
str_sense: db "sense=", 0
str_stat:  db "stat=", 0
str_ticks: db "ticks=", 0

pkt_sense:    db 03h, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0
pkt_capacity: db 25h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0

align 4
packet:       times 12 db 0
pci_dev:      dd 0
pci_devno:    dw 0
pci_fnno:     dw 0
bmbase:       dw 0
bufseg:       dw 0
prdseg:       dw 0
loops_per_ms: dd 0
cap_sectors:  dd 0
total:        dw 0
req_bytes:    dw 0
rt_i:         dw 0
last_status:  db 0
last_error:   db 0
bm_status:    db 0
pad0:         db 0
pf_val:       dd 0
p_idx:        dw 0
              dw 0
; the record's fields, each a dword so print_pass can hand it to put_field;
; run_pass zeroes them before writing the byte / word the record holds
p_mode:       dd 0
p_persec:     dd 0
p_secsz:      dd 0
p_bcl:        dd 0
p_pace:       dd 0
p_lba:        dd 0
p_lba0:       dd 0
p_count:      dd 0
p_count0:     dd 0
p_n:          dw 0
              dw 0
p_sum:        dd 0
p_errs:       dd 0
p_short:      dd 0
p_first:      dd 0
p_sense:      dd 0
p_stat:       dd 0
p_t0:         dd 0
p_ticks:      dd 0

align 4
passes:
{table}
"""


# ---------------------------------------------------------------- the harness

def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def ensure_prereqs():
    missing = [t for t in ("nasm", "mcopy") if shutil.which(t) is None]
    if missing:
        raise SystemExit("missing %s (nasm, mtools)" % ", ".join(missing))
    for f, hint in ((QEMU, "build QEMU"), (DISCX, "cargo build --release -p libdisc"),
                    (FLOPPY, "run tools/x87-guest-test.py once (fetches the FreeDOS floppy)")):
        if not os.path.exists(f):
            raise SystemExit("%s missing: %s" % (f, hint))


def make_discs():
    """A disc whose every sector is different and known: LBA-seeded bytes, as
    a plain .iso (QEMU's raw driver) and as the same data in a MODE1/2352
    cue/bin (libdisc, our ATAPI model). Same user data, so their 2048-byte
    checksums must agree."""
    iso = os.path.join(OUT, "rate.iso")
    cue = os.path.join(OUT, "rate.cue")
    want = SIZE_MB * 1024 * 1024
    if not os.path.exists(iso) or os.path.getsize(iso) != want:
        with open(iso, "wb") as f:
            for lba in range(want // SECTOR):
                x = (lba * 2654435761) & 0xFFFFFFFF
                b = bytearray()
                for _ in range(SECTOR // 4):
                    x = (x * 1103515245 + 12345) & 0xFFFFFFFF
                    b += x.to_bytes(4, "little")
                f.write(bytes(b))
    if not os.path.exists(cue):
        sh(DISCX, "convert", iso, cue, stdout=subprocess.DEVNULL)
    return [iso, cue]


def write_fdconfig():
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    return cfg


def build_floppy(passes, tag):
    asm = os.path.join(OUT, "cdrate-%s.asm" % tag)
    com = os.path.join(OUT, "CDRATE.COM")
    with open(asm, "w") as f:
        f.write(ASM.replace("{table}", table_asm(passes)))
    sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    img = os.path.join(OUT, "cdrate-%s.img" % tag)
    shutil.copy(FLOPPY, img)
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nCDRATE.COM\r\n")
    sh("mcopy", "-o", "-i", img, write_fdconfig(), "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::CDRATE.COM")
    return img


def run_qemu(img, disc, log, qemu_log):
    if os.path.exists(log):
        os.unlink(log)
    p = subprocess.Popen([
        QEMU, "-machine", "pc", "-cpu", "pentium3", "-m", "64",
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        "-fda", img, "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
        "-drive", "if=none,id=cd0,media=cdrom,file=" + disc
        + (",throttling.bps-read=%d" % THROTTLE if THROTTLE else ""),
        "-device", "ide-cd,bus=ide.1,id=ide1-cd0,drive=cd0,audiodev=w0",
        "-audiodev", "none,id=w0",
    ], stderr=open(qemu_log, "w"))
    t0 = time.time()
    try:
        while time.time() - t0 < TIMEOUT:
            time.sleep(2)
            if p.poll() is not None:
                break
            if os.path.exists(log):
                with open(log, "rb") as f:
                    f.seek(0, 2)
                    n = f.tell()
                    f.seek(max(0, n - 64))
                    if b"DONE" in f.read():
                        break
        else:
            raise SystemExit("timeout waiting for the guest (%s)" % log)
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    return time.time() - t0


# ---------------------------------------------------------------- the verdict

FIELD_RE = re.compile(r"(\w+)=([0-9a-f]{1,8})")


def parse(text):
    head, rows = {}, []
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("PASS "):
            rows.append({k: int(v, 16) for k, v in FIELD_RE.findall(line)})
        elif line.startswith(("BM ", "CAL ", "CAP ")):
            k, _, v = line.partition(" ")
            try:
                head[k] = int(v.strip(), 16)
            except ValueError:
                pass
    return head, rows


def host_sum(path, lba, count):
    """The same rolling checksum over the cooked image, for the 2048 passes."""
    s = 0
    with open(path, "rb") as f:
        f.seek(lba * SECTOR)
        data = f.read(count * SECTOR)
    if len(data) < count * SECTOR:
        return None
    for i in range(0, len(data), 2):
        w = data[i] | (data[i + 1] << 8)
        s = ((s << 1) | (s >> 31)) & 0xFFFFFFFF
        s = (s + w) & 0xFFFFFFFF
    return s


def describe(r):
    return ("pass %2d %s sz=%d n=%-2d bcl=%-5d pace=%dus"
            % (r["idx"], "dma" if r["mode"] else "pio", r["sz"], r["n"], r["bcl"], r["pace"]))


def rate_of(r):
    secs = r["ticks"] * 54.9254 / 1000.0
    if secs <= 0:
        return None
    return r["cnt"] * r["sz"] / 1024.0 / secs


def report(disc, head, rows, ref):
    print("\n== %s ==" % os.path.basename(disc))
    print("   bus master at 0x%04x, %d sectors on the disc, %d passes%s"
          % (head.get("BM", 0), head.get("CAP", 0), len(rows),
             ", drive throttled to %d KB/s" % (THROTTLE // 1024) if THROTTLE else ""))
    failures = []
    if not head.get("BM"):
        failures.append("no bus-master IDE controller found: the DMA passes are meaningless")
    for r in rows:
        kb = rate_of(r)
        print("   %s -> sum=%08x errs=%d%s%s" % (
            describe(r), r["sum"], r["errs"],
            " short=%d" % r["short"] if r["short"] else "",
            "  %6.0f KB/s" % kb if kb else ""))
        if r["errs"]:
            failures.append("%s: %d sector(s) refused, first at LBA %d, "
                            "sense key=%x asc=%02x ascq=%02x (status %02x error %02x)"
                            % (describe(r), r["errs"], r["first"],
                               (r["sense"] >> 16) & 0xF, (r["sense"] >> 8) & 0xFF,
                               r["sense"] & 0xFF, r["stat"] & 0xFF, (r["stat"] >> 8) & 0xFF))
        if r["short"]:
            failures.append("%s: %d request(s) came up short" % (describe(r), r["short"]))
    # every pass over one range and sector size must agree with every other
    groups = {}
    for r in rows:
        groups.setdefault((r["sz"], r["lba"], r["cnt"]), []).append(r)
    for (sz, lba, cnt), rs in sorted(groups.items()):
        sums = {r["sum"] for r in rs if not r["errs"]}
        if len(sums) > 1:
            failures.append(
                "%d-byte sectors %d..%d: the data depends on how it is read -- %s"
                % (sz, lba, lba + cnt - 1,
                   ", ".join("%s sum=%08x" % (describe(r), r["sum"]) for r in rs)))
        elif sz == SECTOR and ref and sums:
            want = host_sum(ref, lba, cnt)
            got = sums.pop()
            if want is not None and want != got:
                failures.append("sectors %d..%d: every pass reads %08x, the image holds %08x"
                                % (lba, lba + cnt - 1, got, want))
    return failures


def main():
    ensure_prereqs()
    os.makedirs(OUT, exist_ok=True)
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    discs = args or make_discs()
    ref = os.path.join(OUT, "rate.iso") if not args else (
        args[0] if args[0].lower().endswith(".iso") else None)

    failures, sums = [], {}
    for disc in discs:
        if not os.path.exists(disc):
            raise SystemExit("%s: no such disc image" % disc)
        tag = os.path.basename(disc).replace(".", "_")
        passes = scan_passes(START_LBA) if SCAN else build_passes(START_LBA, WINDOW)
        img = build_floppy(passes, tag)
        log = os.path.join(OUT, "serial-%s.log" % tag)
        secs = run_qemu(img, disc, log, os.path.join(OUT, "qemu-%s.log" % tag))
        head, rows = parse(open(log, "r", errors="replace").read())
        print("%s: %d passes in %.0f s" % (os.path.basename(disc), len(rows), secs))
        if len(rows) != len(passes):
            failures.append("%s: %d of %d passes reported (%s)"
                            % (disc, len(rows), len(passes), log))
        failures += report(disc, head, rows, ref)
        for r in rows:
            if r["sz"] == SECTOR and not r["errs"]:
                sums.setdefault((r["lba"], r["cnt"]), {})[disc] = r["sum"]

    # the same user data through the raw driver and through libdisc
    for (lba, cnt), by_disc in sums.items():
        if len(set(by_disc.values())) > 1:
            failures.append("sectors %d..%d differ between discs: %s"
                            % (lba, lba + cnt - 1,
                               ", ".join("%s=%08x" % (os.path.basename(d), s)
                                         for d, s in by_disc.items())))

    print()
    if failures:
        print("cd rate test: FAIL")
        for f in failures:
            print("  " + f)
        return 1
    print("cd rate test: the bytes are the same at every rate, request size, "
          "byte-count limit and transfer path, and equal to the image")
    return 0


if __name__ == "__main__":
    sys.exit(main())
