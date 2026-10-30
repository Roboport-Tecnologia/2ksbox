#!/usr/bin/env python3
"""The Voodoo 2 device as a *guest* meets it (doc 21, M14), with no 3dfx
driver in the picture: a DOS program finds the card in PCI configuration
space, maps its 16 MiB BAR, checks the Voodoo 2 strap in initEnable, runs
the siProcess measurement 3dfx's Glide starts with (a PCI-clock countdown
that has to read zero, or glide2x.dll polls it for ever), runs
the init sequence 3dfx's own sst1init runs (frame-buffer geometry, video
timing, the DAC's PLL, the colour lookup table, VGA pass-through), fills
the back buffer with red through the linear frame buffer, reads a pixel
of it back through the same window (which makes the chip flush its FIFO:
a store that never landed reads back as something else), swaps, then
drives the **command FIFO** the way 3dfx's Glide does (packets written
into the ring's window and the chip told nothing) in two batches (a blue
fastfill and swap that has to follow a JMP from the ring's end back to its
start, then a magenta one after the read pointer was read), and gives the
monitor back to the VGA.

The FIFO half is what guards `ramfifo` (doc 21 §9): with it on (the
default) the window is RAM and the device finds the packets itself at the
guest's next access to the card, poisoning what the chip consumed before it
answers the read pointer. `RAMFIFO=off` runs the same program on the
per-dword MMIO path, the A/B (the `voodoo-guest-mmiofifo` check).

The last phase is the **dither subtraction** (doc 21 §9): a grey that
dithers in every channel over the whole screen, then blended onto itself
96 times per column at falling alpha, through the chip's own triangle
setup. A colour blended onto itself is that colour, so every column's
pixel must read the reference band's and the screendump must be one 4x4
dither tile from corner to corner, which it is only if each blend's
read-back had its dither taken out. 86Box's interpreter always did;
its two code generators did not until patch 64 (the columns walked
7bef ... 6b4d, 10,240 pixels off the tile). `RECOMP=off` runs the
interpreter, the A/B; `DITHER_SUB=off` (the device's `dither-sub=off`)
is the control that must fail this phase on either path.

Then two phases about **order** (doc 21 §13). The chip has one way in from
the PCI bus, so an LFB write is drawn after every packet the guest put in
the ring before it and a register that reconfigures the FIFO lands behind
them all; 86Box has two queues and empties the LFB one first. *Teardown* is
the guard: a cyan fill and a swap go into the ring and fbiInit7's
command-FIFO bit is cleared at once, with no idle wait. The frame has to
be cyan (the packets were run, not dropped) and the status register has to
read idle afterwards, which is what Glide's grSstIdle waits for at
grSstWinClose and what Carmageddon's 3dfx build hung on for ever. Both
halves of it fail without the fix.

*Ordering* is the other one: the ring gets a red fastfill over the whole
screen, then with nothing waited for the guest writes a 38,400-dword blue
block through the LFB, then the ring gets the swap. The block has to be on
top, and the device has to report at least one publish behind 86Box's own
FIFO (the scene is built to make one, and none means the ordering point
was never reached). Keeping the order there is patch 72's job (the thread's
ring loop yields the moment anything appears in the other FIFO), and it is
what keeps a game's HUD from landing in the buffer a swap has just turned
into the back one (Carmageddon's HUD flashed in and out without it). The
mirror of it, a wait for the *ring* before an LFB write, is `LFB_ORDER=on`:
that window is shorter than the consumer's wake and the block is on top
either way, which is the measurement that ruled it out as the HUD's cause.

Then the **swapped pair** (doc 21 §13): Glide writes a two-word packet
value first and header second, and the chip counts the ring's depth over
what is written *contiguously* (its hole counter). 86Box counted every
write as it came, so a consumer caught up with the guest read the header's
slot the moment the value landed (a stale word from the last lap, taken as
a header). That is how Carmageddon's 3dfx build froze on every race start
on `ramfifo=off`. The phase plants such a stale word (a type-1 header
claiming 256 values, written as a colour on a first lap), brings the
consumer to rest on that slot, writes the value of a `color1` packet past
it, lets the consumer look (`cmdFifoDepth` polled to zero), and only then
writes the header, followed by a fill and a swap: the frame has to be
yellow. Counted per write, the stale header eats the fill and the swap:
`MMIO_HOLES=off` (the device's `mmio-holes=off`, 86Box's own count) is the
control, and there the frame must *not* be yellow. The phases after it
are not judged, the consumer being parked inside the stale packet.

Then the **wrap** (doc 21 §13): the same pair, value first, at the ring's
base right after the guest's JMP back to it, with a stale header there
from the lap before, which is what every lap of Carmageddon's race does.
Glide writes cmdFifoAMin and AMax once at init and never at a wrap, so the
write side has to follow the JMP itself to know where the contiguous count
goes on; taking the value's slot as that point instead held every word of
the next lap until the hole bitmap ran out. The phase plants a type-1 header
claiming 256 values at the base as a real packet (run with its values, so
the word stays), writes a JMP to the base and waits for the consumer to
take it, then the pair value first, a fill and a swap: the frame has to be
green. Not judged in the control, whose consumer is already parked.

Then the **partial packet** (doc 21 §9): the header of a two-word packet
is written and its value is not, and `cmdFifoRdPtr` is read 256 times. The
chip takes the header and parks wanting the value, so the read pointer has
to stop one word in and stay there. It never passes what the guest has
written, because Glide's free space is `rp - wp - 1` and a pointer past
`wp` turns the whole ring into a few words of room, so the guest waits for
space on a ring that is empty. FIFA 2000's loading screen died on exactly
that. Then the value arrives and the packet completes.

Then the **stranded client** (doc 21 §11): with the FIFO still off from
the teardown, a burst of dwords goes into the 0x200000 window (what a
Glide whose FIFO was switched off under it goes on doing) at the offsets
of cmdFifoBaseAddr, videoDimensions and fbiInit7. The device refuses such a
write, so the ring's own register has to read back where it was;
`FIFO_OFF_REGS=on` (the device's `fifo-off-regs=on`) is the control that
decodes the burst as register writes the way 86Box does, and it has to move
the ring. That walk is what leaves a card unusable after a game's close
(FIFA 2000 on `base98-br` took the guest's Windows down with it).

Both phases write swapbufferCMD to the register window as well as putting
the packet in the ring, because 3dfx's Glide does and the card's own
accounting needs it: only Banshee and later count a swap *packet* as
written, while every card's swap decrements `cmd_read`, so the packet
alone drives `written - cmd_read` negative and the status register reads
busy for ever off that difference.

The evidence is on the host side: a QMP screendump while the Voodoo has
the monitor must be the 640x480 red frame (the whole path from a guest
`mov` to the console surface: register decode, the memory FIFO, the
swap on the display timer's retrace, the CLUT, the pass-through switch),
and one after it lets go must be the VGA's text screen again. Every
`voodoo2:` line QEMU printed is shown.

    tools/voodoo-guest-test.py          # needs nasm, mtools, build/qemu
    VGA=d3dpt tools/voodoo-guest-test.py   # beside our own adapter (std, cirrus, d3dpt)

The 2D adapter is whatever the machine has (a Voodoo 2 is a 3D-only
card that borrows the monitor), and `VGA=` picks it: `std` (default),
`cirrus`, or `d3dpt` for `-vga none -device d3dpt-vga`, the pairing a
launcher machine on our own display driver would run. Beside `d3dpt` the
program first puts that adapter in a linear mode, 800x600x32 filled
green, the state a Windows desktop leaves it in: the monitor the Voodoo
hands back is then a linear frame and not the VGA core's text screen, and
the screendump after the hand-back must be that green frame. It used to
stay the Voodoo's last one for good, because the adapter's invalidate did
not put its own surface back on the console when its mode had not changed
(every full-screen switch on a Win98 machine with the card left a stale
frame up).

Outputs in build/voodoo-guest/ (build/voodoo-guest-<VGA>/ beside another
adapter). The `voodoo-guest`, `voodoo-guest-d3dpt`, `voodoo-guest-mmiofifo`
and `voodoo-guest-undither` checks in the guest stage of scripts/test.sh.
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
VGA = os.environ.get("VGA", "std")
RAMFIFO = os.environ.get("RAMFIFO", "on")
RECOMP = os.environ.get("RECOMP", "on")
DITHER_SUB = os.environ.get("DITHER_SUB", "on")
UNDITHER = os.environ.get("UNDITHER", "off")
FIFO_OFF_REGS = os.environ.get("FIFO_OFF_REGS", "off")
LFB_ORDER = os.environ.get("LFB_ORDER", "on")
MMIO_HOLES = os.environ.get("MMIO_HOLES", "on")
OUT = os.path.join(ROOT, "build/voodoo-guest" + ("" if VGA == "std" else "-" + VGA)
                   + ("" if RAMFIFO == "on" else "-mmiofifo")
                   + ("" if RECOMP == "on" else "-interp")
                   + ("" if DITHER_SUB == "on" else "-nodsub")
                   + ("" if UNDITHER == "off" else "-undither")
                   + ("" if FIFO_OFF_REGS == "off" else "-fifooffregs")
                   + ("" if LFB_ORDER == "on" else "-noorder")
                   + ("" if MMIO_HOLES == "on" else "-noholes"))

spec = importlib.util.spec_from_file_location("x87gt", os.path.join(ROOT, "tools/x87-guest-test.py"))
x87gt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(x87gt)
spec = importlib.util.spec_from_file_location("vgadirty", os.path.join(ROOT, "tools/vga-dirty-guest-test.py"))
vgadirty = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vgadirty)

# Where the guest maps the BAR (16 MiB, above RAM, below the APIC).
BAR = 0xE0000000
FIFO_BASE = 0x300000        # the ring's base, as the program's own equ
WIDTH, HEIGHT = 640, 480

ASM = r"""
org 100h
bits 16

BAR         equ 0E0000000h
LFB         equ BAR + 400000h
RED565      equ 0F800F800h          ; two RGB565 pixels of (248, 0, 0)
BLUE565     equ 0001F001Fh          ; two RGB565 pixels of (0, 0, 248)
ORDER_FILLS equ 192                 ; full-screen fills the ordering scene
                                    ; puts in the ring, to keep the consumer
                                    ; behind while the LFB block is written

; register offsets (vid_voodoo_regs.h)
SST_status          equ 000h
SST_lfbMode         equ 114h
SST_swapbufferCMD   equ 128h
SST_videoDimensions equ 20Ch
SST_fbiInit0        equ 210h
SST_fbiInit1        equ 214h
SST_fbiInit2        equ 218h
SST_hSync           equ 220h
SST_vSync           equ 224h
SST_clutData        equ 228h
SST_dacData         equ 22Ch
SST_fbiInit7        equ 24Ch
SST_cmdFifoBaseAddr equ 1E0h
SST_cmdFifoRdPtr    equ 1E8h
SST_cmdFifoDepth    equ 1F4h

; the command FIFO: an 8 KiB ring at 3 MiB of frame-buffer memory (clear of
; the three 640x480 buffers), written through its window at BAR + 200000h;
; batch 1 starts near the ring's end so that it has to jump back
FIFO_BASE           equ 300000h
FIFO_SIZE           equ 2000h
FIFO_START          equ 1F00h
FIFO_WIN            equ BAR + 200000h
; the dither phase re-points the ring at 128 KiB: its whole scene is one
; linear run of packets, so nothing has to wrap or wait for room
DITH_SIZE           equ 40000h

