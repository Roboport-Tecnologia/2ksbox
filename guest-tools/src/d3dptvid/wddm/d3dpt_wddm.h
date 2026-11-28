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
    ULONG fb_caps;                                /* D3DPT_FB_CAP_* (an older KMD leaves 0 here) */
    ULONG reserved[8];
} D3DPT_UMD_INFO;

/* CreateContext's private data: the host context (CTX_CREATE's handle)
 * the device's records will name. The kernel driver's DestroyContext
 * destroys it on the host, which also covers a process that ends without
 * the user-mode driver's DestroyDevice (a crash; Windows 11's DWM clients
 * did so hundreds of times, track M20 step 5). */
#define D3DPT_CTX_MAGIC 0x58544344u               /* "DCTX" */
typedef struct D3DPT_CTX_PRIV {
    ULONG magic;                                  /* D3DPT_CTX_MAGIC */
    ULONG host_ctx;
} D3DPT_CTX_PRIV;

/* Escape: the user-mode driver's private data */
#define D3DPT_ESC_MAGIC 0x43534544u               /* "DESC" */
enum { D3DPT_ESC_LOG = 1, D3DPT_ESC_WINDOW = 2 };
#define D3DPT_ESC_TEXT 120
typedef struct D3DPT_ESC {
    ULONG magic, op;
    char text[D3DPT_ESC_TEXT];                    /* D3DPT_ESC_LOG: one line, NUL terminated, no newline */
} D3DPT_ESC;

/* D3DPT_ESC_WINDOW (register set v12, track M20): the guest agent asks for
 * the mode of the front end's window, after the driver signalled
 * D3DPT_WINDOW_EVENT. The driver answers its custom mode (the window's
 * size, aligned and made to fit the segment), now in its mode list; 0 x 0
 * when there is none. */
typedef struct D3DPT_ESC_SIZE {
    ULONG magic, op;
    ULONG w, h;
} D3DPT_ESC_SIZE;
/* an auto-reset event the driver sets when the window's size changed; the
 * agent opens it as Global\2ksboxWindowSize */
#define D3DPT_WINDOW_EVENT L"\\BaseNamedObjects\\2ksboxWindowSize"

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
enum { D3DPT_ALLOC_PRIMARY = 1, D3DPT_ALLOC_SHADOW, D3DPT_ALLOC_STAGING, D3DPT_ALLOC_D3D,
       D3DPT_ALLOC_GDI };                         /* a GDI surface (GDI acceleration, x64): its D3DKMDT_GDISURFACETYPE in `source` */
#define D3DPT_GDISURF_TEXTURE 1u                  /* D3DKMDT_GDISURFACE_TEXTURE: a redirection bitmap, the host's object */
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
    ULONG shared;                                 /* a shared resource's: other processes open it, the CPU never maps it */
    struct { ULONG a, b; } lv[D3DPT_ALLOC_MAX_LV];
} D3DPT_ALLOC_DESC;

/* the user-mode driver's patch locations: what the kernel driver writes
 * at PatchOffset, one ULONG (DriverId 0 is the kernel driver's own) */
enum {
    D3DPT_PATCH_HANDLE = 1,                       /* the host's handle of the allocation */
    D3DPT_PATCH_OFFSET = 2,                       /* its VRAM offset + AllocationOffset */
};

/* A record the kernel-mode driver consumes, never sent to the host: the
 * next record's result (its ret_off's d3dpt_ret and `bytes` of payload)
 * is copied into VRAM at `dst` (a D3DPT_PATCH_OFFSET fix-up of the
 * user-mode driver's result allocation) once the host ran it. The kernel
 * driver gives that record a return slot of its own and rings the doorbell
 * right after it. Body: d3dpt_u32x2 {dst, bytes}. */
#define D3DPT_UMD_OP_RETURN 0x8000u

/* the allocation list's entries per submission (the context's
 * AllocationListSize, and what the user-mode driver tracks). 64 split a
 * Direct3D 11 game's frame into ~45 submissions, each a round of
 * dxgkrnl's scheduling and the device's exits (Unigine Heaven: ~1,850
 * draws a frame, one buffer each; track M25) */
#define D3DPT_WDDM_MAX_LIST 512

/* what the kernel driver puts in front of a submission's records (its
 * registration packet, an entry of 24 bytes on x64 per allocation, and
 * the packets' headers): the user-mode drivers keep it free */
#define D3DPT_WDDM_SLACK (4096 + D3DPT_WDDM_MAX_LIST * 24)

/* The command buffer: one header, then records. The kernel driver turns
 * each Render into one DMA buffer: its registration packet, then the
 * records unchanged but for the patches. */
#define D3DPT_UMD_CMD_MAGIC 0x444d4355u           /* "UCMD" */
typedef struct D3DPT_UMD_CMD {
    ULONG magic;
    ULONG count;                                  /* records after this header */
} D3DPT_UMD_CMD;

#endif
