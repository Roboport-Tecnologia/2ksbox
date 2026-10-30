#!/usr/bin/env python3
"""The music devices as a *guest* meets them (doc 20 §7, M12): a DOS
program detects the OPL3 the way an AdLib driver does, plays a 440 Hz
note on it, then resets an MPU-401, puts it in UART mode and plays A4
through it. The evidence is the **wav QEMU's own audio backend
recorded**, not the program's own opinion.

Two boots, one per device, because each is checked by the same question
("is there a 440 Hz note in this file?") and one file with two notes in
it cannot answer it twice. The serial log says what the guest saw at the
ports; `synthx wavtone` says whether anything came out.

    tools/midi-guest-test.py            # needs nasm, mtools, build/qemu
    tools/midi-guest-test.py opl        # one of them only
    tools/midi-guest-test.py mpu

Outputs in build/midi-guest/. Local only (it fetches the FreeDOS floppy
`tools/x87-guest-test.py` uses). It is also the `midi-guest` check in
scripts/test.sh's guest stage. The host stage's `music` check proves the
same devices from the monitor in seconds; this proves the half that check
cannot, that a *guest* driving them through its own I/O instructions
finds them.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import qemuhost  # tools/qemuhost.py: the platform's QEMU and QMP address
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = qemuhost.qemu(ROOT)   # this checkout's build, or $QEMU_BIN
SYNTHX = qemuhost.rust_bin(ROOT, "synthx")
SOUNDFONT = os.path.join(ROOT, "soundfonts/TimGM6mb.sf2")
OUT = os.path.join(ROOT, "build/midi-guest")

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

# The note both devices play: A4, the one frequency this whole file is
# about. On the OPL that is fnum 580 at block 4 (440 = fnum * 49716 /
# 2^16); on the MIDI port it is key 69.
NOTE_HZ = 440

ASM = r"""
org 100h
bits 16

start:
%ifdef OPL
    call opl_detect
    call opl_note
%else
    call mpu_open
    call mpu_note
%endif
    mov si, str_done
    call puts
    ; FreeDOS runs this from AUTOEXEC and the runner kills the VM when
    ; DONE arrives; exiting cleanly anyway keeps a by-hand run usable.
    mov ax, 4C00h
    int 21h

; ---------------------------------------------------------------- OPL3

; The sequence every AdLib-aware game runs before it will play a note:
; mask both timer flags and reset them (status must read 0), start timer
; 1 with a preset that expires almost at once, wait, and read the flag
; and the IRQ bit back (0xC0). A chip whose timers do not move is a chip
; no game finds, however well it synthesizes.
opl_detect:
    mov si, str_detect
    call puts
    mov al, 04h
    mov ah, 60h
    call opl_write
    mov al, 04h
    mov ah, 80h
    call opl_write
    call opl_status
    call put_hex8
    mov al, ' '
    call putc
    mov al, 02h
    mov ah, 0FFh
    call opl_write
    mov al, 04h
    mov ah, 21h
    call opl_write
    mov cx, 2                   ; ~110 ms: the timer wants 80 us
    call delay_ticks
    call opl_status
    push ax
    call put_hex8
    mov al, ' '
    call putc
    mov al, 04h
    mov ah, 60h
    call opl_write
    mov al, 04h
    mov ah, 80h
    call opl_write
    call opl_status
    call put_hex8
    call newline
    pop ax
    and al, 0C0h
    cmp al, 0C0h
    mov si, str_fail
    jne .say
    mov si, str_pass
.say:
    call puts
    ret

; One channel, two operators, additive, both outputs, key on at 440 Hz;
; held long enough that the note is unmissable in the recording, then
; released.
opl_note:
    mov al, 05h                 ; the OPL3 register file: stereo, and
    mov ah, 01h                 ; the four-operator modes
    call opl_write3
    mov al, 01h
    mov ah, 20h
    call opl_write
    mov bl, 0                   ; operator 0, then operator 3
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
    mov ah, 44h                 ; fnum 580, low byte
    call opl_write
    mov al, 0B0h
    mov ah, 32h                 ; key on, block 4, fnum high bits
    call opl_write
    mov si, str_playing
    call puts
    mov cx, 36                  ; ~2 s
    call delay_ticks
    mov al, 0B0h
    mov ah, 12h                 ; key off
    call opl_write
    mov cx, 9
    call delay_ticks
    ret