; IEEE floats, the setup unit's vertices and colours
GREY_F              equ 43020000h       ; 130.0: dithers in every channel
A255_F              equ 437F0000h       ; 255.0, opaque
A128_F              equ 43000000h       ; 128.0, half
F640                equ 44200000h
F480                equ 43F00000h
FBAND0              equ 43480000h       ; 200.0: the blended band's top
FBAND1              equ 43B40000h       ; 360.0: and its bottom

; d3dpt-vga (d3dpt/d3dpt_fb.h)
D3D_ENABLE          equ 040h
D3D_WIDTH           equ 044h
D3D_HEIGHT          equ 048h
D3D_BPP             equ 04Ch
D3D_PITCH           equ 050h
D3D_OFFSET          equ 054h
D3D_HZ              equ 058h
D3D_CUR_ADDR        equ 090h
D3D_CUR_W           equ 094h
D3D_CUR_H           equ 098h
D3D_CUR_DEFINE      equ 0A4h
D3D_CUR_X           equ 0A8h
D3D_CUR_Y           equ 0ACh
D3D_CUR_ENABLE      equ 0B0h
D3D_CUR_AT          equ 00200000h       ; past the 800x600x32 frame
GREEN32             equ 0000FF00h

start:
    mov si, str_hello
    call puts
    call a20_on
    call unreal_mode            ; FS = 4 GiB flat data segment
    call d3dpt_linear           ; beside our own adapter: a linear desktop first

    ; --- find the card: bus 0, devices 0..31, function 0 ---------------
    xor bx, bx                  ; bx = device number
.scan:
    mov eax, 80000000h
    mov ecx, ebx
    shl ecx, 11
    or eax, ecx                 ; bus 0, dev bx, func 0, reg 0
    mov dx, 0CF8h
    out dx, eax
    mov dx, 0CFCh
    in eax, dx
    cmp eax, 0002121Ah          ; vendor 121a, device 0002
    je .found
    inc bx
    cmp bx, 32
    jb .scan
    mov si, str_nocard
    call puts
    jmp exit
.found:
    mov [pci_dev], bx
    mov si, str_found
    call puts
    mov eax, ebx
    call puthex8
    call putnl

    ; --- map it and enable memory decoding -----------------------------
    mov cl, 10h
    mov eax, BAR
    call pci_write
    mov cl, 10h
    call pci_read
    mov si, str_bar
    call puts
    call puthex32
    call putnl
    mov cl, 04h
    call pci_read
    or eax, 2                   ; memory space enable
    mov cl, 04h
    call pci_write

    ; --- initEnable: bit 0 unlocks the fbiInit registers; byte 1 reads
    ;     back 0x50 | its high nibble on a Voodoo 2 (the strap the 3dfx
    ;     driver checks) ---------------------------------------------------
    mov cl, 40h
    mov eax, 1
    call pci_write
    mov cl, 40h
    call pci_read
    mov si, str_init
    call puts
    call puthex32
    call putnl
    mov [init_enable], eax

    ; --- siProcess (0x54), the way Glide's sst1InitMeasureSiProcess reads
    ;     it: load a PCI-clock countdown into bits 27:16 with the ring
    ;     oscillator in reset, read once, set RUN, and poll until the
    ;     countdown reads zero; the oscillator count is then bits 15:0. A
    ;     configuration space that merely stores the load never reads zero
    ;     there, and 3dfx's glide2x.dll sat in that loop on every
    ;     grSstWinOpen (2026-09-12). Bounded here, so a wrong device is a
    ;     line rather than a hang ----------------------------------------
    mov cl, 54h
    mov eax, 0FFF0000h          ; load 0xfff, NAND tree, counter held in reset
    call pci_write
    mov cl, 54h
    call pci_read
    mov cl, 54h
    mov eax, 1FFF0000h          ; RUN
    call pci_write
    mov dword [si_polls], 100000
.si_poll:
    mov cl, 54h
    call pci_read
    test eax, 0FFF0000h
    jz .si_done
    dec dword [si_polls]
    jnz .si_poll
    mov si, str_si_timeout
    call puts
    jmp .si_out
.si_done:
    mov si, str_si
    call puts
    call puthex32
    call putnl
.si_out:

    ; --- the chip's own init: what sst1init does before a game draws ----
    mov edi, BAR
    mov eax, 0A0h               ; fbiInit1: 10 x 2 blocks of 32 px = 640 px rows
    mov [fs:edi + SST_fbiInit1], eax
    mov eax, 150 << 11          ; fbiInit2: buffers 150 x 4 KiB apart (640 x 480 x 2)
    mov [fs:edi + SST_fbiInit2], eax
    mov eax, (480 << 16) | 639  ; videoDimensions
    mov [fs:edi + SST_videoDimensions], eax
    mov eax, 009F027Fh          ; hSync: 640 active + 159 blank
    mov [fs:edi + SST_hSync], eax
    mov eax, 002D01E0h          ; vSync: 480 active + 45 blank
    mov [fs:edi + SST_vSync], eax
    mov eax, 0400h              ; dacData: PLL register select = 0
    mov [fs:edi + SST_dacData], eax
    mov eax, 0556h              ; PLL0 low byte: m = 0x56 + 2 = 88
    mov [fs:edi + SST_dacData], eax
    mov eax, 0537h              ; PLL0 high byte: n1 = 0x17 + 2 = 25, n2 = 1 -> 25.2 MHz
    mov [fs:edi + SST_dacData], eax

    ; the CLUT: 33 entries, a linear ramp (entry i = 8i; the last one is
    ; "set to 255" through bit 29, as the driver writes it)
    xor ecx, ecx
.clut:
    mov eax, ecx
    shl eax, 3                  ; 8i
    mov ebx, eax
    shl ebx, 8
    or eax, ebx                 ; g
    shl ebx, 8
    or eax, ebx                 ; r
    mov ebx, ecx
    shl ebx, 24
    or eax, ebx                 ; index
    cmp ecx, 32
    jb .clut_write
    mov eax, (32 << 24) | 20000000h
.clut_write:
    mov [fs:edi + SST_clutData], eax
    inc ecx
    cmp ecx, 33
    jb .clut

    mov eax, 050h               ; lfbMode: RGB565, write the back buffer, read the back buffer
    mov [fs:edi + SST_lfbMode], eax
    mov eax, 1                  ; fbiInit0: VGA pass-through -- the Voodoo takes the monitor
    mov [fs:edi + SST_fbiInit0], eax
    mov eax, [fs:edi + SST_fbiInit0]
    mov si, str_fbiinit0
    call puts
    call puthex32
    call putnl
    mov eax, [fs:edi + SST_status]
    mov si, str_status
    call puts
    call puthex32
    call putnl

    ; --- fill the back buffer through the LFB: 480 rows of 320 dwords,
    ;     2 KiB apart in the window --------------------------------------
    mov si, str_fill
    call puts
    mov edi, LFB
    mov edx, 480
.row:
    mov ecx, 320
    mov ebx, edi
.px:
    mov dword [fs:ebx], RED565
    add ebx, 4
    dec ecx
    jnz .px
    add edi, 2048
    dec edx
    jnz .row

    ; read one pixel pair back: row 10, x = 20..21
    mov edi, LFB + (10 << 11) + 40
    mov eax, [fs:edi]
    mov si, str_readback
    call puts
    call puthex32
    call putnl
    cmp eax, RED565
    je .rb_ok
    mov si, str_rb_fail
    call puts
    jmp .swap
.rb_ok:
    mov si, str_rb_pass
    call puts
.swap:
    ; --- swap on the next vertical retrace -----------------------------
    mov edi, BAR
    mov eax, 1
    mov [fs:edi + SST_swapbufferCMD], eax
    mov cx, 9                   ; ~0.5 s
    call delay_ticks
    mov si, str_swapped
    call puts

    mov cx, 36                  ; ~2 s: the host takes its screendump
    call delay_ticks

    ; --- the command FIFO (doc 21 §9), the way 3dfx's Glide drives it:
    ;     packets written into the ring's window, and the chip told nothing
    ;     -- hole counting on, no doorbell. Under ramfifo=on the window is
    ;     RAM and the device finds the packets at the next access to the
    ;     card; the read pointer the guest polls says how far the chip got,
    ;     and the frame says what it drew -------------------------------------
    mov edi, BAR
    mov eax, (FIFO_BASE >> 12) | (((FIFO_BASE + FIFO_SIZE - 1000h) >> 12) << 16)
    mov [fs:edi + SST_cmdFifoBaseAddr], eax
    mov eax, FIFO_BASE + FIFO_START
    mov [fs:edi + SST_cmdFifoRdPtr], eax
    xor eax, eax
    mov [fs:edi + SST_cmdFifoDepth], eax
    mov eax, 100h               ; fbiInit7: the command FIFO on
    mov [fs:edi + SST_fbiInit7], eax

    ; batch 1: fbzMode, the clip, color1 = blue, a jump to the ring's
    ; start, then fastfill and swap there
    mov edi, FIFO_WIN + FIFO_START
    mov dword [fs:edi], 00010221h       ; packet 1: fbzMode
    mov dword [fs:edi + 4], 4200h       ;   RGB writes, draw the back buffer
    mov dword [fs:edi + 8], 00028231h   ; packet 1: 2 from clipLeftRight on
    mov dword [fs:edi + 12], 280h       ;   x 0..640
    mov dword [fs:edi + 16], 1E0h       ;   y 0..480
    mov dword [fs:edi + 20], 00010291h  ; packet 1: color1
    mov dword [fs:edi + 24], 0F8h       ;   blue
    mov dword [fs:edi + 28], (FIFO_BASE << 4) | 18h   ; packet 0: JMP
    mov edi, FIFO_WIN
    mov dword [fs:edi], 00010249h       ; packet 1: fastfillCMD
    mov dword [fs:edi + 4], 0
    mov dword [fs:edi + 8], 00010251h   ; packet 1: swapbufferCMD
    mov dword [fs:edi + 12], 1          ;   on the next retrace
    call swap_note
    mov edx, FIFO_BASE + 10h            ; where the read pointer must end
    mov si, str_fifo1
    call fifo_wait
    mov si, str_fifo1_swapped
    call puts
    mov cx, 36                  ; ~2 s: the host takes its screendump
    call delay_ticks

    ; batch 2, after the read pointer was read (under ramfifo=on the words
    ; batch 1 took are poison by then): color1 = magenta, fill, swap
    mov edi, FIFO_WIN + 10h
    mov dword [fs:edi], 00010291h       ; packet 1: color1
    mov dword [fs:edi + 4], 0F800F8h    ;   magenta
    mov dword [fs:edi + 8], 00010249h   ; packet 1: fastfillCMD
    mov dword [fs:edi + 12], 0
    mov dword [fs:edi + 16], 00010251h  ; packet 1: swapbufferCMD
    mov dword [fs:edi + 20], 1
    call swap_note
    mov edx, FIFO_BASE + 28h
    mov si, str_fifo2
    call fifo_wait
    mov si, str_fifo2_swapped
    call puts
    mov cx, 36
    call delay_ticks

    call dither_phase
    call order_phase
    call swapped_phase
    call wrap_phase
    call partial_phase
    call teardown_phase
    call stranded_phase

    mov edi, BAR
    xor eax, eax                ; fbiInit7: the command FIFO off again
    mov [fs:edi + SST_fbiInit7], eax

    ; --- give the monitor back to the VGA -------------------------------
    mov edi, BAR
    xor eax, eax
    mov [fs:edi + SST_fbiInit0], eax
    mov cx, 6
    call delay_ticks
    mov si, str_done
    call puts
