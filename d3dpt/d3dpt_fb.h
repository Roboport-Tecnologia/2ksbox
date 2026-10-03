/*
 * d3dpt_fb.h: the paravirtual framebuffer register set of the d3dpt-vga
 * display adapter (doc 15, ADR-008 / M7a).
 *
 * ONE header for both sides: the QEMU device model (d3dpt/hw/d3dpt_vga.c)
 * and the XP video miniport (guest-tools/src/d3dptvid/nt/d3dptvid.c). Plain
 * C, fixed-width types only: the miniport is a kernel-mode PE with no
 * CRT and includes this next to video.h.
 *
 * The adapter is a PCI VGA (class 03.00, QEMU's standard VGA core with
 * the Bochs VBE ports, so SeaBIOS' stdvga ROM boots it and XP's inbox
 * vga.sys drives it until our driver is installed) plus one MMIO BAR of
 * paravirtual registers:
 *
 *   BAR 0  VRAM, prefetchable, D3DPT_FB_VRAM_MB (power of two)
 *   BAR 1  this register page, 4 KiB, 32-bit accesses
 *
 * M7c (version 2): the top D3DPT_SHM_SIZE (64 MiB) of BAR 0 is the
 * Direct3D command window in the d3dpt_proto.h layout (CMD_OFFSET says
 * where it starts; 0 = no Direct3D on this adapter). The display driver
 * appends records there and writes DOORBELL; the host executes the batch
 * synchronously inside the write, reading texels from and writing frames
 * into the VRAM below the window. D3D_STATUS says whether the host has
 * an executor (reading it loads the library). The DirectDraw heap the
 * driver exposes ends at CMD_OFFSET.
 *
 * -device d3dpt-vga,no-exec=on makes the adapter answer D3D_STATUS as a
 * host below ADR-013's Vulkan 1.3 floor does: the command window is there,
 * nothing on the host can run it, so the driver keeps its DirectDraw half
 * and offers no Direct3D. That is how one of those hosts is tested from a
 * host that has Vulkan. It is not DDFLAGS bit 0x20 (DDF_NO_D3D), which
 * makes the *driver* decide and never reads D3D_STATUS at all.
 *
 * The guest driver reads the host's mode table (MODE_COUNT, then MODE_SEL
 * + MODE_W/H/BPP/HZ per entry), programs a linear mode (W, H, BPP, PITCH,
 * OFFSET into VRAM) and sets ENABLE = 1; the VGA core is bypassed while
 * ENABLE is set and the console shows VRAM directly (no copy inside QEMU,
 * dirty pages from the memory log). ENABLE = 0 hands the console back to
 * the VGA core (text mode for BSODs / reboot via the BIOS).
 *
 * Version 3: 8 bpp palettized modes. BPP = 8 shows VRAM bytes as indices
 * into the 256-entry PALETTE block (x8r8g8b8 per entry, written by the
 * miniport's IOCTL_VIDEO_SET_COLOR_REGISTERS and by the display driver
 * for DirectDraw palettes); a palette write takes effect at the next
 * refresh, for the whole frame. The 2D DirectDraw titles of the late 90s
 * (StarCraft, Diablo, Age of Empires) set 640x480x8 and animate the
 * palette.
 *
 * Version 4: a hardware cursor. The display driver's DrvSetPointerShape
 * writes the pointer as 0xAARRGGBB pixels into VRAM (anywhere below the
 * command window; it reserves the top of its DirectDraw heap), describes
 * it in CURSOR_ADDR / W / H / HOT_X / HOT_Y and writes CURSOR_DEFINE;
 * DrvMovePointer writes CURSOR_X / Y and CURSOR_ENABLE. The device hands
 * the shape and the position to QEMU's console (dpy_cursor_define /
 * dpy_mouse_set), the embed library to the player, which shows the
 * guest's shape as the host window's cursor. Nothing is composited into
 * the frame, and headless screendumps show no cursor, as with a real
 * sprite. Without it GDI paints a software pointer into the primary,
 * which flickers under a flip chain (it is in one buffer of the two).
 *
 * Version 5: gamma ramps (GAMMA_ENABLE and the GAMMA block).
 *
 * Version 6: interrupts (IRQ_ENABLE, IRQ_STATUS), for the WDDM driver
 * (M18): the vertical blank, at the mode's refresh, on the PCI INTx pin
 * that -device d3dpt-vga,irq=on gives the adapter (CAP_IRQ says it is
 * there). Level triggered and possibly shared: a set STATUS bit that is
 * enabled holds the line until the driver writes it back.
 *
 * **Versions only add.** Every driver (the XP miniport, the 9x display
 * driver and mini-VDD) accepts any VERSION at or above the one it was
 * built with and refuses only an older one, because a newer register set is
 * its own plus registers it never touches (a feature is found by its CAP bit
 * or its version, never by a register changing meaning). That is what lets
 * an installed guest survive a QEMU update: until 2026-09-12 the drivers
 * wanted the version exactly, and a Windows 98 machine with a v4 driver
 * died at boot with "Windows protection error" on the v5 adapter. So a
 * bump must never move, resize or reinterpret an existing register; a
 * change that has to is a new MAGIC, which every driver does compare
 * exactly. -device d3dpt-vga,fb-version=N reports another version, to
 * check exactly this.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_FB_H
#define D3DPT_FB_H

#include <stdint.h>

#define D3DPT_FB_VERSION      6u
#define D3DPT_FB_MAGIC        0x42463344u          /* "D3FB" at REG_MAGIC */

