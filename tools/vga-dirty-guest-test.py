#!/usr/bin/env python3
"""Does the display see what the guest wrote to video memory?

A DOS program sets a VGA mode and fills video memory **one 4 KiB page at
a time, each page with its own pixel value**, so a screendump answers the
question page by page: which of them reached the display and which are
still showing something else.  Nothing here is a game, a driver or the
player (it is `qemu-system-i386`, a floppy, and QMP), so a failure is
QEMU's and a pass sends the question elsewhere.

Why this shape.  A guest reaches video memory two ways, and only one of
them is what a game uses:

  * through the VGA **MMIO ops** (`vga_mem_write`), which mark the region
    dirty themselves.  Planar and odd/even modes go here.
  * as **plain RAM** (the Cirrus maps its 0xA0000 window as a RAM alias
    whenever the mode allows, `map_linear_vram`, and every VESA linear
    frame buffer is RAM), where the *only* thing that marks it dirty is
    the softmmu's `TLB_NOTDIRTY` -> `notdirty_write` path.

Duke Nukem 3D was clean at 320x200 on `-vga std` and wrong at 320x200 on
`-vga cirrus`, and wrong in its VESA modes on both. That is exactly the
split above (the one working case is the one that never touches
`notdirty_write`), so this asks the question with no game in the way.

Two fills, and the gap between them is half the point:

  * fill 1 writes page p with value p, then the program waits ~2 s, which
    is many refreshes: QEMU snapshots the VGA dirty bitmap and clears it,
    and `tlb_reset_dirty_range_all` is supposed to put `TLB_NOTDIRTY`
    back on every TLB entry for that memory.
  * fill 2 writes page p with value p+32.  **If fill 2 is invisible and
    fill 1 is not, the tracking works once and then stops**, which is
    what "only the first 15 % of the screen updates" looks like from the
    inside.

And each fill writes even-numbered pages with `rep stosw` and odd ones
with a byte loop, so one screendump also says whether patch 17's REP fast
path is the difference: that would come out striped, every other page.

The palette is programmed by the guest to a grey ramp (entry i -> level
2i) so a screendump's RGB reads back as the pixel value with no table to
trust.  It is set *before* the fills and never touched again, because a
palette write forces a full redraw and would hide the very thing this
looks for.

    tools/vga-dirty-guest-test.py                 # std and cirrus
    tools/vga-dirty-guest-test.py cirrus          # one adapter
    QEMU_EXTRA='-accel tcg,...' tools/vga-dirty-guest-test.py

`VBEPAL=1` sets the palette through the VGA BIOS (VBE 4F09h) instead of
the DAC ports, as DOS Quake and Build do in a VESA mode, and also checks
one entry's channel order through the ports and a 4F09h get.
`VBEPAL=1 ... vesa` is the `vbe-palette` check in scripts/test.sh (the
VGA BIOS of patches/seabios); the rest is local only.

Needs nasm, mtools and build/qemu (it reuses the FreeDOS floppy
`tools/x87-guest-test.py` fetches).  Outputs in build/vga-dirty/.
"""
import importlib.util
import json
import os
import shlex
import socket
import shutil
import subprocess
import sys
import qemuhost  # tools/qemuhost.py: the platform's QEMU and QMP address
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = qemuhost.qemu(ROOT)   # this checkout's build, or $QEMU_BIN
OUT = os.path.join(ROOT, "build/vga-dirty")

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)

# Mode 13h: 320x200x8 in one 64 KiB segment at 0xA0000, which is 16 pages
# of 4 KiB.  The last one is only 2560 bytes on screen (the mode is 64000
# bytes), so page 15 is checked over the rows it does cover.
PAGE = 4096
# fill 1 writes page p with (p mod 32), fill 2 with (p mod 32) + 32; the
# guest programs 64 DAC entries (entry i -> level i), so a screendump
# pixel reads back as 4i and the value is byte / 4.
BASE2 = 32
# The two modes this asks about, and the reason for each.  13h is what a
# DOS game means by 320x200 and is where the report started; VESA 0x101
# is the banked 640x480 the same games use for "SVGA", which reaches
# video memory through a 64 KiB window that has to be moved as the fill
# walks the frame buffer.
MODES = {
    "13h": dict(mode=None, width=320, height=200),
    "vesa": dict(mode=0x101, width=640, height=480),
}
# --refill: keep rewriting after fill 2, to tell a dropped update from
# memory the display has stopped watching.
REFILL = False