exit:
    mov ax, 4C00h
    int 21h

; ---------------------------------------------------------------- helpers

; d3dpt-vga, when the machine has one: 800x600x32 filled green and ENABLE
; set -- what a Windows desktop on our driver leaves the adapter in, so
; the monitor the Voodoo later hands back is a linear frame. Its BARs are
; where SeaBIOS put them (0 = VRAM, 1 = the registers).
d3dpt_linear:
    xor ebx, ebx
.scan:
    mov eax, 80000000h
    mov ecx, ebx
    shl ecx, 11
    or eax, ecx
    mov dx, 0CF8h
    out dx, eax
    mov dx, 0CFCh
    in eax, dx
    cmp eax, 3D001234h          ; vendor 1234, device 3d00
    je .found
    inc bx
    cmp bx, 32
    jb .scan
    ret                         ; another adapter: nothing to do
.found:
    mov [pci_dev], bx
    mov cl, 10h
    call pci_read
    and eax, 0FFFFFFF0h
    mov [d3d_vram], eax
    mov cl, 14h
    call pci_read
    and eax, 0FFFFFFF0h
    mov [d3d_regs], eax
    mov cl, 04h
    call pci_read
    or eax, 2                   ; memory space enable (SeaBIOS set it already)
    mov cl, 04h
    call pci_write
    mov si, str_d3dpt
    call puts
    mov eax, [d3d_vram]
    call puthex32
    mov al, ' '
    call putc
    mov eax, [d3d_regs]
    call puthex32
    call putnl

    mov edi, [d3d_vram]
    mov ecx, 800 * 600
.fill:
    mov dword [fs:edi], GREEN32
    add edi, 4
    dec ecx
    jnz .fill

    mov edi, [d3d_regs]
    mov dword [fs:edi + D3D_WIDTH], 800
    mov dword [fs:edi + D3D_HEIGHT], 600
    mov dword [fs:edi + D3D_BPP], 32
    mov dword [fs:edi + D3D_PITCH], 0
    mov dword [fs:edi + D3D_OFFSET], 0
    mov dword [fs:edi + D3D_HZ], 60
    mov dword [fs:edi + D3D_ENABLE], 1
    mov eax, [fs:edi + D3D_ENABLE]
    mov si, str_d3d_enable
    call puts
    call puthex32
    call putnl

    ; and a hardware cursor shown on it, as a Windows desktop has: a 2x2
    ; white sprite at 100,100. While the Voodoo has the monitor it must not
    ; be published as visible (patch 66), and after the hand-back it must be
    push edi
    mov edi, [d3d_vram]
    add edi, D3D_CUR_AT
    mov ecx, 4
.cur:
    mov dword [fs:edi], 0FFFFFFFFh
    add edi, 4
    dec ecx
    jnz .cur
    pop edi
    mov dword [fs:edi + D3D_CUR_ADDR], D3D_CUR_AT
    mov dword [fs:edi + D3D_CUR_W], 2
    mov dword [fs:edi + D3D_CUR_H], 2
    mov dword [fs:edi + D3D_CUR_DEFINE], 1
    mov dword [fs:edi + D3D_CUR_X], 100
    mov dword [fs:edi + D3D_CUR_Y], 100
    mov dword [fs:edi + D3D_CUR_ENABLE], 1
    ; a page flip, as a DirectDraw game makes: the sprite must go (a flip
    ; chain draws its own pointer), and a mode set gives the screen back
    mov dword [fs:edi + D3D_OFFSET], 800 * 600 * 4
    mov dword [fs:edi + D3D_OFFSET], 0
    mov dword [fs:edi + D3D_ENABLE], 1
    ; and a flip chain that ends with no mode set, as a game at the
    ; desktop's own mode does (3DMark 99 at 800x600x16): idle on its other
    ; page the sprite stays hidden, idle on the desktop's page it comes
    ; back -- the device's 2 s without a flip, well inside each ~3 s wait
    push edi
    mov dword [fs:edi + D3D_OFFSET], 800 * 600 * 4
    mov cx, 55
    call delay_ticks
    pop edi
    push edi
    mov dword [fs:edi + D3D_OFFSET], 0
    mov cx, 55
    call delay_ticks
    pop edi
    ; back on the desktop's page: the host takes its linear-mode screendump
    ; now, not at ENABLE (page 2 above is not the green one)
    mov si, str_d3d_ready
    call puts
    call putnl
    mov cx, 36                  ; ~2 s: the host's screendump shows the linear
    call delay_ticks            ; mode, the way the player's refresh would
    ret

; --------------------------------------------------- the dither phase
;
; What repeated alpha blending does to a 16-bit frame buffer, drawn by the
; chip's own triangle rasterizer (doc 21 §9). A Voodoo dithers what it
; writes, so a pixel read back to be blended with carries this position's
; dither offset, and the chip subtracts it again on the way in when
; fbzMode's DITHER_SUB bit is set. 86Box's plain interpreter does that;
; its two recompilers did not until patch 64, so the error accumulated
; there, one blend at a time. The scene makes that visible: a grey that has to dither
; in every channel (130,130,130 -- red and blue step by 8, green by 4),
; drawn over the screen, then blended *onto itself* at alpha 128 in eight
; columns, 1, 2, 4 ... 128 times. Blending a colour onto itself is that
; colour, so every column should stay the background's grey and the bands
; above and below are the reference. One pixel per column goes to COM1.
;
; Everything is packets: with the FIFO on, a direct register write is
; ignored, and the vertices go through the setup unit (sBeginTriCMD /
; sDrawTriCMD), which sorts them and works out the area's sign itself.
dither_phase:
    ; the ring again, 128 KiB this time: the whole scene is written in one
    ; linear run, so nothing has to wrap or wait for room
    mov edi, BAR
    xor eax, eax
    mov [fs:edi + SST_fbiInit7], eax        ; off while the ring moves
    mov eax, (FIFO_BASE >> 12) | (((FIFO_BASE + DITH_SIZE - 1000h) >> 12) << 16)
    mov [fs:edi + SST_cmdFifoBaseAddr], eax
    mov eax, FIFO_BASE
    mov [fs:edi + SST_cmdFifoRdPtr], eax
    xor eax, eax
    mov [fs:edi + SST_cmdFifoDepth], eax
    mov eax, 100h
    mov [fs:edi + SST_fbiInit7], eax

    mov edi, FIFO_WIN
    mov dword [fs:edi], 00010221h       ; fbzMode
    mov dword [fs:edi + 4], 00084300h   ;   dither, dither-subtract, RGB write, back buffer
    mov dword [fs:edi + 8], 00010209h   ; fbzColorPath
    mov dword [fs:edi + 12], 00824100h  ;   flat iterated colour and alpha, no texture
    mov dword [fs:edi + 16], 00010219h  ; alphaMode
    mov dword [fs:edi + 20], 00005110h  ;   blend: src alpha, one minus src alpha
    mov dword [fs:edi + 24], 000104C1h  ; sSetupMode
    mov dword [fs:edi + 28], 3          ;   the vertices carry RGB and alpha
    add edi, 32

    ; the background: the grey over the whole screen, opaque (alpha 255),
    ; written once through the pixel pipeline -- so it is dithered, which is
    ; what the blends below then read back
    mov eax, A255_F
    mov [dith_a], eax
    xor eax, eax
    mov [dith_x0], eax                  ; 0.0
    mov eax, F640
    mov [dith_x1], eax
    xor eax, eax
    mov [dith_y0], eax                  ; 0.0
    mov eax, F480
    mov [dith_y1], eax
    call dith_quad

    ; and the columns: the same grey blended onto itself 96 times, each
    ; column at a lower alpha than the one before (128, 96 ... 12). A blend
    ; whose read-back was never un-dithered settles about d*(1-a)/a away
    ; from the colour, so the lower the alpha the further the column sits
    ; from the reference: a staircase where the dither is not subtracted,
    ; flat where it is
    mov eax, FBAND0
    mov [dith_y0], eax
    mov eax, FBAND1
    mov [dith_y1], eax
    xor ebx, ebx
.col:
    mov esi, ebx
    shl esi, 2
    mov eax, [col_x + esi]
    mov [dith_x0], eax
    mov eax, [col_x + esi + 4]
    mov [dith_x1], eax
    mov eax, [col_a + esi]
    mov [dith_a], eax
    mov dword [dith_passes], 96
.pass:
    call dith_quad
    dec dword [dith_passes]
    jnz .pass
    inc ebx
    cmp ebx, 8
    jb .col

    ; wait for the chip to reach the end of what was written
    mov edx, edi
    sub edx, FIFO_WIN
    add edx, FIFO_BASE
    mov si, str_dith_rp
    call fifo_wait

    ; read one pixel out of each column, and one out of the reference band
    mov si, str_dith_ref
    call puts
    mov edi, LFB + (100 << 11) + (320 * 2)
    movzx eax, word [fs:edi]
    call puthex32
    call putnl
    xor ebx, ebx
.read:
    mov si, str_dith_col
    call puts
    mov eax, ebx
    call puthex8
    mov al, ' '
    call putc
    mov esi, ebx
    shl esi, 2
    mov edi, [col_px + esi]             ; the LFB address of this column
    movzx eax, word [fs:edi]
    call puthex32
    call putnl
    inc ebx
    cmp ebx, 8
    jb .read

    ; show it: a swap, then the host's screendump
    mov edi, [dith_cursor]
    mov dword [fs:edi], 00010251h       ; swapbufferCMD
    mov dword [fs:edi + 4], 1
    add edi, 8
    mov edx, edi
    sub edx, FIFO_WIN
    add edx, FIFO_BASE
    call swap_note
    mov si, str_dith_swap
    call fifo_wait
    mov si, str_dith_shown
    call puts
    mov cx, 54                          ; ~3 s: the host takes its screendump
    call delay_ticks
    ret

; one rectangle, as the two triangles the setup unit wants: the vertex
; registers and then its command, per vertex
dith_quad:
    mov [dith_cursor], edi
    mov eax, [dith_x0]
    mov edx, [dith_y0]
    call dith_vertex_begin
    mov eax, [dith_x1]
    mov edx, [dith_y0]
    call dith_vertex_draw
    mov eax, [dith_x0]
    mov edx, [dith_y1]
    call dith_vertex_draw
    mov eax, [dith_x1]
    mov edx, [dith_y0]
    call dith_vertex_begin
    mov eax, [dith_x0]
    mov edx, [dith_y1]
    call dith_vertex_draw
    mov eax, [dith_x1]
    mov edx, [dith_y1]
    call dith_vertex_draw
    mov [dith_cursor], edi
    ret