opl_write:                      ; al = register, ah = value (bank 0)
    push dx
    mov dx, 388h
    jmp opl_write_at
opl_write3:                     ; the same in the OPL3 register file
    push dx
    mov dx, 38Ah
opl_write_at:
    push cx
    push ax
    out dx, al
    mov cx, 6                   ; the address delay a real chip needs
.a: in al, dx
    loop .a
    pop ax
    push ax
    mov al, ah
    inc dx
    out dx, al
    dec dx
    mov cx, 35                  ; and the data delay
.d: in al, dx
    loop .d
    pop ax
    pop cx
    pop dx
    ret

opl_status:                     ; al = the status register
    push dx
    mov dx, 388h
    in al, dx
    pop dx
    ret

; ------------------------------------------------------------- MPU-401

; How a driver finds the port: reset it, and read the 0xFE back. Then
; UART mode, which is the only mode anything of this era uses.
mpu_open:
    mov si, str_reset
    call puts
    mov al, 0FFh
    call mpu_cmd
    call mpu_read
    call put_hex8
    push ax
    mov al, ' '
    call putc
    mov al, 3Fh                 ; UART mode
    call mpu_cmd
    call mpu_read
    call put_hex8
    push ax                     ; newline is a putc: it lands in al
    call newline
    pop ax
    cmp al, 0FEh
    pop bx                      ; the reset's ACK, from above
    jne .bad
    cmp bl, 0FEh
    jne .bad
    mov si, str_pass
    jmp puts
.bad:
    mov si, str_fail
    jmp puts

mpu_note:
    mov al, 0C0h                ; program change, channel 1
    call mpu_data
    mov al, 00h                 ; acoustic piano
    call mpu_data
    mov al, 90h                 ; note on
    call mpu_data
    mov al, 45h                 ; key 69 = A4 = 440 Hz
    call mpu_data
    mov al, 64h
    call mpu_data
    mov si, str_playing
    call puts
    mov cx, 36                  ; ~2 s
    call delay_ticks
    mov al, 45h                 ; running status: the note off
    call mpu_data
    mov al, 00h
    call mpu_data
    mov cx, 18
    call delay_ticks
    ret

mpu_cmd:                        ; al -> the command port
    push dx
    push ax
    mov dx, 331h
    out dx, al
    pop ax
    pop dx
    ret

mpu_data:                       ; al -> the data port, once it will take it
    push dx
    push cx
    push ax
    mov dx, 331h
    mov cx, 0FFFFh
.w: in al, dx
    test al, 40h                ; clear = it will take a byte
    jz .go
    loop .w
.go:
    pop ax
    push ax
    mov dx, 330h
    out dx, al
    pop ax
    pop cx
    pop dx
    ret

mpu_read:                       ; al = a byte from the port, 0 if none came
    push dx
    push cx
    mov dx, 331h
    mov cx, 0FFFFh
.w: in al, dx
    test al, 80h                ; clear = a byte is waiting
    jz .got
    loop .w
    xor al, al
    jmp .out
.got:
    mov dx, 330h
    in al, dx
.out:
    pop cx
    pop dx
    ret

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

putc:                           ; al -> COM1, poll THR empty
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

puts:                           ; si = asciiz
    lodsb
    test al, al
    jz .d
    call putc
    jmp puts
.d: ret

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

newline:
    mov al, 10
    jmp putc