ASM = r"""
org 100h
bits 16

start:
%ifdef VESA
    ; What this VBE implementation says it is, before anything is asked
    ; of it: the signature it returns, its version, and whether the two
    ; palette-related functions are there at all.
    push es
    mov ax, cs
    mov es, ax
    mov di, ctrlinfo
    mov dword [es:di], "VBE2"
    mov ax, 4F00h
    int 10h
    pop es
    push ax
    mov si, msg_vbe
    call puts
    pop ax
    call put_hex16              ; 004f = supported
    mov si, msg_sig
    call puts
    mov al, [ctrlinfo + 0]
    call putc
    mov al, [ctrlinfo + 1]
    call putc
    mov al, [ctrlinfo + 2]
    call putc
    mov al, [ctrlinfo + 3]
    call putc
    mov si, msg_ver
    call puts
    mov ax, [ctrlinfo + 4]      ; VbeVersion, bcd-ish (0x0200 = 2.0)
    call put_hex16
    mov si, msg_dacw
    call puts
    mov ax, 4F08h               ; DAC palette width control
    mov bl, 1                   ; get current width
    int 10h
    call put_hex16
    mov si, msg_crlf
    call puts

    ; VBE mode info first: the window granularity and segment are read,
    ; not assumed -- the two adapters need not agree on them.
    push es
    mov ax, cs
    mov es, ax
    mov di, modeinfo
    mov ax, 4F01h
    mov cx, VESAMODE
    int 10h
    pop es
    cmp ax, 004Fh
    jne .nomode
    mov ax, 4F02h               ; banked, no linear frame buffer
    mov bx, VESAMODE
    int 10h
    cmp ax, 004Fh
    jne .nomode
    ; say what the adapter actually offers, rather than assuming it
    mov si, msg_gran
    call puts
    mov ax, [modeinfo + 4]      ; WinAGranularity, KiB
    call put_hex16
    mov si, msg_winsize
    call puts
    mov ax, [modeinfo + 6]      ; WinSize, KiB
    call put_hex16
    mov si, msg_bpl
    call puts
    mov ax, [modeinfo + 16]     ; BytesPerScanLine
    call put_hex16
    ; How many 4 KiB pages one granule holds. The window number handed to
    ; 4F05h counts *granules*, not windows, and the two adapters do not
    ; agree on the size of one: 64 KiB on the standard VGA, 16 KiB on the
    ; Cirrus. Assuming either one puts the fill in the wrong place on the
    ; other, which the guest's own read-back then reports as damage.
%ifdef GRAN64
    ; Deliberately wrong: assume a 64 KiB granule instead of asking. This
    ; is what a program that takes the window *size* for the granularity
    ; does, and it is the one thing the two adapters disagree about --
    ; the standard VGA reports 64 KiB, the Cirrus 16 KiB. On the Cirrus
    ; every bank then lands at a quarter of the offset meant, and the
    ; picture comes out in blocks that have swapped places.
    mov byte [pages_per_gran], 16
%else
    mov ax, [modeinfo + 4]      ; WinAGranularity, KiB
    shr ax, 2                   ; KiB / 4 = pages of 4 KiB per granule
    mov [pages_per_gran], al
%endif
    mov si, msg_crlf
    call puts
%else
    mov ax, 0013h               ; 320x200x8, the mode every DOS game knows
    int 10h
%endif
    call set_dac                ; a palette we can read back, before any fill

    xor bl, bl
    xor bh, bh
    call fill                   ; page p <- p mod 32
    mov si, msg_fill1
    call puts
    call wait_2s                ; many refreshes: the dirty bitmap is
                                ; snapshotted and cleared, and TLB_NOTDIRTY
                                ; is supposed to come back
    xor bl, bl
    mov bh, 32
    call fill                   ; page p <- (p mod 32) + 32
    mov si, msg_fill2
    call puts
    call verify                 ; what the guest itself reads back
%ifdef REFILL
    ; Keep writing the same values for as long as the run lasts. Every
    ; page is rewritten thousands of times after this point, so a page
    ; still showing fill 1 at the end is not a dropped update -- it is
    ; memory the display has stopped watching altogether.
.again:
    xor bl, bl
    mov bh, 32
    call fill
    jmp .again
%else
.hang:
    hlt
    jmp .hang

%ifdef VESA
.nomode:
    mov si, msg_nomode
    call puts
    jmp .hang
%endif
%endif

; --------------------------------------------------------------- fill

; Fill all 16 pages of A000:0000, page p with bl + p.  Even pages go
; through `rep stosw` (patch 17's fast path), odd pages through a byte
; loop, so a screendump says whether the two differ.
fill:
    push es
%ifdef VESA
    mov ax, [modeinfo + 8]      ; WinASegment
    mov es, ax
    xor si, si                  ; window currently mapped
    push bx
    mov ax, 4F05h
    xor bx, bx
    xor dx, dx
    int 10h
    pop bx
%else
    mov ax, 0A000h
    mov es, ax
%endif
    xor di, di
    xor dl, dl                  ; page number
.page:
%ifdef VESA
    call window_for_page        ; di = offset in the window, si = window
%endif
    mov al, bl
    add al, dl
    and al, 31
    add al, bh                  ; bh = 0 for fill 1, 32 for fill 2
    mov ah, al
    test dl, 1
%ifdef FLIP
    jz .slow
%else
    jnz .slow
%endif
    mov cx, PAGE / 2
    cld
    rep stosw
    jmp .next
.slow:
    mov cx, PAGE
.byte_loop:
    mov [es:di], al
    inc di
    dec cx
    jnz .byte_loop
.next:
    inc dl
    cmp dl, PAGES
    jb .page
    pop es
    ret

%ifdef VESA
; Page dl of the frame buffer -> the window holding it (mapped if it is
; not the one already there) and di, its offset inside that window.
; [pages_per_gran] pages of 4 KiB fit in one granule, which is the unit
; 4F05h counts in.
window_for_page:
    push ax
    push bx
    push cx
    push dx
    mov al, dl
    xor ah, ah
    xor cx, cx
    mov cl, [pages_per_gran]
    div cl                      ; al = granule, ah = page within it
    mov ch, ah                  ; keep the remainder
    xor bh, bh
    mov bl, al
    cmp bx, si
    je .have
    mov si, bx
    push si
    mov ax, 4F05h
    xor bx, bx
    mov dx, si
    int 10h
    pop si
.have:
    mov al, ch
    xor ah, ah
    mov cl, 12
    shl ax, cl                  ; page within window * 4096
    mov di, ax
    pop dx
    pop cx
    pop bx
    pop ax
    ret
%endif

; -------------------------------------------------------------- verify

; Read back one byte from every page and count the ones that do not hold
; what fill 2 wrote. This is the guest's own opinion of video memory,
; which settles whether a page missing from the display was never
; written or was written and not shown.
verify:
    push es
%ifdef VESA
    mov ax, [modeinfo + 8]
    mov es, ax
    xor si, si
    mov ax, 4F05h
    xor bx, bx
    xor dx, dx
    int 10h
%else
    mov ax, 0A000h
    mov es, ax
%endif
    xor di, di
    xor dl, dl
    xor bp, bp                  ; mismatches
.page:
%ifdef VESA
    call window_for_page
%endif
    mov al, dl
    and al, 31
    add al, 32                  ; what fill 2 wrote
    cmp al, [es:di]
    je .ok
    inc bp
.ok:
    inc dl
    cmp dl, PAGES
    jb .page
    pop es
    mov si, msg_verify
    call puts
    mov ax, bp
    call put_hex16
    mov si, msg_crlf
    call puts
    ret

; ---------------------------------------------------------------- DAC

; Entry i (0..31) -> grey level 2i.  QEMU scales the 6-bit DAC to 8 bits,
; so a screendump pixel reads back as (2i) << 2 = 8i in all three
; channels, and the pixel value is (byte / 8).
set_dac:
%ifdef VBEPAL
    ; The palette through the VBE BIOS (4F09h) rather than the DAC ports:
    ; what a VESA game of the era actually calls, and a different path
    ; through the adapter. The ramp is grey, so the byte order inside an
    ; entry (blue/green/red vs red/green/blue) cannot change the answer --
    ; so one more entry, 64, which no page shows, has three different
    ; channels, and is read back twice below.
    push es
    mov ax, cs
    mov es, ax
    mov di, palbuf
    xor cx, cx
.build:
    mov al, cl
    mov [es:di], al             ; blue
    mov [es:di + 1], al         ; green
    mov [es:di + 2], al         ; red
    mov byte [es:di + 3], 0     ; alignment
    add di, 4
    inc cx
    cmp cx, 64
    jb .build
    mov byte [es:di], 11h       ; entry 64: blue 11h, green 22h, red 33h
    mov byte [es:di + 1], 22h
    mov byte [es:di + 2], 33h
    mov byte [es:di + 3], 0
    mov ax, 4F09h
    xor bl, bl                  ; set palette data
    mov cx, 65
    xor dx, dx                  ; from entry 0
    mov di, palbuf
    int 10h
    push ax
    mov si, msg_pal
    call puts
    pop ax
    call put_hex16
    ; Entry 64 as the DAC holds it, through the ports (red, green, blue):
    ; "33 22 11" is the BIOS having read the table in DOS Quake's order.
    mov si, msg_dac64
    call puts
    mov dx, 3C7h
    mov al, 64
    out dx, al
    mov dx, 3C9h
    in al, dx
    call put_hex8
    in al, dx
    call put_hex8
    in al, dx
    call put_hex8
    ; And through the BIOS again (4F09h BL=01h), back in table order.
    mov ax, 4F09h
    mov bl, 1                   ; get palette data
    mov cx, 1
    mov dx, 64
    mov di, palget
    int 10h
    push ax
    mov si, msg_get
    call puts
    pop ax
    call put_hex16
    mov al, ' '
    call putc
    mov al, [palget]
    call put_hex8
    mov al, [palget + 1]
    call put_hex8
    mov al, [palget + 2]
    call put_hex8
    mov al, [palget + 3]
    call put_hex8
    mov si, msg_crlf
    call puts
    pop es
    ret
%endif
    mov dx, 3C8h
    xor al, al
    out dx, al
    mov dx, 3C9h
    xor cx, cx
.entry:
    mov al, cl
    out dx, al
    out dx, al
    out dx, al
    inc cx
    cmp cx, 64
    jb .entry
    ret

; --------------------------------------------------------------- wait

; ~2 s on the BIOS tick counter at 0040:006C (18.2 Hz).  Not a busy spin
; on the clock alone: `hlt` keeps the vCPU out of the way of the refresh
; this is waiting for.
wait_2s:
    push ds
    mov ax, 40h
    mov ds, ax
    mov bx, [6Ch]
.wait:
    hlt
    mov ax, [6Ch]
    sub ax, bx
    cmp ax, 36
    jb .wait
    pop ds
    ret

; -------------------------------------------------------------- serial

putc:                           ; al -> COM1 (0x3F8), poll THR empty
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

put_hex16:                      ; ax -> four hex digits on COM1
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

puts:                           ; si = asciiz
    lodsb
    test al, al
    jz .d
    call putc
    jmp puts
.d: ret

msg_vbe:     db "VBE00=", 0
msg_sig:     db " SIG=", 0
msg_ver:     db " VER=", 0
msg_dacw:    db " DACWIDTH(4F08)=", 0
msg_pal:     db "VBE_SETPAL=", 0
msg_dac64:   db " DAC64=", 0
msg_get:     db " VBE_GETPAL=", 0
msg_verify:  db "VERIFY_BAD=", 0
msg_gran:    db "GRAN=", 0
msg_winsize: db " WINSIZE=", 0
msg_bpl:     db " BPL=", 0
msg_crlf:    db 13, 10, 0
msg_fill1: db "FILL1", 13, 10, 0
msg_fill2: db "FILL2", 13, 10, 0
%ifdef VESA
msg_nomode: db "NOMODE", 13, 10, 0
modeinfo:  times 256 db 0
pages_per_gran: db 16
ctrlinfo:  times 512 db 0
%ifdef VBEPAL
palbuf:    times 65 * 4 db 0
palget:    times 4 db 0
%endif
%endif
"""