; EAX = x, EDX = y (both IEEE floats); the colour is the grey, the alpha
; whatever the caller set
dith_vertex:
    mov dword [fs:edi], 000284C9h       ; sVx, sVy
    mov [fs:edi + 4], eax
    mov [fs:edi + 8], edx
    mov dword [fs:edi + 12], 000484E1h  ; sRed, sGreen, sBlue, sAlpha
    mov eax, GREY_F
    mov [fs:edi + 16], eax
    mov [fs:edi + 20], eax
    mov [fs:edi + 24], eax
    mov eax, [dith_a]
    mov [fs:edi + 28], eax
    add edi, 32
    ret
dith_vertex_begin:
    call dith_vertex
    mov dword [fs:edi], 00010549h       ; sBeginTriCMD
    mov dword [fs:edi + 4], 0
    add edi, 8
    ret
dith_vertex_draw:
    call dith_vertex
    mov dword [fs:edi], 00010541h       ; sDrawTriCMD
    mov dword [fs:edi + 4], 0
    add edi, 8
    ret

; ------------------------------------------------- the ordering phase
;
; The chip has one way in from the PCI bus, so a write into the LFB window
; is drawn after every packet the guest put in the ring before it. 86Box
; has two queues and empties the LFB one first, which puts an LFB write in
; front of work the guest wrote earlier -- a game that draws its world with
; triangles and its HUD through the LFB then loses the HUD on whichever
; frames the consumer was behind (Carmageddon, 2026-09-18). The scene is
; the smallest statement of it: the ring gets a red fastfill over the whole
; screen, then, with nothing waited for, the guest writes a blue block
; through the LFB, then the ring gets the swap. The blue block has to be
; there. Out of order the fastfill runs last and the frame is all red;
; `lfb-order=off` is the A/B and fails here.
order_phase:
    mov edi, BAR
    xor eax, eax
    mov [fs:edi + SST_fbiInit7], eax        ; off while the ring moves back
    mov eax, (FIFO_BASE >> 12) | (((FIFO_BASE + FIFO_SIZE - 1000h) >> 12) << 16)
    mov [fs:edi + SST_cmdFifoBaseAddr], eax
    mov eax, FIFO_BASE
    mov [fs:edi + SST_cmdFifoRdPtr], eax
    xor eax, eax
    mov [fs:edi + SST_cmdFifoDepth], eax
    mov eax, 100h
    mov [fs:edi + SST_fbiInit7], eax

    ; the fill, and the dither phase's pipeline state put back
    mov edi, FIFO_WIN
    mov dword [fs:edi], 00010221h       ; fbzMode
    mov dword [fs:edi + 4], 4200h       ;   RGB writes, back buffer, no dither
    mov dword [fs:edi + 8], 00010209h   ; fbzColorPath
    mov dword [fs:edi + 12], 0
    mov dword [fs:edi + 16], 00010219h  ; alphaMode
    mov dword [fs:edi + 20], 0          ;   no blending
    mov dword [fs:edi + 24], 00028231h  ; 2 from clipLeftRight on
    mov dword [fs:edi + 28], 280h       ;   x 0..640
    mov dword [fs:edi + 32], 1E0h       ;   y 0..480
    mov dword [fs:edi + 36], 00010291h  ; color1
    mov dword [fs:edi + 40], 0F80000h   ;   red

    ; ORDER_FILLS fastfills, not one: the point of the scene is that the
    ; consumer is *behind* when the LFB writes arrive, which is the state a
    ; game is in all the time (Carmageddon's race ran ~20,000 words behind)
    ; and the only state in which the ordering can be got wrong. A single
    ; fastfill is consumed before the guest has written its second LFB
    ; dword and the scene passes whatever the thread does.
    mov edi, FIFO_WIN + 2Ch
    mov ecx, ORDER_FILLS
.fill:
    mov dword [fs:edi], 00010249h       ; fastfillCMD
    mov dword [fs:edi + 4], 0
    add edi, 8
    dec ecx
    jnz .fill

    ; and now, with the chip told nothing, the block through the LFB: rows
    ; 100..339, 320 pixels wide. The first of these writes is the guest's
    ; next access to the card, so it is where the packets above are counted
    ; -- and where the two streams have to be put back in order
    mov edi, LFB + (100 << 11)
    mov edx, 240
.row:
    mov ecx, 160
    mov ebx, edi
.px:
    mov dword [fs:ebx], BLUE565
    add ebx, 4
    dec ecx
    jnz .px
    add edi, 2048
    dec edx
    jnz .row

    mov edi, FIFO_WIN + 2Ch + ORDER_FILLS * 8
    mov dword [fs:edi], 00010251h       ; swapbufferCMD
    mov dword [fs:edi + 4], 1           ;   on the next retrace
    call swap_note
    mov edx, FIFO_BASE + 34h + ORDER_FILLS * 8
    mov si, str_order
    call fifo_wait
    mov si, str_order_swapped
    call puts
    mov cx, 36                  ; ~2 s: the host takes its screendump
    call delay_ticks
    ret

; ------------------------------------------------ the swapped-pair phase
;
; Glide writes a two-word packet value first and header second, and the
; chip counts the ring's depth over what has been written contiguously
; (cmdFifoAMin / AMax / Holes). 86Box counted every write as it came, so a
; consumer caught up with the guest read the header's slot the moment the
; value landed -- a stale word from the last lap, taken as a header. One
; that said "256 values follow" ate the next 256 words as data, and the
; first word after those that read as a Banshee packet was a fatal():
; Carmageddon's 3dfx build froze on every race start on `ramfifo=off`
; (2026-09-21), the guest polling cmdFifoRdPtr for room the card would
; never make. The device counts a word once every word before it has
; arrived now, as the chip does.
;
; The phase plants exactly that stale word: one lap writes a color1 packet
; whose *value* is a type-1 header claiming 256 values, and the chip runs
; it (a colour, nothing more). Then the ring is set to its base again, a
; NOP goes in the first slot and the consumer is waited for at the second,
; where the stale word now sits; the value of a color1 packet is written
; to the third slot, cmdFifoDepth polled down to zero so the consumer has
; had its look, and only then the header to the second. A fill and a swap
; follow. Counted as the chip counts, the frame is yellow; counted per
; write, the stale header ate the fill and the swap and the frame is not.
; With the ring in RAM the restart poisons the slot and nothing is stale,
; so there the phase only has to complete.
swapped_phase:
    mov edi, BAR                        ; the ring at its base
    xor eax, eax
    mov [fs:edi + SST_fbiInit7], eax
    mov eax, (FIFO_BASE >> 12) | (((FIFO_BASE + FIFO_SIZE - 1000h) >> 12) << 16)
    mov [fs:edi + SST_cmdFifoBaseAddr], eax
    mov eax, FIFO_BASE
    mov [fs:edi + SST_cmdFifoRdPtr], eax
    xor eax, eax
    mov [fs:edi + SST_cmdFifoDepth], eax
    mov eax, 100h
    mov [fs:edi + SST_fbiInit7], eax

    mov edi, FIFO_WIN                   ; lap 1: the stale word, planted in
    mov dword [fs:edi], 00010291h       ; the second slot as a colour -- a
    mov dword [fs:edi + 4], 01000291h   ; type-1 header, 256 values to come
    mov edx, FIFO_BASE + 8
    mov si, str_sp_plant
    call fifo_wait

    mov edi, BAR                        ; the ring at its base again, over
    xor eax, eax                        ; the same memory
    mov [fs:edi + SST_fbiInit7], eax
    mov eax, FIFO_BASE
    mov [fs:edi + SST_cmdFifoRdPtr], eax
    xor eax, eax
    mov [fs:edi + SST_cmdFifoDepth], eax
    mov eax, 100h
    mov [fs:edi + SST_fbiInit7], eax

    mov edi, FIFO_WIN                   ; a NOP in the first slot, so the
    mov dword [fs:edi], 0               ; consumer comes to rest on the second
    mov edx, FIFO_BASE + 4
    mov si, str_sp_nop
    call fifo_wait

    mov edi, FIFO_WIN                   ; the value first: yellow, in the
    mov dword [fs:edi + 8], 00F8F800h   ; third slot
    mov edi, BAR                        ; ... and the consumer has its look
    mov ecx, 2000000
.settle:
    mov eax, [fs:edi + SST_cmdFifoDepth]
    test eax, eax
    jz .settled
    dec ecx
    jnz .settle
.settled:
    mov edi, FIFO_WIN                   ; then the header, in the second
    mov dword [fs:edi + 4], 00010291h   ; color1
    mov dword [fs:edi + 12], 00010249h  ; fastfillCMD
    mov dword [fs:edi + 16], 0
    mov dword [fs:edi + 20], 00010251h  ; swapbufferCMD
    mov dword [fs:edi + 24], 1
    call swap_note
    mov edx, FIFO_BASE + 28
    mov si, str_sp_done
    call fifo_wait
    mov si, str_sp_swapped
    call puts
    mov cx, 36                          ; ~2 s: the host takes its screendump
    call delay_ticks
    ret

; ------------------------------------------------------ the wrap phase
;
; The same stale header, at the ring's base after Glide's JMP back to it.
; Glide writes cmdFifoAMin and AMax once at init and never at a wrap, so
; the chip follows its own JMP on the write side to know where the
; contiguous count goes on, and the device does the same (doc 21 §13).
; Without it the first packet after every wrap -- value first, header
; second, at the base -- was taken as the guest continuing from the value,
; its header as a jump back, and the words after it held until the hole
; counter's bitmap ran out 64 words on (Carmageddon's race, 2026-09-21:
; five laps of 359). The phase plants a type-1 header claiming 256 values
; *at the base* -- as a real packet, run with its 256 values, so the word
; stays there -- writes a JMP to the base after it and waits for the
; consumer to take it (the read pointer at the base), writes the value of a
; color1 packet to the second slot, lets the consumer look, then the header
; to the first, a fill and a swap: the frame has to be green.
wrap_phase:
    mov edi, BAR                        ; a fresh ring at its base
    xor eax, eax
    mov [fs:edi + SST_fbiInit7], eax
    mov eax, (FIFO_BASE >> 12) | (((FIFO_BASE + FIFO_SIZE - 1000h) >> 12) << 16)
    mov [fs:edi + SST_cmdFifoBaseAddr], eax
    mov eax, FIFO_BASE
    mov [fs:edi + SST_cmdFifoRdPtr], eax
    xor eax, eax
    mov [fs:edi + SST_cmdFifoDepth], eax
    mov eax, 100h
    mov [fs:edi + SST_fbiInit7], eax

    mov edi, FIFO_WIN                   ; the plant: color1 256 times over,
    mov dword [fs:edi], 01000291h       ; a header the base keeps afterwards
    add edi, 4
    mov ecx, 256
    xor eax, eax
.plant:
    mov [fs:edi], eax
    add edi, 4
    dec ecx
    jnz .plant
    mov edx, FIFO_BASE + 404h
    mov si, str_wr_plant
    call fifo_wait

    mov edi, FIFO_WIN                   ; the JMP back to the base, and the
    mov dword [fs:edi + 404h], (FIFO_BASE << 4) | 18h ; consumer taking it
    mov edx, FIFO_BASE
    mov si, str_wr_jump
    call fifo_wait

    mov edi, FIFO_WIN                   ; the value first: green, in the
    mov dword [fs:edi + 4], 0000F800h   ; second slot
    mov edi, BAR                        ; ... and the consumer has its look
    mov ecx, 2000000