/* PCI identity: QEMU/Bochs pseudo vendor, our device id ("3D00"). The INF
 * matches PCI\VEN_1234&DEV_3D00. */
#define D3DPT_FB_PCI_VENDOR   0x1234u
#define D3DPT_FB_PCI_DEVICE   0x3d00u

#define D3DPT_FB_VRAM_MB      128u                 /* default BAR 0 size: 64 MiB heap + the 64 MiB window */
#define D3DPT_FB_REGS_SIZE    0x1000u

/* register page (byte offsets, 32-bit accesses) */
#define D3DPT_FB_REG_MAGIC       0x00u   /* R: D3DPT_FB_MAGIC */
#define D3DPT_FB_REG_VERSION     0x04u   /* R: D3DPT_FB_VERSION */
#define D3DPT_FB_REG_VRAM_SIZE   0x08u   /* R: bytes in BAR 0 */
#define D3DPT_FB_REG_CAPS        0x0cu   /* R: D3DPT_FB_CAP_* */
#define D3DPT_FB_REG_MODE_COUNT  0x10u   /* R: entries in the host mode table */
#define D3DPT_FB_REG_MODE_SEL    0x14u   /* RW: table index the MODE_* registers describe */
#define D3DPT_FB_REG_MODE_W      0x18u   /* R: width of the selected entry (0 = out of range) */
#define D3DPT_FB_REG_MODE_H      0x1cu   /* R: height */
#define D3DPT_FB_REG_MODE_BPP    0x20u   /* R: 8, 16 or 32 */
#define D3DPT_FB_REG_MODE_HZ     0x24u   /* R: refresh the mode is advertised with */

#define D3DPT_FB_REG_ENABLE      0x40u   /* RW: 1 = linear mode below is shown, 0 = VGA core */
#define D3DPT_FB_REG_WIDTH       0x44u   /* RW: visible pixels per line */
#define D3DPT_FB_REG_HEIGHT      0x48u   /* RW: visible lines */
#define D3DPT_FB_REG_BPP         0x4cu   /* RW: 8 (PALETTE indices), 16 (r5g6b5) or 32 (x8r8g8b8) */
#define D3DPT_FB_REG_PITCH       0x50u   /* RW: bytes per line (0 = width * bpp / 8) */
#define D3DPT_FB_REG_OFFSET      0x54u   /* RW: byte offset of the first line in VRAM */
#define D3DPT_FB_REG_HZ          0x58u   /* RW: refresh the guest picked (informational) */

#define D3DPT_FB_REG_FRAMES      0x60u   /* R: vertical blanks since ENABLE, periods of the
                                            mode's HZ off the host clock, not the display
                                            client's pull, so the guest's frame pacing is the
                                            same headless and under the player */
#define D3DPT_FB_REG_DEBUG       0x70u   /* W: one character; lines go to the QEMU log */
#define D3DPT_FB_REG_DDFLAGS     0x74u   /* R: host test knob for the display driver's DirectDraw
                                            behaviour (-device d3dpt-vga,ddflags=N); 0 = normal */