class Qmp:
    """Just enough QMP to ask for a screendump."""

    def __init__(self, path, timeout=60):
        t0 = time.time()
        while True:
            try:
                self.s = qemuhost.connect(path)
                break
            except OSError:
                if time.time() - t0 > timeout:
                    raise
                time.sleep(0.1)
        self.f = self.s.makefile("rwb")
        self.f.readline()                       # greeting
        self.cmd("qmp_capabilities")

    def cmd(self, execute, **args):
        msg = {"execute": execute}
        if args:
            msg["arguments"] = args
        self.f.write((json.dumps(msg) + "\n").encode())
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                raise SystemExit("QMP closed")
            reply = json.loads(line)
            if "event" in reply:
                continue
            if "error" in reply:
                raise SystemExit("QMP %s: %s" % (execute, reply["error"]))
            return reply.get("return")

    def screendump(self, path):
        if os.path.exists(path):
            os.unlink(path)
        self.cmd("screendump", filename=path)
        # the dump is written by the main loop; wait for the file to land
        for _ in range(600):
            if os.path.exists(path) and os.path.getsize(path) > 0:
                return path
            time.sleep(0.05)
        raise SystemExit("screendump never appeared at " + path)


def c6_to_8(v):
    """QEMU's own 6-bit DAC level -> 8-bit channel (hw/display/vga.c)."""
    v &= 0x3f
    b = v & 1
    return (v << 2) | (b << 1) | b


