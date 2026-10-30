#!/usr/bin/env python3
"""Clicks in what the host hears: a DOS guest plays a pure tone through a
sound device **in the player**, the player's consumer records exactly what
it hands the audio device (`PLAYER_AUDIO_TAP`), and every discontinuity in
that recording is counted. "Sounds crackle sometimes" is a count here.

The consumer is the player's simulated DAC (`PLAYER_AUDIO_NULL=<frames>`):
it drains the ring the way a real device does (a whole period at a time,
1024 frames being what PipeWire hands a client), so the run is headless
and repeatable, and a real device's burstiness is part of it.

Two sources, because they fail differently:

    tools/audio-glitch-test.py sb16    # the default
    tools/audio-glitch-test.py opl
    tools/audio-glitch-test.py cd      # CD-DA off the drive
    tools/audio-glitch-test.py cd+sb16 # both at once: a race with CD music

**sb16** is what a Windows or DOS game does with a Sound Blaster: 8-bit
auto-init DMA at 22050 Hz out of an 8 KiB ring, the guest keeping the ring
filled a fixed margin (`MARGIN=` samples, default 441 = 20 ms) ahead of the
play cursor it reads off the DMA controller, DirectSound's own scheme.
QEMU's sb16 moves the guest's DMA exactly as far as the audio core asks,
so if the host side ever pulls the guest ahead of wall time by more than
the margin, the card plays samples the guest has not written yet. The
guest sees that itself (the cursor passed its write position) and says so
on COM1: `late` events, the `stale` samples played, and the largest jump
of the cursor between two polls.

**opl** is a synthesizer: nothing can be read ahead of a guest there, so
a click in it is the ring's own: audio dropped by the producer or a
consumer that ran dry.

The verdict is the tap's: a least-squares sine recurrence
(y[n] = c·y[n-1] − y[n-2]) is exact for a pure tone whatever its phase and
amplitude, so its residual is quantisation noise everywhere except where
the waveform breaks: a dropped tick, a padded silence, a stale buffer.

Knobs: `PERIOD=` the DAC's frames (1024), `SECS=` (20), `MARGIN=` (sb16),
`ICOUNT=1` paces the guest like a DOS machine (`-icount shift=7,align=on`),
`QEMU_EXTRA=` more QEMU arguments. Outputs in build/audio-glitch/. Needs
nasm, mtools, the FreeDOS floppy `tools/x87-guest-test.py` fetches, and a
display (it is the player). Local only, not in scripts/test.sh.
"""
import importlib.util
import math
import os
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLAYER = os.path.join(ROOT, "player-mitsuami/target/release/player-mitsuami")
OUT = os.path.join(ROOT, "build/audio-glitch")
RATE = 48000

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

# 22050 / 50 = 441 Hz: the table is one whole period, so the phase of
# sample n is n mod 50 and a stale sample is a phase jump.
TABLE = ", ".join(str(round(128 + 100 * math.sin(2 * math.pi * k / 50))) for k in range(50))