.settle:
    mov eax, [fs:edi + SST_cmdFifoDepth]
    test eax, eax
    jz .settled
    dec ecx
    jnz .settle
.settled:
    mov edi, FIFO_WIN                   ; then the header, in the first
    mov dword [fs:edi], 00010291h       ; color1
    mov dword [fs:edi + 8], 00010249h   ; fastfillCMD
    mov dword [fs:edi + 12], 0
    mov dword [fs:edi + 16], 00010251h  ; swapbufferCMD
    mov dword [fs:edi + 20], 1
    call swap_note
    mov edx, FIFO_BASE + 24
    mov si, str_wr_done
    call fifo_wait
    mov si, str_wr_swapped
    call puts
    mov cx, 36                          ; ~2 s: the host takes its screendump
    call delay_ticks
    ret

; ------------------------------------------------ the partial-packet phase
;
; The one invariant the RAM ring owes the guest: **the read pointer never
; passes what the guest has written**. Glide's free space is `rp - wp - 1`,
; so a pointer past the write pointer turns the whole ring into a few words
; of room and the guest waits for space on a ring that is empty, for ever
; (2026-09-20, FIFA 2000's loading screen: a 66-word LFB packet with 19
; words written, 47 taken on a guess, and a guest with 46 words of room
; asking for 66). It is also a state the chip cannot reach.
;
; The phase writes the *header* of a two-word packet and nothing else, then
; reads cmdFifoRdPtr 256 times -- more than the 64 polls the old last
; resort waited for. The chip consumes the header and parks wanting the
; value, so the pointer has to read one word in and stay there. Then the
; value arrives and the packet completes. Both transports answer the same:
; with `ramfifo=off` every word is counted as it is written and nothing is
; ever guessed at.
partial_phase:
    mov edi, BAR                        ; the ring back to its base
    xor eax, eax
    mov [fs:edi + SST_fbiInit7], eax
    mov eax, (FIFO_BASE >> 12) | (((FIFO_BASE + FIFO_SIZE - 1000h) >> 12) << 16)
    mov [fs:edi + SST_cmdFifoBaseAddr], eax
    mov eax, FIFO_BASE
    mov [fs:edi + SST_cmdFifoRdPtr], eax
    xor eax, eax
    mov [fs:edi + SST_cmdFifoDepth], eax
    mov eax, 100h
    mov [fs:edi + SST_fbiInit7], eax

    mov edi, FIFO_WIN                   ; the header alone: one value to come
    mov dword [fs:edi], 00010291h       ; color1

    ; wait for the chip to take the header and park wanting the value, so
    ; what follows measures the hold and not the consumer's head start
    mov edx, FIFO_BASE + 4
    mov si, str_pp_head
    call fifo_wait

    mov edi, BAR
    mov ecx, 256
.poll:
    mov eax, [fs:edi + SST_cmdFifoRdPtr]
    dec ecx
    jnz .poll
    push eax
    mov si, str_pp_held
    call puts
    pop eax
    call puthex32
    call putnl

    mov edi, FIFO_WIN                   ; and now the value
    mov dword [fs:edi + 4], 0F80000h    ;   red
    mov edx, FIFO_BASE + 8
    mov si, str_pp_done
    call fifo_wait

    mov edi, BAR                        ; the ring as the ordering phase left
    xor eax, eax                        ; it, for the teardown below
    mov [fs:edi + SST_fbiInit7], eax
    mov eax, (FIFO_BASE >> 12) | (((FIFO_BASE + FIFO_SIZE - 1000h) >> 12) << 16)
    mov [fs:edi + SST_cmdFifoBaseAddr], eax
    mov eax, FIFO_BASE + 34h + ORDER_FILLS * 8
    mov [fs:edi + SST_cmdFifoRdPtr], eax
    xor eax, eax
    mov [fs:edi + SST_cmdFifoDepth], eax
    mov eax, 100h
    mov [fs:edi + SST_fbiInit7], eax
    ret

