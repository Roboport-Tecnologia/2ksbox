/*
 * d3dpt_wddm.h: what the WDDM user-mode driver (um/d3dptumd.c) and the
 * kernel-mode driver (km/d3dptkmd.c) say to each other (M18 step 2, plan
 * step 6). Nothing here reaches the host: the device still sees only
 * d3dpt_proto.h records in its command window.
 *
 * Three channels, all of them dxgkrnl's:
 *
 *   - QueryAdapterInfo(UMDRIVERPRIVATE): the adapter's facts the user-mode
 *     driver needs before it can answer GetCaps (D3DPT_UMD_INFO);
 *   - Escape: a log line into the device's DEBUG register (D3DPT_ESC), so
 *     the user-mode driver's trace lands in the QEMU log beside the kernel
 *     driver's;
 *   - the allocations' private data (D3DPT_ALLOC_DESC, its D3D kind) and
 *     the command buffer (D3DPT_UMD_CMD), which Render copies into a DMA
 *     buffer and SubmitCommand hands to the device.
 *
 * The command buffer is d3dpt_proto.h records, written by the user-mode
 * driver as the XP display driver writes them into the window, with one
 * difference: an allocation is named by its index in the submission's
 * allocation list, with a patch location whose DriverId says what the
 * kernel driver writes there at Patch time (D3DPT_PATCH_*): the surface
 * handle it gave the allocation, or its VRAM offset. The kernel driver
 * registers every listed allocation with the host (VRAM_SURFACE) before
 * the records run whenever its place in VRAM is new to the host.
 *
 * Both sides include this: plain C, ULONG and friends only (ntddk.h and
 * windows.h both have them).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_WDDM_H
#define D3DPT_WDDM_H

/* QueryAdapterInfo(DXGKQAITYPE_UMDRIVERPRIVATE) */
#define D3DPT_UMD_MAGIC 0x444d5544u               /* "DUMD" */
#define D3DPT_UMD_VERSION 1u
typedef struct D3DPT_UMD_INFO {
    ULONG magic, version;                         /* D3DPT_UMD_MAGIC, D3DPT_UMD_VERSION */
    ULONG fb_version;                             /* the device's register set (D3DPT_FB_REG_VERSION) */
    ULONG ddflags;                                /* -device d3dpt-vga,ddflags=N (core_caps.c reads them) */
    ULONG d3d;                                    /* 1: a command window and an executor on the host */
    ULONG vram, seg_size;                         /* BAR 0's bytes, and the segment's (VRAM below the window) */
    ULONG reserved[9];
} D3DPT_UMD_INFO;

/* Escape: the user-mode driver's private data */
#define D3DPT_ESC_MAGIC 0x43534544u               /* "DESC" */
enum { D3DPT_ESC_LOG = 1 };
#define D3DPT_ESC_TEXT 120
typedef struct D3DPT_ESC {
    ULONG magic, op;
    char text[D3DPT_ESC_TEXT];                    /* D3DPT_ESC_LOG: one line, NUL terminated, no newline */
} D3DPT_ESC;

/*
 * An allocation's private data, from GetStandardAllocationDriverData
 * (dxgkrnl's own surfaces) or from the user-mode driver's AllocateCb
 * (D3DPT_ALLOC_D3D). A D3D allocation is one resource, every level (and
 * face) inside it at the offsets the user-mode driver laid out, as the
 * XP driver's lightweight mip levels are; `lv` is d3dpt_vram_surface's
 * tail with offsets relative to the allocation, which the kernel driver
 * makes absolute when it registers the allocation with the host.
 */
#define D3DPT_ALLOC_MAGIC 0x3d41544cu             /* "LTA=" */
enum { D3DPT_ALLOC_PRIMARY = 1, D3DPT_ALLOC_SHADOW, D3DPT_ALLOC_STAGING, D3DPT_ALLOC_D3D };
#define D3DPT_ALLOC_MAX_LV (6 * 16)               /* a cube's six faces of 16 levels, less one, plus a volume's pair */

typedef struct D3DPT_ALLOC_DESC {
    ULONG magic;
    ULONG kind;
    ULONG w, h, pitch, bpp;
    ULONG format;                                 /* D3DDDIFORMAT (the same numbers as D3DFORMAT) */
    ULONG refresh_num, refresh_den;               /* a primary's */
    ULONG source;                                 /* a primary's VidPN source */
    /* D3DPT_ALLOC_D3D */
    ULONG size;                                   /* bytes, every level */
    ULONG caps;                                   /* D3DPT_VS_* (d3dpt_proto.h); 0: never shown to the host (system memory copies) */
    ULONG levels;                                 /* d3dpt_vram_surface.levels */
    ULONG nlv;                                    /* entries of lv in use */
    ULONG primary;                                /* also a primary the VidPN may scan out (a full-screen swap chain) */
    struct { ULONG a, b; } lv[D3DPT_ALLOC_MAX_LV];
} D3DPT_ALLOC_DESC;

/* the user-mode driver's patch locations: what the kernel driver writes
 * at PatchOffset, one ULONG (DriverId 0 is the kernel driver's own) */
enum {
    D3DPT_PATCH_HANDLE = 1,                       /* the host's handle of the allocation */
    D3DPT_PATCH_OFFSET = 2,                       /* its VRAM offset + AllocationOffset */
};

/* the allocation list's entries per submission (the context's
 * AllocationListSize, and what the user-mode driver tracks) */
#define D3DPT_WDDM_MAX_LIST 64

/* The command buffer: one header, then records. The kernel driver turns
 * each Render into one DMA buffer: its registration packet, then the
 * records unchanged but for the patches. */
#define D3DPT_UMD_CMD_MAGIC 0x444d4355u           /* "UCMD" */
typedef struct D3DPT_UMD_CMD {
    ULONG magic;
    ULONG count;                                  /* records after this header */
} D3DPT_UMD_CMD;

#endif