ASM = r"""
org 100h
bits 16
cpu 686

RING    equ 8192                ; bytes = samples (8-bit mono)
BUFSEG  equ 8000h               ; physical 0x80000: 64 KiB aligned
PERIOD  equ 50

start:
    cld
    mov si, str_start
    call puts
%ifdef CD
    call cd_play
%endif
%ifdef OPL
    call opl_note
    mov cx, TICKS
    call delay_ticks
    mov al, 0B0h
    mov ah, 12h                 ; key off
    call opl_write
%elifdef SB
    call sb_play
%ifdef CD
    ; end the two tones together: a CD playing on alone after the card
    ; stops is an amplitude step, which the detector would count
    mov si, pkt_stop
    call send_packet
%endif
%else
    mov cx, TICKS
    call delay_ticks
%endif
    mov si, str_done
    call puts
    mov ax, 4C00h
    int 21h

%ifdef OPL
; One channel, two operators at the same pitch, additive: a sine at 440 Hz
; (fnum 580, block 4), no decay, held until key off.
opl_note:
    mov al, 05h
    mov ah, 01h
    call opl_write3
    mov al, 01h
    mov ah, 20h
    call opl_write
    mov bl, 0
.op:
    mov al, 20h
    add al, bl
    mov ah, 01h
    call opl_write
    mov al, 40h
    add al, bl
    mov ah, 00h
    call opl_write
    mov al, 60h
    add al, bl
    mov ah, 0F0h
    call opl_write
    mov al, 80h
    add al, bl
    mov ah, 77h
    call opl_write
    cmp bl, 0
    jne .voice
    mov bl, 3
    jmp .op
.voice:
    mov al, 0C0h
    mov ah, 31h
    call opl_write
    mov al, 0A0h
    mov ah, 44h
    call opl_write
    mov al, 0B0h
    mov ah, 32h
    call opl_write
    ret

opl_write:                      ; al = register, ah = value (bank 0)
    push dx
    mov dx, 388h
    jmp opl_write_at
opl_write3:
    push dx
    mov dx, 38Ah
opl_write_at:
    push cx
    push ax
    out dx, al
    mov cx, 6
.a: in al, dx
    loop .a
    pop ax
    push ax
    mov al, ah
    inc dx
    out dx, al
    dec dx
    mov cx, 35
.d: in al, dx
    loop .d
    pop ax
    pop cx
    pop dx
    ret
%else
; ----------------------------------------------------------------- SB16

sb_play:
%if LOAD
    mov ax, 13h                 ; Mode X: 13h unchained, all four planes
    int 10h
    mov dx, 3C4h
    mov ax, 0604h               ; memory mode: chain-4 and odd/even off
    out dx, ax
    mov ax, 0F02h               ; map mask: every plane
    out dx, ax
    mov dx, 3D4h
    mov ax, 0014h               ; underline: dword mode off
    out dx, ax
    mov ax, 0E317h              ; mode control: byte mode
    out dx, ax
%endif
    in al, 21h
    or al, 20h                  ; IRQ5 masked: this program polls
    out 21h, al
    call dsp_reset
    jc .fail
    mov ax, BUFSEG
    mov es, ax
    mov ax, 40h
    mov fs, ax
    xor edi, edi                ; edi = next sample index to write
    xor bp, bp                  ; bp = its phase, edi mod PERIOD
    mov eax, RING               ; the whole ring before the DMA starts
    call fill_to
    mov al, 0D1h                ; speaker on
    call dsp_write
    mov al, 41h                 ; output rate 22050
    call dsp_write
    mov al, 56h
    call dsp_write
    mov al, 22h
    call dsp_write
    mov al, 05h                 ; DMA 1: masked while it is programmed
    out 0Ah, al
    out 0Ch, al
    mov al, 59h                 ; single, auto-init, memory to device
    out 0Bh, al
    xor al, al
    out 02h, al
    out 02h, al
    mov al, 08h                 ; page: 0x80000 >> 16
    out 83h, al
    mov al, (RING-1) & 0FFh
    out 03h, al
    mov al, (RING-1) >> 8
    out 03h, al
    mov al, 01h
    out 0Ah, al
    mov al, 0C6h                ; 8-bit auto-init output
    call dsp_write
    mov al, 00h                 ; mono, unsigned
    call dsp_write
    mov al, (RING/4-1) & 0FFh   ; an interrupt every quarter ring
    call dsp_write
    mov al, (RING/4-1) >> 8
    call dsp_write

    xor esi, esi                ; esi = the play cursor, as a sample index
    mov ax, [fs:6Ch]
    mov [t0], ax
.loop:
    call read_pos
    mov bx, ax
    sub ax, [last]
    and ax, RING-1
    mov [last], bx
    movzx eax, ax
    add esi, eax
    inc dword [polls]
    cmp eax, [maxjump]
    jbe .j
    mov [maxjump], eax
.j:
    cmp esi, edi
    jbe .ahead
    ; The card has read past what was written: it played the previous
    ; lap's samples. Count them and write on from the cursor.
    inc dword [late]
    mov eax, esi
    sub eax, edi
    add [stale], eax
    mov edi, esi
    mov eax, edi
    xor edx, edx
    mov ecx, PERIOD
    div ecx
    mov bp, dx
.ahead:
    mov eax, esi
    add eax, MARGIN
    call fill_to
    mov dx, 22Eh                ; acknowledge the block interrupt
    in al, dx
%if LOAD
    ; A game's frame going out through unchained VGA memory, a slice per
    ; poll: every store is a device access QEMU takes its big lock for.
    push es
    push edi
    mov ax, 0A000h
    mov es, ax
    mov di, [vga_off]
    mov cx, LOAD/4
    mov eax, esi
    rep stosd
    add word [vga_off], LOAD
    cmp word [vga_off], 16000
    jb .nov
    mov word [vga_off], 0
.nov:
    pop edi
    pop es
%endif
    mov ax, [fs:6Ch]
    sub ax, [t0]
    cmp ax, TICKS
    jb .loop

    mov al, 0DAh                ; exit auto-init
    call dsp_write
    mov al, 0D3h                ; speaker off
    call dsp_write
    mov si, str_late
    call puts
    mov eax, [late]
    call put_hex32
    mov si, str_stale
    call puts
    mov eax, [stale]
    call put_hex32
    mov si, str_jump
    call puts
    mov eax, [maxjump]
    call put_hex32
    mov si, str_polls
    call puts
    mov eax, [polls]
    call put_hex32
    mov al, 10
    call putc
    ret
.fail:
    mov si, str_nodsp
    jmp puts

fill_to:                        ; write samples up to index eax (exclusive)
    push bx
    push dx
.f: cmp edi, eax
    jae .d
    mov bx, di
    and bx, RING-1
    mov dl, [ds:table+bp]
    mov [es:bx], dl
    inc edi
    inc bp
    cmp bp, PERIOD
    jb .f
    xor bp, bp
    jmp .f
.d: pop dx
    pop bx
    ret

read_addr:                      ; ax = DMA 1's current address
    out 0Ch, al
    in al, 02h
    mov ah, al
    in al, 02h
    xchg al, ah
    ret

read_pos:                       ; the same, read until two reads agree
    push bx
.r: call read_addr
    mov bx, ax
    call read_addr
    cmp ax, bx
    jne .r
    pop bx
    ret

dsp_reset:                      ; CF = no DSP answered
    mov dx, 226h
    mov al, 1
    out dx, al
    mov cx, 64
.d: in al, dx
    loop .d
    xor al, al
    out dx, al
    mov cx, 0FFFFh
.w: mov dx, 22Eh
    in al, dx
    test al, 80h
    jnz .r
    loop .w
    stc
    ret
.r: mov dx, 22Ah
    in al, dx
    cmp al, 0AAh
    jne .bad
    clc
    ret
.bad:
    stc
    ret

dsp_write:                      ; al -> the DSP, once it will take it
    push dx
    push ax
    mov dx, 22Ch
.w: in al, dx
    test al, 80h
    jnz .w
    pop ax
    out dx, al
    pop dx
    ret
%endif

%ifdef CD
; ---------------------------------------------------------------- CD-DA

BASE    equ 170h                ; ide.1, the master: the machine's CD-ROM
CTRL    equ 376h

; PLAY AUDIO MSF over the whole disc. The drive's first command after power
; on fails with UNIT ATTENTION, which only a REQUEST SENSE clears: so TEST
; UNIT READY, and REQUEST SENSE after each failure, until it says ready.
cd_play:
%ifdef CDVOL
    ; the SB16's CD input volume, both sides: the drive's audio is scaled
    ; by the card's mixer the way the analog cable was (patch 61)
    mov dx, 224h
    mov al, 36h
    out dx, al
    inc dx
    mov al, CDVOL
    out dx, al
    dec dx
    mov al, 37h
    out dx, al
    inc dx
    mov al, CDVOL
    out dx, al
%endif
    mov word [bcl], 0FFFEh
    mov word [bufseg], 9000h
    mov bx, 20
.tur:
    mov si, pkt_tur
    call send_packet
    jnc .ready
    mov si, pkt_sense
    call send_packet
    mov cx, 2
    call delay_ticks
    dec bx
    jnz .tur
    mov si, str_noready
    jmp puts
.ready:
    mov si, pkt_play
    call send_packet
    jc .fail
    mov si, str_cdplay
    jmp puts
.fail:
    mov si, str_noplay
    call puts
    mov al, [last_status]
    call put_hex8
    mov al, 10
    jmp putc

send_packet:            ; ds:si = packet -> carry on error
    mov dx, BASE+6
    mov al, 0A0h
    out dx, al
    call wait_bsy_clear
    jc .timeout
    mov dx, CTRL
    mov al, 02h         ; nIEN: we poll
    out dx, al
    mov dx, BASE+1
    xor al, al
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
    push es
    mov ax, [bufseg]
    mov es, ax
    xor di, di
.loop:
    call wait_bsy_clear
    jc .timeout_es
    mov dx, BASE+7
    in al, dx
    mov [last_status], al
    test al, 01h
    jnz .fail_es
    test al, 08h
    jz .done
    mov dx, BASE+4      ; data in: take it and throw it away
    in al, dx
    mov cl, al
    inc dx
    in al, dx
    mov ch, al
    inc cx
    shr cx, 1
    mov dx, BASE
    xor di, di
    rep insw
    jmp .loop
.done:
    pop es
    clc
    ret
.fail_es:
    pop es
.fail:
    stc
    ret
.timeout_es:
    pop es
.timeout:
    mov byte [last_status], 0FFh
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

wait_drq:
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

pkt_tur:     db 00h, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
pkt_sense:   db 03h, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0
pkt_play:    db 47h, 0, 0, 0, 2, 0, CD_END_M, CD_END_S, CD_END_F, 0, 0, 0
pkt_stop:    db 4Eh, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
str_cdplay:  db "CDPLAY", 10, 0
str_noplay:  db "NOPLAY ", 0
str_noready: db "NOREADY", 10, 0
bcl:         dw 0
bufseg:      dw 0
last_status: db 0
%endif

; --------------------------------------------------------------- plumbing

delay_ticks:                    ; cx = BIOS ticks (18.2 Hz)
    push es
    push ax
    push bx
    mov ax, 40h
    mov es, ax
    mov bx, [es:6Ch]
.w: mov ax, [es:6Ch]
    sub ax, bx
    cmp ax, cx
    jb .w
    pop bx
    pop ax
    pop es
    ret

putc:                           ; al -> COM1
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
    push ax
    and al, 0Fh
    add al, '0'
    cmp al, '9'
    jbe .o
    add al, 'a' - '0' - 10
.o: call putc
    pop ax
    ret

str_start:  db "START", 10, 0
str_done:   db "DONE", 10, 0
str_nodsp:  db "NODSP", 10, 0
str_late:   db "late ", 0
str_stale:  db " stale ", 0
str_jump:   db " maxjump ", 0
str_polls:  db " polls ", 0

align 4
late:       dd 0
stale:      dd 0
maxjump:    dd 0
polls:      dd 0
last:       dw 0
t0:         dw 0
vga_off:    dw 0
table:      db TABLE
"""


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build(mode, secs, margin):
    asm = os.path.join(OUT, "glitch.asm")
    com = os.path.join(OUT, "GLITCH.COM")
    img = os.path.join(OUT, "glitch-%s.img" % mode)
    with open(asm, "w") as f:
        f.write(ASM.replace("db TABLE", "db " + TABLE))
    defs = ["-DTICKS=%d" % round(secs * 18.2), "-DMARGIN=%d" % margin,
            "-DLOAD=%d" % int(os.environ.get("LOAD", "0"))]
    parts = mode.split("+")
    if "opl" in parts:
        defs.append("-DOPL")
    if "sb16" in parts:
        defs.append("-DSB")
    if "cd" in parts:
        # the play range's end: the whole tone, as an MSF address
        m, r = divmod(CD_SECONDS * 75 + 150, 75 * 60)
        if os.environ.get("CDVOL"):
            defs.append("-DCDVOL=%d" % int(os.environ["CDVOL"], 0))
        defs += ["-DCD", "-DCD_END_M=%d" % m, "-DCD_END_S=%d" % (r // 75),
                 "-DCD_END_F=%d" % (r % 75)]
    sh("nasm", "-O0", "-f", "bin", *defs, "-o", com, asm)
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nGLITCH.COM\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::GLITCH.COM")
    return img


CD_SECONDS = 60


def make_disc(mode):
    """An audio-only disc: one track of a 441 Hz tone at 44100 Hz, 100
    samples a period, so it sums with the SB16's 441 Hz into one pure sine
    and one click detector serves both. Quieter beside the SB16, so the sum
    cannot clip."""
    amp = int(os.environ.get("CDAMP", "4500" if "sb16" in mode else "16000"))
    period = b"".join(struct.pack("<hh", v, v) for v in
                      (round(amp * math.sin(2 * math.pi * k / 100)) for k in range(100)))
    frames = CD_SECONDS * 44100
    tone = os.path.join(OUT, "tone.bin")
    with open(tone, "wb") as f:
        f.write(period * (frames // 100))
    cue = os.path.join(OUT, "tone.cue")
    with open(cue, "w") as f:
        f.write('FILE "tone.bin" BINARY\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n')
    return cue


def staller(sock, log, mb, stalls):
    """`STALL=<MB>`: a main loop that is busy for a while every frame, the
    way one is while a 3D title's swap finishes under the big lock. A QMP
    `pmemsave` runs synchronously in QEMU's main loop, so each one holds
    off the audio tick for as long as it takes; the guest keeps running."""
    qspec = importlib.util.spec_from_file_location("qmpc", os.path.join(ROOT, "tools/qmpc.py"))
    qmpc = importlib.util.module_from_spec(qspec)
    qspec.loader.exec_module(qmpc)
    while not (os.path.exists(log) and b"START" in open(log, "rb").read()):
        time.sleep(0.2)
    f = qmpc.connect(sock)
    qmpc.cmd(f, "qmp_capabilities")
    while b"DONE" not in open(log, "rb").read():
        t = time.time()
        qmpc.cmd(f, "pmemsave", {"val": 0, "size": mb << 20, "filename": "/dev/null"})
        stalls.append(time.time() - t)
        time.sleep(max(0.0, 1 / 30 - stalls[-1]))


def run(mode, img, tap, log, plog, period):
    for f in (tap, log, plog):
        if os.path.exists(f):
            os.unlink(f)
    sock = os.path.join(OUT, "q.sock")
    stall_mb = int(os.environ.get("STALL", "0"))
    env = dict(os.environ, PLAYER_AUDIO_NULL=str(period), PLAYER_AUDIO_TAP=tap)
    parts = mode.split("+")
    device = []
    if "opl" in parts:
        device += ["-device", "opl3,audiodev=embed0"]
    if "sb16" in parts or os.environ.get("CDVOL"):
        device += ["-device", "sb16,audiodev=embed0"]
    if "cd" in parts:
        device += ["-drive", "if=none,id=cd0,media=cdrom,file=" + make_disc(mode),
                   "-device", "ide-cd,bus=ide.1,drive=cd0,audiodev=embed0"]
    icount = ["-icount", "shift=7,align=on"] if os.environ.get("ICOUNT") else []
    extra = os.environ.get("QEMU_EXTRA", "").split()
    vga = ["-vga", "none", "-device", "d3dpt-vga"] if os.environ.get("VGA") == "d3dpt" \
        else ["-vga", os.environ.get("VGA", "std")]
    with open(plog, "wb") as out:
        p = subprocess.Popen([
            PLAYER, "--",
            "-L", os.path.join(ROOT, "qemu/pc-bios"),
            "-machine", "pc", "-cpu", "pentium3", "-m", str(max(16, stall_mb)),
            "-qmp", "unix:%s,server,nowait" % sock,
            *vga, "-net", "none", *device,
            "-drive", "file=%s,if=floppy,index=0,format=raw" % img,
            "-boot", "a", "-serial", "file:" + log, *icount, *extra,
        ], env=env, stdout=out, stderr=subprocess.STDOUT)
    stalls = []
    if stall_mb:
        import threading
        threading.Thread(target=staller, args=(sock, log, stall_mb, stalls),
                         daemon=True).start()
    t0 = time.time()
    try:
        while time.time() - t0 < 300:
            time.sleep(0.5)
            if p.poll() is not None:
                break
            if os.path.exists(log) and b"DONE" in open(log, "rb").read():
                time.sleep(1.5)     # the tap's header is rewritten every second
                break
        else:
            raise SystemExit("%s: timeout waiting for DONE" % mode)
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    if stalls:
        s = sorted(stalls)
        print("    main loop held %d times at 30 Hz: median %.0f ms, max %.0f ms"
              % (len(s), 1000 * s[len(s) // 2], 1000 * s[-1]))
    return open(log, "rb").read().decode("latin-1"), open(plog, "rb").read().decode("latin-1")


def left_channel(path):
    data = open(path, "rb").read()[44:]
    n = len(data) // 4
    return struct.unpack("<%dh" % (2 * n), data[:4 * n])[0::2]


def analyse(y):
    """Clicks in the steady part of a pure tone: residual events of the
    least-squares sine recurrence, and silent runs inside the tone."""
    loud = [i for i in range(0, len(y), 48) if abs(y[i]) > 1000]
    if not loud:
        return None
    a = loud[0] + int(0.3 * RATE)
    b = loud[-1] - int(0.1 * RATE)
    if b - a < RATE:
        return None
    seg = y[a:b]
    num = den = 0.0
    for n in range(2, len(seg)):
        num += seg[n - 1] * (seg[n] + seg[n - 2])
        den += seg[n - 1] * seg[n - 1]
    c = num / den
    hz = math.acos(max(-1.0, min(1.0, c / 2))) * RATE / (2 * math.pi)
    amp = math.sqrt(2 * sum(v * v for v in seg) / len(seg))
    thr = 0.08 * amp
    events, last, peak = [], -10 ** 9, 0.0
    for n in range(2, len(seg)):
        r = abs(seg[n] - c * seg[n - 1] + seg[n - 2])
        if r > thr:
            if n - last > 96:
                events.append(n)
            last = n
        elif n - last > 96:
            peak = max(peak, r)
    gaps, run = [], 0
    for n, v in enumerate(seg):
        if abs(v) < 0.02 * amp:
            run += 1
        else:
            if run > 48:
                gaps.append((n - run, run))
            run = 0
    return {
        # Samples at full scale: a sum that did not fit, saturated. The
        # sine recurrence does not see it (a clipped sine is smooth), so it
        # is counted on its own.
        "clipped": sum(1 for v in seg if v >= 32767 or v <= -32768),
        "start": a / RATE, "secs": len(seg) / RATE, "hz": hz, "amp": amp,
        "noise": peak / amp, "events": [(a + n) / RATE for n in events],
        "gaps": [((a + n) / RATE, k * 1000 / RATE) for n, k in gaps],
    }


def main():
    modes = [a for a in sys.argv[1:] if not a.startswith("-")] or ["sb16"]
    for m in modes:
        if m not in ("sb16", "opl", "cd", "cd+sb16"):
            raise SystemExit("usage: tools/audio-glitch-test.py [sb16] [opl] [cd] [cd+sb16]")
    period = int(os.environ.get("PERIOD", "1024"))
    secs = float(os.environ.get("SECS", "20"))
    margin = int(os.environ.get("MARGIN", "441"))
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    failed = []
    for mode in modes:              # one TCG guest at a time
        img = build(mode, secs, margin)
        tap = os.path.join(OUT, "%s.wav" % mode)
        log = os.path.join(OUT, "%s-serial.log" % mode)
        plog = os.path.join(OUT, "%s-player.log" % mode)
        text, ptext = run(mode, img, tap, log, plog, period)
        print("%s: DAC period %d frames%s" % (mode, period, ", -icount" if os.environ.get("ICOUNT") else ""))
        for line in text.splitlines():
            if line.strip() and line.strip() not in ("START", "DONE"):
                print("    guest:", line.strip())
        for line in ptext.splitlines():
            if "audio" in line:
                print("    player:", line.strip())
        ok = True
        if "NODSP" in text:
            print("FAIL %s: no DSP answered the reset" % mode)
            ok = False
        if "cd" in mode and "CDPLAY" not in text:
            print("FAIL %s: the drive did not start playing" % mode)
            ok = False
        if "sb16" in mode:
            late = int(text.split("late ")[1].split()[0], 16) if "late " in text else None
            if late is None:
                print("FAIL sb16: the guest never reported")
                ok = False
            elif late:
                print("FAIL sb16: the card read past the guest's write position %d times" % late)
                ok = False
        r = analyse(left_channel(tap)) if os.path.exists(tap) else None
        if not r:
            print("FAIL %s: no tone in %s" % (mode, tap))
            failed.append(mode)
            continue
        print("    tap: %.1f s of %.1f Hz from %.1f s, noise floor %.1f%% of the amplitude"
              % (r["secs"], r["hz"], r["start"], 100 * r["noise"]))
        if r["gaps"]:
            print("    %d silent gaps: %s" % (len(r["gaps"]), ", ".join(
                "%.2f s (%.0f ms)" % g for g in r["gaps"][:12])))
        if mode == "cd" and os.environ.get("CDVOL"):
            reg = int(os.environ["CDVOL"], 0)
            want = ((reg >> 3) - 31) * 2
            got = 20 * math.log10(r["amp"] / int(os.environ.get("CDAMP", "16000")))
            print("    CD volume 0x%02x: %.1f dB, the register says %d dB" % (reg, got, want))
            if abs(got - want) > 1.0:
                print("FAIL cd: the card's CD volume did not reach the drive's audio")
                ok = False
        if r["clipped"]:
            print("FAIL %s: %d samples clipped at full scale (the mix summed past it)"
                  % (mode, r["clipped"]))
            ok = False
        if r["events"]:
            print("FAIL %s: %d clicks in the tone, at %s%s" % (mode, len(r["events"]),
                  ", ".join("%.2f" % t for t in r["events"][:16]),
                  " ..." if len(r["events"]) > 16 else ""))
            ok = False
        else:
            print("PASS %s: no clicks in %.1f s" % (mode, r["secs"]))
        if not ok:
            failed.append(mode)
    if failed:
        raise SystemExit("audio-glitch-test: %s failed" % ", ".join(failed))
    print("audio-glitch-test: %s passed" % ", ".join(modes))


if __name__ == "__main__":
    main()