str_detect:  db "OPL status ", 0
str_reset:   db "MPU reset/uart ", 0
str_playing: db "PLAYING", 10, 0
str_pass:    db "PASS", 10, 0
str_fail:    db "FAIL", 10, 0
str_done:    db "DONE", 10, 0
"""


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build(mode):
    """One .COM and one floppy that runs it, for `opl` or `mpu`."""
    asm = os.path.join(OUT, "midi-%s.asm" % mode)
    com = os.path.join(OUT, "MIDITEST.COM")
    img = os.path.join(OUT, "midi-%s.img" % mode)
    with open(asm, "w") as f:
        f.write(ASM)
    sh("nasm", "-O0", "-f", "bin", *(["-DOPL"] if mode == "opl" else []), "-o", com, asm)
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nMIDITEST.COM\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::MIDITEST.COM")
    return img


def device_args(mode, wav):
    """The machine under test: one device, and QEMU's own wav backend as
    the only listener, the same `-audiodev wav` the CD-DA tools record
    through."""
    args = ["-audiodev", "wav,id=w,path=" + wav]
    if mode == "opl":
        # With the Sound Blaster mirror, since that is how a DOS machine
        # is put together (`bundle::Sound::Sb16`); the program writes to
        # 0x388 either way.
        args += ["-device", "opl3,audiodev=w,sbbase=0x220"]
    else:
        args += ["-device", "mpu401,audiodev=w,synth=gm,soundfont=" + SOUNDFONT]
    return args


def run(mode, img, wav, log):
    for f in (wav, log):
        if os.path.exists(f):
            os.unlink(f)
    p = subprocess.Popen([
        QEMU, "-machine", "pc", "-cpu", "pentium3", "-m", "64",
        "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
        # `format=raw`: a floppy image has no header to probe, and QEMU
        # otherwise warns and restricts writes to the boot sector.
        "-drive", "file=%s,if=floppy,index=0,format=raw" % img,
        "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
        *device_args(mode, wav),
    ])
    t0 = time.time()
    try:
        while time.time() - t0 < 300:
            time.sleep(1)
            if p.poll() is not None:
                break
            if os.path.exists(log) and b"DONE" in open(log, "rb").read():
                break
        else:
            raise SystemExit("%s: timeout waiting for DONE" % mode)
    finally:
        if p.poll() is None:
            # The note has been released and the tail rendered by now;
            # what matters is that the wav backend has flushed it, which
            # it does on close.
            p.terminate()
            p.wait()
    return open(log, "rb").read().decode("latin-1")


def check(mode):
    img = build(mode)
    wav = os.path.join(OUT, "%s.wav" % mode)
    log = os.path.join(OUT, "%s-serial.log" % mode)
    t0 = time.time()
    text = run(mode, img, wav, log)
    print("%s: %.0f s in the guest" % (mode, time.time() - t0))
    for line in text.splitlines():
        if line.strip():
            print("   ", line.strip())
    ok = True
    # What the guest saw at the ports. The OPL's is the detection
    # sequence's three status reads; the MPU's is the two ACKs.
    if "PASS" not in text:
        print("FAIL %-5s the guest did not find the device at its ports" % mode)
        ok = False
    if "PLAYING" not in text:
        print("FAIL %-5s the guest never got as far as the note" % mode)
        ok = False
    # And what came out, which is the half no register read can answer.
    if not os.path.exists(SYNTHX):
        print("SKIP %-5s no target/release/synthx to measure %s" % (mode, wav))
        return ok
    sys.stdout.flush()          # the child writes the verdict line
    rc = subprocess.run([SYNTHX, "wavtone", wav, str(NOTE_HZ)]).returncode
    return ok and rc == 0


def main():
    modes = [a for a in sys.argv[1:] if not a.startswith("-")] or ["opl", "mpu"]
    for m in modes:
        if m not in ("opl", "mpu"):
            raise SystemExit("usage: tools/midi-guest-test.py [opl] [mpu]")
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(SOUNDFONT):
        raise SystemExit("%s missing (it is checked in)" % SOUNDFONT)
    # One TCG guest at a time, deliberately: two starve each other and a
    # run that is merely slow reads as a failure.
    failed = [m for m in modes if not check(m)]
    if failed:
        raise SystemExit("midi-guest-test: %s failed" % ", ".join(failed))
    print("midi-guest-test: %s passed" % ", ".join(modes))


if __name__ == "__main__":
    main()