def read_ppm(path):
    """A binary PPM as (width, height, bytes). QEMU writes P6, maxval 255."""
    with open(path, "rb") as f:
        data = f.read()
    fields, i = [], 0
    while len(fields) < 4:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while i < len(data) and data[i] != 0x0A:
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(data[i:j])
        i = j
    i += 1
    magic, w, h = fields[0], int(fields[1]), int(fields[2])
    if magic != b"P6":
        raise SystemExit("%s: not a binary PPM (%r)" % (path, magic))
    return w, h, data[i : i + w * h * 3]


def page_values(path, geom):
    WIDTH, HEIGHT, PAGES = geom["width"], geom["height"], geom["pages"]
    """The pixel value seen for each of the 16 pages, as a histogram.

    The guest's palette makes a pixel value v read back as 8v in every
    channel, so this inverts that and counts what each page's rows hold.
    A page is `None` where the mode does not reach it.
    """
    w, h, px = read_ppm(path)
    out = []
    for p in range(PAGES):
        counts = {}
        for off in range(p * PAGE, min((p + 1) * PAGE, WIDTH * HEIGHT)):
            y, x = divmod(off, WIDTH)
            sy, sx = y * h // HEIGHT, x * w // WIDTH
            i = (sy * w + sx) * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            # QEMU expands the 6-bit DAC to 8 bits with c6_to_8(): the
            # low bit is replicated into the bottom two, so level 33
            # comes back as 135 and not 132. Invert that rather than
            # assuming a plain 4x, or every odd palette entry reads as
            # a mismatch that is really this arithmetic.
            v = r >> 2
            v = v if r == g == b and c6_to_8(v) == r else -1
            counts[v] = counts.get(v, 0) + 1
        out.append(counts if counts else None)
    return out