#define D3DPT_FB_REG_CMD_OFFSET  0x80u   /* R: byte offset of the command window in BAR 0 (0 = none) */
#define D3DPT_FB_REG_DOORBELL    0x84u   /* W: 1 = execute the batch in the window; R: last D3DPT_ERR_* */
#define D3DPT_FB_REG_D3D_STATUS  0x88u   /* R: D3DPT_STATUS_* (0 = no executor on the host: no library,
                                            no Vulkan 1.3 device, or no-exec=on; 1 = ready) */

/* the hardware cursor (version 4) */
#define D3DPT_FB_REG_CURSOR_ADDR 0x90u   /* RW: byte offset in VRAM of the a8r8g8b8 image, W * H * 4 bytes, rows packed */
#define D3DPT_FB_REG_CURSOR_W    0x94u   /* RW: 1..D3DPT_FB_CURSOR_MAX */
#define D3DPT_FB_REG_CURSOR_H    0x98u   /* RW: 1..D3DPT_FB_CURSOR_MAX */
#define D3DPT_FB_REG_CURSOR_HOT_X 0x9cu  /* RW: hot spot inside the image */
#define D3DPT_FB_REG_CURSOR_HOT_Y 0xa0u
#define D3DPT_FB_REG_CURSOR_DEFINE 0xa4u /* W: 1 = take the image described above (0 = no cursor shape) */
#define D3DPT_FB_REG_CURSOR_X    0xa8u   /* RW: hot spot position on the screen (signed) */
#define D3DPT_FB_REG_CURSOR_Y    0xacu
#define D3DPT_FB_REG_CURSOR_ENABLE 0xb0u /* RW: 1 = shown at X / Y, 0 = hidden */
#define D3DPT_FB_REG_GAMMA_ENABLE 0xb4u  /* RW (version 5): 1 = the GAMMA block below is applied to every pixel
                                          * shown, as a RAMDAC would; the tables take effect at this write, so
                                          * write the 256 entries first. An identity ramp costs nothing */
#define D3DPT_FB_REG_IRQ_ENABLE  0xb8u   /* RW (version 6, CAP_IRQ): D3DPT_FB_IRQ_* the device may raise; 0 at reset */
#define D3DPT_FB_REG_IRQ_STATUS  0xbcu   /* R: D3DPT_FB_IRQ_* that happened while enabled; W: 1 bits acknowledge
                                          * (clear) them, and the line drops when none enabled is left */
#define D3DPT_FB_IRQ_VBLANK      0x1u    /* each period of HZ (60 when unset) while enabled, off the guest's clock */
#define D3DPT_FB_CURSOR_MAX      64u     /* pixels per side; larger pointers stay with GDI's software one */
#define D3DPT_FB_CURSOR_BYTES    (D3DPT_FB_CURSOR_MAX * D3DPT_FB_CURSOR_MAX * 4u)

#define D3DPT_FB_REG_PALETTE     0x400u  /* RW: 256 x8r8g8b8 entries (version 3), 0x400..0x7fc */
#define D3DPT_FB_PALETTE_SIZE    256u
#define D3DPT_FB_REG_GAMMA       0x800u  /* RW: 256 x8r8g8b8 entries (version 5), 0x800..0xbfc: entry i is what
                                          * a channel value i becomes on screen, per channel: the high bytes of
                                          * GDI's 3 x 256-word ramp (DrvIcmSetDeviceGammaRamp) */
#define D3DPT_FB_GAMMA_SIZE      256u

#define D3DPT_FB_CAP_BPP16       0x1u
#define D3DPT_FB_CAP_BPP32       0x2u
#define D3DPT_FB_CAP_D3D         0x4u    /* a command window exists (CMD_OFFSET != 0) */
#define D3DPT_FB_CAP_BPP8        0x8u    /* version 3: BPP = 8 and the PALETTE block */
#define D3DPT_FB_CAP_CURSOR      0x10u   /* version 4: the CURSOR registers */
#define D3DPT_FB_CAP_GAMMA       0x20u   /* version 5: GAMMA_ENABLE and the GAMMA block */
#define D3DPT_FB_CAP_IRQ         0x40u   /* version 6: an interrupt pin and the IRQ registers (irq=on) */

#endif