; ------------------------------------------------- the teardown phase
;
; What Glide does at grSstWinClose: the last packets, then fbiInit7 with
; the command-FIFO bit clear. On the chip the register write is behind them
; on the same bus. Here 86Box's consumer stops the moment the bit goes, so
; anything it has not reached is never run -- and the status register's
; busy bit is `cmdfifo_depth_rd != cmdfifo_depth_wr`, so the card then
; reads busy to every later poll and Glide's own idle wait never returns
; (Carmageddon's 3dfx build hung on quit for exactly that, 2026-09-18).
; The phase writes a cyan fill and a swap, turns the FIFO off with nothing
; waited for, and then polls status the way grSstIdle does: the frame has
; to be cyan (the packets were run, not dropped) and the card has to go
; idle (bits 9:7 clear).
;
; The idle poll ends the moment the swap completes, and the swap completes
; at a retrace: the frame it swapped in is scanned out over the *next*
; frame period, line by line, and the console shows what has been scanned
; out. So the marker the host takes its screendump on must not go out until
; that period is over -- announced at once, a dump within ~16 ms of it is
; the previous scene whole, with the status idle and the ring consumed
; (2026-09-21: what looked like the fill and swap no longer presenting after
; the swapped-pair phase was this, and polling the serial log every 1 ms
; instead of every 100 ms makes it fail every time). Every other phase's
; marker follows fifo_wait's half-second; this one has its own.
teardown_phase:
    mov edi, FIFO_WIN + 34h + ORDER_FILLS * 8
    mov dword [fs:edi], 00010291h       ; color1
    mov dword [fs:edi + 4], 00F8F8h     ;   cyan
    mov dword [fs:edi + 8], 00010249h   ; fastfillCMD
    mov dword [fs:edi + 12], 0
    mov dword [fs:edi + 16], 00010251h  ; swapbufferCMD
    mov dword [fs:edi + 20], 1
    call swap_note

    mov edi, BAR
    xor eax, eax                ; fbiInit7: the command FIFO off, at once
    mov [fs:edi + SST_fbiInit7], eax

    mov ecx, 200000             ; grSstIdle: poll until the busy bits clear
.poll:
    mov eax, [fs:edi + SST_status]
    test eax, 380h
    jz .idle
    dec ecx
    jnz .poll
.idle:
    push eax
    mov si, str_td_status
    call puts
    pop eax
    call puthex32
    call putnl
    mov cx, 9                   ; ~0.5 s: the swapped-in frame's scanout
    call delay_ticks
    mov si, str_td_swapped
    call puts
    mov cx, 36                  ; ~2 s: the host takes its screendump
    call delay_ticks
    ret

; --------------------------------------------- the stranded-client phase
;
; What a Glide whose FIFO was switched off under it does (doc 21 §11): it
; goes on writing command-FIFO packets into the 0x200000 window, and with
; the FIFO off 86Box decodes each of them as the register at bits 9:2 --
; the ring's own base, videoDimensions, fbiInit7 -- which destroys the card
; for everything after it. The device refuses them by default
; (`fifo-off-regs=on` is the control, the walk as 86Box decodes it). The
; phase runs with the FIFO already off (the teardown left it so): it reads
; cmdFifoBaseAddr, writes a burst into the window at the offsets of that
; register, videoDimensions and fbiInit7, and reads it back. Refused, the
; ring is where it was; decoded, it is wherever the burst put it.
stranded_phase:
    mov edi, BAR
    mov eax, [fs:edi + SST_cmdFifoBaseAddr]
    push eax
    mov si, str_st_before
    call puts
    pop eax
    call puthex32
    call putnl

    mov edi, FIFO_WIN
    mov eax, 0DEADBEEFh
    mov [fs:edi + SST_cmdFifoBaseAddr], eax
    mov [fs:edi + SST_videoDimensions], eax
    mov [fs:edi + SST_fbiInit7], eax

    mov edi, BAR
    mov eax, [fs:edi + SST_cmdFifoBaseAddr]
    push eax
    mov si, str_st_after
    call puts
    pop eax
    call puthex32
    call putnl
    ret

; 3dfx's Glide writes swapbufferCMD to the register window as well as
; putting the packet in the ring, and the measured stream of a real game
; does one of each per frame. With the FIFO on the register write only
; counts -- 86Box's `cmd_written`, and the swap backlog Glide reads out of
; the status register's bits 31:28 -- and the packet is what swaps. It is
; not decoration here: only Banshee and later count a *swap packet* as
; written (`cmd_written_fifo`), while every card's swap decrements
; `cmd_read`, so a program that sends the packet alone drives
; `written - cmd_read` negative and the status register's busy bit is that
; difference. The card then reads busy for ever, with nothing busy.
swap_note:
    push edi
    push eax
    mov edi, BAR
    mov eax, 1
    mov [fs:edi + SST_swapbufferCMD], eax
    pop eax
    pop edi
    ret

; EDX = where the chip's read pointer has to end up, SI = the batch's label:
; poll cmdFifoRdPtr until it gets there (bounded), print what it read, and
; give the swap its retrace
fifo_wait:
    mov edi, BAR
    mov ecx, 2000000
.poll:
    mov eax, [fs:edi + SST_cmdFifoRdPtr]
    cmp eax, edx
    je .there
    dec ecx
    jnz .poll
.there:
    call puts
    call puthex32
    call putnl
    mov cx, 9                   ; ~0.5 s
    call delay_ticks
    ret

a20_on:
    in al, 92h
    or al, 2
    out 92h, al
    ret

; Unreal mode: FS gets a 4 GiB limit from a protected-mode descriptor
; and keeps it after the switch back (a real-mode segment load changes
; the base, never the cached limit). No memory manager is loaded, so the
; CPU is really in real mode and the switch is legal.
unreal_mode:
    cli
    xor eax, eax
    mov ax, cs
    shl eax, 4
    add eax, gdt
    mov [gdtr + 2], eax
    lgdt [gdtr]
    mov eax, cr0
    or al, 1
    mov cr0, eax
    jmp short .pm
.pm:
    mov ax, 08h
    mov fs, ax
    mov eax, cr0
    and al, 0FEh
    mov cr0, eax
    jmp short .rm
.rm:
    sti
    ret

; CL = register, EAX = value / result; the device found by the scan
pci_addr:
    movzx eax, byte [pci_dev]
    shl eax, 11
    or eax, 80000000h
    movzx ecx, cl
    or eax, ecx
    mov dx, 0CF8h
    out dx, eax
    ret
pci_read:
    call pci_addr
    mov dx, 0CFCh
    in eax, dx
    ret
pci_write:
    push eax
    call pci_addr
    pop eax
    mov dx, 0CFCh
    out dx, eax
    ret

; CX = BIOS ticks (18.2/s) to wait
delay_ticks:
    push es
    push ax
    mov ax, 40h
    mov es, ax
    mov ax, [es:6Ch]
.wait:
    mov bx, [es:6Ch]
    sub bx, ax
    cmp bx, cx
    jb .wait
    pop ax
    pop es
    ret

; serial console on COM1 (QEMU -serial), so the runner can read it
putc:
    push dx
    push ax
    mov dx, 3FDh
.lsr:
    in al, dx
    test al, 20h
    jz .lsr
    pop ax
    mov dx, 3F8h
    out dx, al
    pop dx
    ret
puts:                           ; DS:SI, keeps EAX (a value follows it)
    push eax
.next:
    lodsb
    test al, al
    jz .done
    call putc
    jmp .next
.done:
    pop eax
    ret
putnl:
    push eax
    mov al, 10
    call putc
    pop eax
    ret
puthex32:                       ; EAX
    push ecx
    mov ecx, 8
.digit:
    rol eax, 4
    push eax
    and al, 0Fh
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 7
.out:
    call putc
    pop eax
    dec ecx
    jnz .digit
    pop ecx
    ret
puthex8:                        ; AL
    push ecx
    push eax
    mov ecx, 2
    shl eax, 24
.digit:
    rol eax, 4
    push eax
    and al, 0Fh
    add al, '0'
    cmp al, '9'
    jbe .out
    add al, 7
.out:
    call putc
    pop eax
    dec ecx
    jnz .digit
    pop eax
    pop ecx
    ret

align 8
gdt:
    dq 0
    dw 0FFFFh, 0000h
    db 00h, 92h, 0CFh, 00h      ; data, base 0, limit 4 GiB, 32-bit
gdtr:
    dw 15
    dd 0

; the dither phase's scratch and its tables
dith_x0:     dd 0
dith_x1:     dd 0
dith_y0:     dd 0
dith_y1:     dd 0
dith_a:      dd 0
dith_passes: dd 0
dith_cursor: dd 0
; the columns' left edges, as floats: 0, 64 ... 512, and one past the last
col_x:       dd 0, 42800000h, 43000000h, 43400000h, 43800000h
             dd 43A00000h, 43C00000h, 43E00000h, 44000000h
; each column's blend weight, as a float: 128, 96, 64, 48, 32, 24, 16, 12
col_a:       dd 43000000h, 42C00000h, 42800000h, 42400000h
             dd 42000000h, 41C00000h, 41800000h, 41400000h
; where each column is sampled: row 280, 32 pixels in from its left edge
col_px:      dd LFB + (280 << 11) + 64,  LFB + (280 << 11) + 192
             dd LFB + (280 << 11) + 320, LFB + (280 << 11) + 448
             dd LFB + (280 << 11) + 576, LFB + (280 << 11) + 704
             dd LFB + (280 << 11) + 832, LFB + (280 << 11) + 960

pci_dev:     dw 0
init_enable: dd 0
si_polls:    dd 0
d3d_vram:    dd 0
d3d_regs:    dd 0

str_hello:    db "VOODOO2 guest test", 10, 0
str_nocard:   db "NOCARD: no 121a:0002 on bus 0", 10, 0
str_found:    db "FOUND dev ", 0
str_bar:      db "BAR0 ", 0
str_init:     db "INITENABLE ", 0
str_si:       db "SIPROCESS ", 0
str_si_timeout: db "SIPROCESS TIMEOUT: the countdown never read zero", 10, 0
str_fbiinit0: db "FBIINIT0 ", 0
str_status:   db "STATUS ", 0
str_fill:     db "FILL 640x480 red", 10, 0
str_readback: db "READBACK ", 0
str_rb_pass:  db "LFB PASS", 10, 0
str_rb_fail:  db "LFB FAIL", 10, 0
str_swapped:  db "SWAPPED", 10, 0
str_done:     db "DONE", 10, 0
str_d3dpt:    db "D3DPT ", 0
str_d3d_enable: db "D3DPT ENABLE ", 0
str_d3d_ready:  db "D3DPT READY", 0
str_fifo1:    db "FIFO1 RDPTR ", 0
str_fifo1_swapped: db "FIFO1 SWAPPED", 10, 0
str_fifo2:    db "FIFO2 RDPTR ", 0
str_fifo2_swapped: db "FIFO2 SWAPPED", 10, 0
str_dith_rp:  db "DITH RDPTR ", 0
str_dith_ref: db "DITH REF ", 0
str_dith_col: db "DITH COL ", 0
str_dith_swap: db "DITH SWAP RDPTR ", 0
str_dith_shown: db "DITH SHOWN", 10, 0
str_order:    db "ORDER RDPTR ", 0
str_order_swapped: db "ORDER SWAPPED", 10, 0
str_td_status: db "TD STATUS ", 0
str_sp_plant: db "SP PLANTED ", 0
str_sp_nop: db "SP NOP ", 0
str_sp_done: db "SP WHOLE ", 0
str_sp_swapped: db "SP SWAPPED", 10, 0
str_wr_plant: db "WR PLANTED ", 0
str_wr_jump: db "WR JUMPED ", 0
str_wr_done: db "WR WHOLE ", 0
str_wr_swapped: db "WR SWAPPED", 10, 0
str_pp_head: db "PP HEAD ", 0
str_pp_held: db "PP HELD ", 0
str_pp_done: db "PP WHOLE ", 0
str_td_swapped: db "TD SWAPPED", 10, 0
str_st_before: db "ST BEFORE ", 0
str_st_after: db "ST AFTER ", 0
"""


def sh(*cmd, **kw):
    subprocess.run(cmd, check=True, **kw)


def build():
    asm = os.path.join(OUT, "voodoo.asm")
    com = os.path.join(OUT, "VOODOO.COM")
    img = os.path.join(OUT, "voodoo.img")
    with open(asm, "w") as f:
        f.write(ASM)
    sh("nasm", "-O0", "-f", "bin", "-o", com, asm)
    shutil.copy(x87gt.FLOPPY, img)
    cfg = os.path.join(OUT, "FDCONFIG.SYS")
    with open(cfg, "w") as f:
        # no HIMEM / EMM386: the program switches to unreal mode itself,
        # which a V86 monitor would refuse
        f.write("!LASTDRIVE=Z\r\n!BUFFERS=20\r\n!FILES=40\r\n"
                "SHELL=\\FREEDOS\\BIN\\COMMAND.COM \\FREEDOS\\BIN /E:2048 /P=\\FDAUTO.BAT\r\n")
    bat = os.path.join(OUT, "FDAUTO.BAT")
    with open(bat, "w") as f:
        f.write("@echo off\r\nVOODOO.COM\r\n")
    sh("mcopy", "-o", "-i", img, cfg, "::FDCONFIG.SYS")
    sh("mcopy", "-o", "-i", img, bat, "::FDAUTO.BAT")
    sh("mcopy", "-o", "-i", img, com, "::VOODOO.COM")
    return img


def wait_for(log, marker, p, timeout, what):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if p.poll() is not None:
            raise SystemExit("QEMU exited while waiting for %s" % what)
        if os.path.exists(log):
            text = open(log, "rb").read()
            if marker in text:
                return text.decode("latin-1")
            if b"NOCARD" in text:
                raise SystemExit("the guest found no Voodoo 2 on the PCI bus")
        time.sleep(0.1)
    raise SystemExit("timeout waiting for %s" % what)


def vga_args():
    if VGA == "d3dpt":
        return ["-vga", "none", "-device", "d3dpt-vga"]
    if VGA in ("std", "cirrus"):
        return ["-vga", VGA]
    raise SystemExit("VGA must be std, cirrus or d3dpt")


def fraction(path, want):
    """(width, height, fraction of pixels that are the colour `want` picks)."""
    w, h, px = vgadirty.read_ppm(path)
    n = w * h
    hit = 0
    for i in range(0, n * 3, 3):
        if want(px[i], px[i + 1], px[i + 2]):
            hit += 1
    return w, h, hit / n if n else 0.0


def red_fraction(path):
    """The CLUT-ramped red the Voodoo draws."""
    return fraction(path, lambda r, g, b: r >= 240 and g < 8 and b < 8)


def green_fraction(path):
    """The green the program leaves d3dpt-vga's linear mode showing."""
    return fraction(path, lambda r, g, b: r < 8 and g >= 248 and b < 8)


def blue_fraction(path):
    """The command FIFO's first fastfill."""
    return fraction(path, lambda r, g, b: r < 8 and g < 8 and b >= 240)


def cyan_fraction(path):
    """The teardown phase's fastfill."""
    return fraction(path, lambda r, g, b: r < 8 and g >= 240 and b >= 240)


def magenta_fraction(path):
    """The command FIFO's second fastfill."""
    return fraction(path, lambda r, g, b: r >= 240 and g < 8 and b >= 240)


def yellow_fraction(path):
    """The swapped-pair phase's fastfill."""
    return fraction(path, lambda r, g, b: r >= 240 and g >= 240 and b < 8)


def green_fraction(path):
    """The wrap phase's fastfill."""
    return fraction(path, lambda r, g, b: r < 8 and g >= 240 and b < 8)


def off_tile_pixels(path):
    """Pixels of the dither scene that are not the 4x4 dither tile of its
    reference band (rows 100-103). The scene is one grey over the whole
    screen with columns blended onto themselves, so a chip that takes the
    dither out on every read-back leaves the whole frame one tile."""
    w, h, px = vgadirty.read_ppm(path)
    tile = {}
    for y in range(100, 104):
        for x in range(4):
            i = (y * w + x) * 3
            tile[(x & 3, y & 3)] = px[i:i + 3]
    bad = 0
    for y in range(h):
        row = [tile[(x, y & 3)] for x in range(4)]
        for x in range(w):
            i = (y * w + x) * 3
            if px[i:i + 3] != row[x & 3]:
                bad += 1
    return w, h, bad


def off_colour_pixels(path):
    """Pixels of the dither scene that are not the one colour the scene is.

    The scene is one grey over the whole screen (a blend of a colour onto
    itself is that colour), dithered on the way into the frame buffer, so
    with `undither=on` the frame has to come back *flat*, and at the colour
    that was rendered rather than near it. The interior only: the outermost
    two rows and columns see a window clamped at the edge of the screen,
    which has fewer than sixteen dither phases in it and so can land a level
    away."""
    w, h, px = vgadirty.read_ppm(path)
    seen = {}
    for y in range(2, h - 2):
        for x in range(2, w - 2):
            i = (y * w + x) * 3
            c = bytes(px[i:i + 3])
            seen[c] = seen.get(c, 0) + 1
    best = max(seen, key=seen.get) if seen else b""
    off = sum(n for c, n in seen.items() if c != best)
    return w, h, off, tuple(best)


def main():
    x87gt.ensure_prereqs()
    x87gt.ensure_floppy()
    os.makedirs(OUT, exist_ok=True)
    img = build()
    log = os.path.join(OUT, "serial.log")
    qlog = os.path.join(OUT, "qemu.log")
    # short: a socket path deep in a scratch tree is over AF_UNIX's limit
    # (a loopback port on Windows, tools/qemuhost.py)
    sock = qemuhost.addr("/tmp", "voodoo-guest-%d" % os.getpid())
    shot_on = os.path.join(OUT, "voodoo.ppm")
    shot_off = os.path.join(OUT, "vga.ppm")
    shot_lin = os.path.join(OUT, "linear.ppm")
    shot_f1 = os.path.join(OUT, "fifo1.ppm")
    shot_f2 = os.path.join(OUT, "fifo2.ppm")
    shot_dith = os.path.join(OUT, "dither.ppm")
    shot_order = os.path.join(OUT, "order.ppm")
    shot_sp = os.path.join(OUT, "swapped.ppm")
    shot_wr = os.path.join(OUT, "wrap.ppm")
    shot_td = os.path.join(OUT, "teardown.ppm")
    for f in (log, qlog, shot_on, shot_off, shot_lin, shot_f1, shot_f2, shot_dith,
              shot_order, shot_sp, shot_wr, shot_td, sock):
        if os.path.exists(f):
            os.unlink(f)
    ok = True
    with open(qlog, "wb") as qf:
        p = subprocess.Popen([
            QEMU, "-machine", "pc", "-cpu", "pentium3", "-m", "64",
            "-L", os.path.join(ROOT, "qemu/pc-bios"), "-display", "none", "-net", "none",
            *vga_args(),
            # the cursor the console publishes (patch 66), in this log
            *(["-trace", "dpy_mouse_publish"] if VGA == "d3dpt" else []),
            "-device",
            "voodoo2,ramfifo=%s,recompiler=%s,dither-sub=%s,undither=%s,lfb-order=%s,"
            "fifo-off-regs=%s,mmio-holes=%s"
            % (RAMFIFO, RECOMP, DITHER_SUB, UNDITHER, LFB_ORDER, FIFO_OFF_REGS, MMIO_HOLES),
            "-drive", "file=%s,if=floppy,index=0,format=raw" % img,
            "-boot", "a", "-serial", "file:" + log, "-monitor", "none",
            "-qmp", qemuhost.qemu_opt(sock), "-audiodev", "none,id=a0",
        ], stdout=qf, stderr=subprocess.STDOUT)
        t0 = time.time()
        try:
            q = vgadirty.Qmp(sock)
            if VGA == "d3dpt":
                # headless, nothing refreshes the console on its own: this
                # dump is what makes the adapter put its linear surface up
                # before the Voodoo takes the monitor, as the player's
                # refresh does. Without it the hand-back finds no linear
                # surface of the adapter's and the bug cannot show
                wait_for(log, b"D3DPT READY", p, 180, "the linear mode, after its flips")
                q.screendump(shot_lin)
            text = wait_for(log, b"SWAPPED", p, 180, "the guest's swap")
            q.screendump(shot_on)
            text = wait_for(log, b"FIFO1 SWAPPED", p, 60, "the command FIFO's first batch")
            q.screendump(shot_f1)
            text = wait_for(log, b"FIFO2 SWAPPED", p, 60, "the command FIFO's second batch")
            q.screendump(shot_f2)
            text = wait_for(log, b"DITH SHOWN", p, 180, "the dither scene")
            q.screendump(shot_dith)
            text = wait_for(log, b"ORDER SWAPPED", p, 120, "the ordering scene")
            q.screendump(shot_order)
            text = wait_for(log, b"SP SWAPPED", p, 120, "the swapped-pair scene")
            q.screendump(shot_sp)
            text = wait_for(log, b"WR SWAPPED", p, 120, "the wrap scene")
            q.screendump(shot_wr)
            text = wait_for(log, b"TD SWAPPED", p, 120, "the teardown scene")
            q.screendump(shot_td)
            text = wait_for(log, b"DONE", p, 60, "the guest to finish")
            q.screendump(shot_off)
        finally:
            if p.poll() is None:
                p.terminate()
                p.wait()
            if os.path.exists(sock):
                os.unlink(sock)
    print("voodoo-guest: %.0f s in the guest, beside VGA=%s" % (time.time() - t0, VGA))
    for line in text.splitlines():
        if line.strip():
            print("   ", line.strip())
    for line in open(qlog, "rb").read().decode("latin-1").splitlines():
        if "voodoo2" in line:
            print("   ", line.strip())

    # the guest's own findings
    if "INITENABLE 00005001" not in text:
        print("FAIL initEnable did not read back the Voodoo 2 strap (0x50) and bit 0")
        ok = False
    # siProcess: the countdown read zero (the loop ended) and the
    # oscillator count is a real one (RUN still set, bits 15:0 nonzero)
    si = [l.split()[1] for l in text.splitlines() if l.startswith("SIPROCESS ") and len(l.split()) == 2]
    if not si or (int(si[0], 16) & 0x1fffffff) <= 0x10000000 or (int(si[0], 16) & 0x0fff0000):
        print("FAIL siProcess: %s (Glide polls it until bits 27:16 read zero, then takes bits 15:0)"
              % (si[0] if si else "no reading: the countdown never ended"))
        ok = False
    if "FBIINIT0 00000001" not in text:
        print("FAIL fbiInit0 did not read back VGA pass-through")
        ok = False
    if "LFB PASS" not in text:
        print("FAIL the LFB did not read back what was written")
        ok = False
    # and the host's: the frame on the console while the Voodoo has the
    # monitor, and the VGA's screen once it gives it back
    w, h, frac = red_fraction(shot_on)
    print("    screendump with the Voodoo on: %dx%d, %.1f%% red" % (w, h, frac * 100))
    if (w, h) != (WIDTH, HEIGHT) or frac < 0.99:
        print("FAIL the console did not show the Voodoo's 640x480 red frame")
        ok = False
    # the command FIFO: the read pointer where the packets end (the jump
    # followed, every word counted once), and what the packets drew
    for name, want, shot, frac_of, colour in (
            ("FIFO1", "00300010", shot_f1, blue_fraction, "blue"),
            ("FIFO2", "00300028", shot_f2, magenta_fraction, "magenta")):
        rd = [l.split()[2] for l in text.splitlines()
              if l.startswith(name + " RDPTR ") and len(l.split()) == 3]
        if rd != [want]:
            print("FAIL %s: the chip's read pointer ended at %s, not %s"
                  % (name, rd[0] if rd else "nothing", want))
            ok = False
        w, h, frac = frac_of(shot)
        print("    screendump after %s: %dx%d, %.1f%% %s" % (name, w, h, frac * 100, colour))
        if (w, h) != (WIDTH, HEIGHT) or frac < 0.99:
            print("FAIL the command FIFO's %s batch did not draw its %s frame"
                  % (name, colour))
            ok = False
    # the dither phase: a colour blended onto itself is that colour, so
    # every column reads the reference band's pixel and the frame is one
    # dither tile, which it is only if the read-back is un-dithered
    # (patch 64 for the recompilers; the interpreter always did)
    ref = [l.split()[2] for l in text.splitlines()
           if l.startswith("DITH REF ") and len(l.split()) == 3]
    cols = [l.split()[3] for l in text.splitlines()
            if l.startswith("DITH COL ") and len(l.split()) == 4]
    if len(ref) != 1 or len(cols) != 8 or any(c != ref[0] for c in cols):
        print("FAIL the dither scene's columns (%s) are not the reference (%s): "
              "a blend read-back kept its dither" % (" ".join(cols), ref[0] if ref else "none"))
        ok = False
    if UNDITHER == "on":
        # the same scene through the undither (doc 21 §12): the dither the
        # rasterizer put in is reconstructed away at scanout, so the frame is
        # not a tile any more, it is one colour
        w, h, off, colour = off_colour_pixels(shot_dith)
        print("    screendump of the dither scene: %dx%d, %d interior pixels are not "
              "the one colour %s" % (w, h, off, colour))
        if (w, h) != (WIDTH, HEIGHT) or off:
            print("FAIL the undithered dither scene is not one flat colour")
            ok = False
        if colour and max(colour) - min(colour) > 2:
            print("FAIL the undithered scene's colour %s is not the grey that was drawn"
                  % (colour,))
            ok = False
    else:
        w, h, bad = off_tile_pixels(shot_dith)
        print("    screendump of the dither scene: %dx%d, %d pixels off the dither tile" % (w, h, bad))
        if (w, h) != (WIDTH, HEIGHT) or bad:
            print("FAIL the dither scene is not one dither tile")
            ok = False
    # the ordering phase: the LFB block the guest wrote after the ring's
    # fastfill has to be on top of it, not under it ("one order" in
    # voodoo/voodoo2.c). Out of order the frame is all red.
    w, h, blue = blue_fraction(shot_order)
    _, _, red = red_fraction(shot_order)
    print("    screendump of the ordering scene: %dx%d, %.1f%% blue on %.1f%% red"
          % (w, h, blue * 100, red * 100))
    if (w, h) != (WIDTH, HEIGHT) or blue < 0.20 or red < 0.60:
        print("FAIL the LFB block the guest wrote after the ring's fastfill is not on "
              "top of it: the two queues ran out of the guest's order")
        ok = False
    # and the other direction, the one a game meets: the LFB block was
    # written before the swap packet, so 86Box's own queue has to be empty
    # before the ring's publish point lets that swap through, on either
    # transport (with ramfifo=off the publish point is the ring-window write
    # itself). The device counts the times it waited; the scene above is
    # built to make one, and none means the ordering point was never
    # reached.
    behind = sum(int(m) for m in re.findall(r"(\d+) publishes behind the LFB queue",
                 open(qlog, "rb").read().decode("latin-1")))
    print("    ring publishes with 86Box's own FIFO not yet empty: %d" % behind)
    if not behind:
        print("FAIL the ring published packets with no LFB write ever queued behind "
              "them: the ordering scene did not reach the point it is about")
        ok = False
    # the mmio-holes=off control leaves the consumer parked inside the stale
    # packet, eating whatever follows as its values (below): the teardown
    # and partial-packet phases after the swapped pair are not judged
    # there, they cannot mean anything
    judged = not (MMIO_HOLES == "off" and RAMFIFO == "off")
    fail = "FAIL" if judged else "    (not judged, the control:)"
    # the teardown: the packets written before the FIFO was turned off were
    # run (the frame is cyan), and the card reads idle afterwards rather
    # than busy for ever (Glide's own wait at grSstWinClose)
    td = [l.split()[2] for l in text.splitlines()
          if l.startswith("TD STATUS ") and len(l.split()) == 3]
    print("    status after the command FIFO was turned off: %s"
          % (td[0] if td else "nothing"))
    if len(td) != 1 or int(td[0], 16) & 0x380:
        print(fail + " the card still reads busy with the command FIFO off: a guest "
              "polling for idle there never gets out")
        ok = False if judged else ok
    # the swapped pair: a packet's value written before its header, with the
    # consumer resting on the header's slot and a stale word planted there.
    # Counted per write (86Box's transport as it came), the consumer read the
    # stale word as the header the moment the value landed and ate the fill
    # and the swap as its values; counted as the chip counts, contiguously,
    # the pair goes in whole and the frame is the fill.
    sp_p = [l.split()[2] for l in text.splitlines()
            if l.startswith("SP PLANTED ") and len(l.split()) == 3]
    sp_n = [l.split()[2] for l in text.splitlines()
            if l.startswith("SP NOP ") and len(l.split()) == 3]
    sp_d = [l.split()[2] for l in text.splitlines()
            if l.startswith("SP WHOLE ") and len(l.split()) == 3]
    w, h, frac = yellow_fraction(shot_sp)
    print("    swapped pair: read pointer after the planted lap %s (want %08X), "
          "resting %s (want %08X), after the pair, fill and swap %s (want %08X); "
          "screendump %dx%d, %.1f%% yellow"
          % (sp_p[0] if sp_p else "nothing", FIFO_BASE + 8,
             sp_n[0] if sp_n else "nothing", FIFO_BASE + 4,
             sp_d[0] if sp_d else "nothing", FIFO_BASE + 28, w, h, frac * 100))
    if len(sp_p) != 1 or len(sp_n) != 1 or len(sp_d) != 1:
        print("FAIL the swapped-pair phase did not report the read pointer")
        ok = False
    elif int(sp_p[0], 16) != FIFO_BASE + 8 or int(sp_n[0], 16) != FIFO_BASE + 4:
        print("FAIL the consumer was not at rest on the planted slot when the "
              "value was written: the phase never reached the state it is about")
        ok = False
    if MMIO_HOLES == "on" and RAMFIFO == "on" and frac >= 0.99:
        pass    # in RAM the restart poisons the planted slot: nothing is stale
    elif MMIO_HOLES == "on" and ((w, h) != (WIDTH, HEIGHT) or frac < 0.99):
        print("FAIL a packet written value first, header second was read with a "
              "stale header: the ring is counted per write, not as the chip "
              "counts it, and the fill and swap after it were eaten as data")
        ok = False
    elif MMIO_HOLES == "off" and RAMFIFO == "off" and frac >= 0.99:
        print("FAIL mmio-holes=off did not take the stale word as the header: "
              "the control proves nothing")
        ok = False
    if not judged:
        print("    (mmio-holes=off: the wrap, teardown and partial-packet phases are "
              "not judged, the consumer is parked inside the stale packet)")
    # the wrap: the same pair, value first, at the ring's base right after
    # the guest's JMP back to it, with a stale header there from the lap
    # before. The write side has to follow the JMP to know the base is
    # where the contiguous count goes on (Glide never says so through
    # cmdFifoAMin); taking the value's slot as that point instead is what
    # held every word of the next lap until the hole bitmap ran out.
    wr_p = [l.split()[2] for l in text.splitlines()
            if l.startswith("WR PLANTED ") and len(l.split()) == 3]
    wr_j = [l.split()[2] for l in text.splitlines()
            if l.startswith("WR JUMPED ") and len(l.split()) == 3]
    wr_d = [l.split()[2] for l in text.splitlines()
            if l.startswith("WR WHOLE ") and len(l.split()) == 3]
    w, h, frac = green_fraction(shot_wr)
    print("    wrap: read pointer after the plant %s (want %08X), after the jump "
          "%s (want %08X), after the pair, fill and swap %s (want %08X); "
          "screendump %dx%d, %.1f%% green"
          % (wr_p[0] if wr_p else "nothing", FIFO_BASE + 0x404,
             wr_j[0] if wr_j else "nothing", FIFO_BASE,
             wr_d[0] if wr_d else "nothing", FIFO_BASE + 24, w, h, frac * 100))
    if len(wr_p) != 1 or len(wr_j) != 1 or len(wr_d) != 1:
        print(fail + " the wrap phase did not report the read pointer")
        ok = False if judged else ok
    elif int(wr_p[0], 16) != FIFO_BASE + 0x404 or int(wr_j[0], 16) != FIFO_BASE:
        print(fail + " the consumer was not at rest on the base after the jump "
              "when the value was written: the phase never reached the state it "
              "is about")
        ok = False if judged else ok
    if judged and ((w, h) != (WIDTH, HEIGHT) or frac < 0.99):
        print("FAIL a packet written value first at the ring's base after a jump "
              "was read with a stale header: the write side did not follow the "
              "JMP, and the fill and swap after it were lost")
        ok = False
    w, h, frac = cyan_fraction(shot_td)
    print("    screendump of the teardown scene: %dx%d, %.1f%% cyan" % (w, h, frac * 100))
    if (w, h) != (WIDTH, HEIGHT) or frac < 0.99:
        print(fail + " the packets written before the command FIFO was turned off were "
              "not run")
        ok = False if judged else ok
    # the partial packet: the read pointer must never pass what the guest
    # has written. The header alone is consumed and the chip parks wanting
    # the value, one word in; then the value arrives and the packet
    # completes. Taking the rest on a guess instead is what deadlocked FIFA
    # 2000's loading screen: the guest's free space is
    # `rp - wp - 1`, so a pointer past the write pointer leaves it waiting
    # for room on an empty ring.
    pp_c = [l.split()[2] for l in text.splitlines()
            if l.startswith("PP HEAD ") and len(l.split()) == 3]
    pp_h = [l.split()[2] for l in text.splitlines()
            if l.startswith("PP HELD ") and len(l.split()) == 3]
    pp_d = [l.split()[2] for l in text.splitlines()
            if l.startswith("PP WHOLE ") and len(l.split()) == 3]
    print("    read pointer with a packet's header written and its value not: "
          "%s (want %08X), and once the value arrived: %s (want %08X)"
          % (pp_h[0] if pp_h else "nothing", FIFO_BASE + 4,
             pp_d[0] if pp_d else "nothing", FIFO_BASE + 8))
    if len(pp_c) != 1 or len(pp_h) != 1 or len(pp_d) != 1:
        print(fail + " the partial-packet phase did not report the read pointer")
        ok = False if judged else ok
    else:
        if int(pp_c[0], 16) != FIFO_BASE + 4:
            print(fail + " the chip did not stop on the header (%s): the phase never "
                  "reached the state it is about" % pp_c[0])
            ok = False if judged else ok
        if int(pp_h[0], 16) != FIFO_BASE + 4:
            print(fail + " the read pointer passed the words the guest had written: "
                  "the walk took the rest of the packet on a guess, which leaves "
                  "a guest waiting for room on an empty ring")
            ok = False if judged else ok
        if int(pp_d[0], 16) != FIFO_BASE + 8:
            print(fail + " the packet did not complete once its last word arrived")
            ok = False if judged else ok
    # the stranded client: a burst into the command-FIFO window with the
    # FIFO off must leave the ring's own register where it was. With
    # fifo-off-regs=on it is decoded as a register write instead, the walk
    # 86Box does and the chip is said to do. That is the control, which
    # must move it. That walk is what left a card unusable after a game's
    # close (FIFA 2000 on base98-br).
    st_b = [l.split()[2] for l in text.splitlines()
            if l.startswith("ST BEFORE ") and len(l.split()) == 3]
    st_a = [l.split()[2] for l in text.splitlines()
            if l.startswith("ST AFTER ") and len(l.split()) == 3]
    print("    cmdFifoBaseAddr across a burst into the window with the FIFO off: "
          "%s -> %s (fifo-off-regs=%s)"
          % (st_b[0] if st_b else "nothing", st_a[0] if st_a else "nothing",
             FIFO_OFF_REGS))
    if len(st_b) != 1 or len(st_a) != 1:
        print("FAIL the stranded-client phase did not report the ring's register")
        ok = False
    elif FIFO_OFF_REGS == "off" and st_a[0] != st_b[0]:
        print("FAIL a command-FIFO packet met with the FIFO off moved the ring: "
              "the burst walked the register file")
        ok = False
    elif FIFO_OFF_REGS == "on" and st_a[0] == st_b[0]:
        print("FAIL fifo-off-regs=on did not decode the burst as register writes: "
              "the control proves nothing")
        ok = False
    w, h, frac = red_fraction(shot_off)
    print("    screendump with the Voodoo off: %dx%d, %.1f%% red" % (w, h, frac * 100))
    if frac > 0.5:
        print("FAIL the console did not go back to the VGA")
        ok = False
    if VGA == "d3dpt":
        # the linear desktop the program left behind, not the Voodoo's last
        # frame on a surface the adapter kept updating
        if "D3DPT ENABLE 00000001" not in text:
            print("FAIL the program did not find d3dpt-vga or its ENABLE did not read back")
            ok = False
        w, h, frac = green_fraction(shot_lin)
        print("    screendump before the Voodoo: %dx%d, %.1f%% green" % (w, h, frac * 100))
        if (w, h) != (800, 600) or frac < 0.99:
            print("FAIL d3dpt-vga did not show its 800x600 linear mode before the Voodoo")
            ok = False
        w, h, frac = green_fraction(shot_off)
        print("    ... and %.1f%% the linear mode's green" % (frac * 100))
        if (w, h) != (800, 600) or frac < 0.99:
            print("FAIL the console did not go back to d3dpt-vga's 800x600 linear mode")
            ok = False
        # the adapter's hardware cursor (patch 66): shown before the Voodoo
        # takes the monitor, hidden while it has it (a pass-through cable
        # shows nothing of the 2D card, and Windows' desktop pointer was
        # drawn over Moto Racer on the Voodoo 2), shown again after
        # (the switch is published before the device logs it, so each
        # line is placed by its own passthrough field)
        cur, seen = [], False
        for line in open(qlog, "rb").read().decode("latin-1").splitlines():
            m = re.search(r"dpy_mouse_publish x=(-?\d+) y=(-?\d+) on=(\d) passthrough=(\d)", line)
            if m:
                on, through = int(m.group(3)), int(m.group(4))
                seen = seen or through == 1
                cur.append(("during" if through else "after" if seen else "before", on, through))
        last = {k: [c for c in cur if c[0] == k][-1:] for k in ("before", "during", "after")}
        # before the Voodoo: shown, hidden by the flip, shown again by the
        # mode set; hidden by the next flip, kept hidden while that chain
        # idles on its other page, shown again once it idles on the
        # desktop's (in that order, exactly; repeats of a state are fine)
        seq = []
        for c in cur:
            if c[0] == "before" and (not seq or seq[-1] != c[1]):
                seq.append(c[1])
        print("    cursor before the Voodoo, in order: %s" % seq)
        if 1 not in seq or seq[seq.index(1):] != [1, 0, 1, 0, 1]:
            print("FAIL the adapter's cursor was not hidden by a page flip and shown again by the "
                  "mode set, then hidden by a flip and shown again only once the flips stopped on "
                  "the desktop's page")
            ok = False
        if "the flip chain is gone" not in open(qlog, "rb").read().decode("latin-1"):
            print("FAIL d3dpt-vga did not log the idle flip chain it gave the cursor back from")
            ok = False
        print("    cursor published: before %s, during %s, after %s" % (
            last["before"] or "-", last["during"] or "-", last["after"] or "-"))
        if not last["before"] or last["before"][0][1] != 1:
            print("FAIL the adapter's cursor was not published as shown before the Voodoo")
            ok = False
        if not last["during"] or last["during"][0][1] != 0:
            print("FAIL the adapter's cursor stayed visible while the Voodoo had the monitor")
            ok = False
        if not last["after"] or last["after"][0][1] != 1:
            print("FAIL the adapter's cursor did not come back with the monitor")
            ok = False
    qtext = open(qlog, "rb").read().decode("latin-1")
    if "display on (VGA pass-through)" not in qtext or "display off (VGA back)" not in qtext:
        print("FAIL the device did not log both pass-through switches")
        ok = False
    in_ram = "command FIFO in RAM (ring 00300000+2000)" in qtext
    if in_ram != (RAMFIFO == "on"):
        print("FAIL the ring was %s with ramfifo=%s"
              % ("mapped as RAM" if in_ram else "not mapped as RAM", RAMFIFO))
        ok = False
    if "command FIFO back to MMIO" in qtext:
        print("FAIL the device gave up on a packet (see the warning above)")
        ok = False
    if not ok:
        raise SystemExit("voodoo-guest-test: failed")
    print("voodoo-guest-test: passed")


if __name__ == "__main__":
    main()