def verdict(counts, want):
    """(ok, description) for one page's histogram against the wanted value."""
    if counts is None:
        return True, "off screen"
    total = sum(counts.values())
    hit = counts.get(want, 0)
    if hit == total:
        return True, "%d" % want
    top = sorted(counts.items(), key=lambda kv: -kv[1])[:2]
    saw = ", ".join("%s x%d" % ("?" if v < 0 else v, n) for v, n in top)
    return False, "%d%% right (saw %s)" % (100 * hit // total, saw)


def run(tag, adapter, img, extra):
    d = os.path.join(OUT, tag)
    os.makedirs(d, exist_ok=True)
    log = os.path.join(d, "serial.log")
    qmp_sock = qemuhost.addr(d)
    for f in (log, qmp_sock):
        if os.path.exists(f):
            os.unlink(f)
    argv = [
        QEMU, "-machine", "pc", "-m", "64",
        "-L", os.path.join(ROOT, "qemu/pc-bios"),
    ] + (["-vga", "none", "-device", "d3dpt-vga"] if adapter == "d3dpt" else ["-vga", adapter]) + [
        "-display", "none", "-net", "none", "-audiodev", "none,id=a",
        "-fda", img, "-boot", "a",
        "-serial", "file:" + log, "-monitor", "none",
        "-qmp", qemuhost.qemu_opt(qmp_sock),
    ] + extra
    print("  $ %s" % " ".join(shlex.quote(a) for a in argv))
    p = subprocess.Popen(argv)
    try:
        q = Qmp(qmp_sock)
        # Fill 1 is up: dump before the guest writes anything else.
        wait_for(p, log, b"FILL1", 300)
        first = q.screendump(os.path.join(d, "fill1.ppm"))
        # Then keep asking for the surface until fill 2 is done. This is
        # the part that matters: every screendump snapshots and *clears*
        # the VGA dirty bitmap, which is what any real display listener
        # does on its refresh timer (the player's, a VNC client's). With `-display none` and nobody asking, the bits
        # simply pile up until the one dump at the end consumes them all
        # and everything looks fine, which is why this has to poll.
        scratch = os.path.join(d, "poll.ppm")
        t0 = time.time()
        while time.time() - t0 < 300:
            q.screendump(scratch)
            if os.path.exists(log) and b"FILL2" in open(log, "rb").read():
                break
            if p.poll() is not None:
                raise SystemExit("QEMU exited before FILL2")
            time.sleep(0.05)
        else:
            raise SystemExit("timeout waiting for FILL2")
        # A moment for the refresh that follows the second fill; the
        # screendump forces an update anyway, this only removes the
        # argument that it was too early.
        time.sleep(1.5)
        second = q.screendump(os.path.join(d, "fill2.ppm"))
    finally:
        if p.poll() is None:
            p.terminate()
            p.wait()
    return first, second


def wait_for(p, log, needle, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if p.poll() is not None:
            raise SystemExit("QEMU exited before %s" % needle.decode())
        if os.path.exists(log):
            with open(log, "rb") as f:
                if needle in f.read():
                    return
        time.sleep(0.2)
    raise SystemExit("timeout waiting for %s" % needle.decode())


def report(adapter, mode, geom, first, second):
    """Print both dumps page by page and return whether they are right."""
    pages = geom["pages"]
    ok = True
    for label, path, base in (("fill 1", first, 0), ("fill 2", second, BASE2)):
        vals = page_values(path, geom)
        rows, bad = [], 0
        for p in range(pages):
            good, what = verdict(vals[p], base + p % 32)
            rows.append("%3d%s %-20s" % (p, " " if good else "!", what))
            if not good:
                bad += 1
                ok = False
        print("  %s %s %s (%s):" % (adapter, mode, label, os.path.relpath(path, ROOT)))
        for i in range(0, min(pages, 16), 4):
            print("   " + "".join(rows[i : i + 4]))
        if pages > 16:
            print("    ... %d pages" % pages)
        if bad:
            evens = sum(1 for p in range(0, pages, 2)
                        if not verdict(vals[p], base + p % 32)[0])
            odds = bad - evens
            print("    %d of %d pages wrong (rep stosw %d, byte loop %d)"
                  % (bad, pages, evens, odds))
    return ok


def report_vbepal(adapter, tag):
    """VBEPAL=1: did the VBE BIOS take the palette, in the right order?

    The pages' greys already say the 64 grey entries arrived; this reads
    the guest's own line for entry 64 (blue 11h, green 22h, red 33h): the
    set's AX, the DAC as the ports give it back (red, green, blue), and a
    VBE get of the same entry (blue, green, red, alignment).
    """
    with open(os.path.join(OUT, tag, "serial.log"), "rb") as f:
        text = f.read().decode("ascii", "replace")
    line = next((l for l in text.splitlines() if l.startswith("VBE_SETPAL=")), "")
    want = "VBE_SETPAL=004f DAC64=332211 VBE_GETPAL=004f 11223300"
    ok = line.strip() == want
    print("  %s 4F09h: %s%s" % (adapter, line.strip() or "(no VBE_SETPAL line)",
                                "" if ok else "  -- wanted " + want))
    return ok


def build_floppy(geom):
    asm = os.path.join(OUT, "vgafill.asm")
    com = os.path.join(OUT, "VGAFILL.COM")
    defines = "%%define PAGE %d\n%%define BASE2 %d\n%%define PAGES %d\n" % (
        PAGE, BASE2, geom["pages"])
    if geom["mode"] is not None:
        defines += "%%define VESA 1\n%%define VESAMODE 0x%x\n" % geom["mode"]
    if REFILL:
        defines += "%define REFILL 1\n"
    if os.environ.get("FLIP"):
        defines += "%define FLIP 1\n"
    if os.environ.get("VBEPAL"):
        defines += "%define VBEPAL 1\n"
    if os.environ.get("GRAN64"):
        defines += "%define GRAN64 1\n"
    with open(asm, "w") as f:
        f.write(defines + ASM)
    x87gt.sh("nasm", "-O0", "-f", "bin", "-o", com, asm)

    img = os.path.join(OUT, "vgafill-%s.img" % geom["name"])
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nVGAFILL.COM\r\n")
    x87gt.sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    x87gt.sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    x87gt.sh("mcopy", "-o", "-i", img, com, "::VGAFILL.COM")
    return img


def main():
    global REFILL
    REFILL = "--refill" in sys.argv
    args = [a for a in sys.argv[1:] if not a.startswith("-")]
    modes = [a for a in args if a in MODES] or list(MODES)
    adapters = [a for a in args if a not in MODES] or ["std", "cirrus"]
    extra = shlex.split(os.environ.get("QEMU_EXTRA", ""))
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)

    failures = []
    for mode in modes:
        geom = dict(MODES[mode], name=mode)
        geom["pages"] = (geom["width"] * geom["height"] + PAGE - 1) // PAGE
        geom["pages_per_win"] = 65536 // PAGE
        img = build_floppy(geom)
        # One guest at a time: two TCG guests on one box starve each other.
        for adapter in adapters:
            print("== -vga %s, %s" % (adapter, mode))
            t0 = time.time()
            first, second = run("%s-%s" % (adapter, mode), adapter, img, extra)
            print("  (%.0f s)" % (time.time() - t0))
            if not report(adapter, mode, geom, first, second):
                failures.append("%s/%s" % (adapter, mode))
            if os.environ.get("VBEPAL") and geom["mode"] is not None:
                if not report_vbepal(adapter, "%s-%s" % (adapter, mode)):
                    failures.append("%s/%s 4F09h" % (adapter, mode))
    print()
    if failures:
        print("FAIL: the display did not see what the guest wrote on: %s"
              % ", ".join(failures))
        return 1
    print("PASS: every page of video memory reached the display (%s x %s)"
          % (", ".join(adapters), ", ".join(modes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
