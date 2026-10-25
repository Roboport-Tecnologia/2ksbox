/*
 * d3dpt_proto.h: the paravirtual Direct3D protocol (doc 14, ADR-006).
 *
 * ONE header for every side: the display drivers (guest-tools/src/d3dptvid,
 * 32-bit, C), the d3dpt-vga adapter (hw/d3dpt, C), the host
 * decoder/executor (d3dpt/exec, C++ over DXVK) and the host test. Every
 * struct here is laid out with fixed-width fields and explicit padding so
 * a 32-bit guest and a 64-bit host agree byte for byte; all values
 * little-endian.
 *
 * Transport: the top D3DPT_SHM_SIZE bytes of the d3dpt-vga adapter's VRAM
 * BAR are the window (doc 15). The driver appends command records to it
 * and writes the adapter's DOORBELL register once per batch (a DP2 call
 * that needs its result, a readback, a query); the host executes the
 * whole batch synchronously inside the MMIO write and returns results
 * through the return area at the offsets the guest chose per record. No
 * interrupts. Surfaces live in guest VRAM: the host reads texels from it
 * and writes rendered frames back into it.
 *
 * v21 (M16 step 7): the guest DLLs' records (device, state, draws,
 * resources, Present) and the SysBus device's addresses and registers
 * are gone; the surviving ops keep their numbers.
 *
 * v22 (M18): the DP2 stream's BLT (op 81) also copies between two colour
 * render targets, with its rectangles and filter (see v18 below).
 *
 * v23 (M20): ... and between a plain texture and a colour render target,
 * either way.
 *
 * v24 (M20): the DP2 stream's COLORFILL (op 82) on a colour render target;
 * DRAW8's 32-bit indices and vertex ranges past 65536 (Direct3D 11's
 * feature level 9_2).
 *
 * v25 (M25, ADR-029): Direct3D 10 and 11: the D11 record (op 109), a
 * context's run of Direct3D 11 tokens from the WDDM driver's Direct3D 11
 * user-mode driver, and D3DPT_VS_D3D11 for the allocations behind them.
 *
 * v26 (M25 step 4): the Direct3D 11.1 interface's: a signature entry's
 * component type, constant buffers bound with offsets (SET_CB1) and
 * ClearView (CLEAR_VIEW).
 *
 * v27 (M25): GDI (op 110), the WDDM kernel driver's GDI acceleration: a
 * RenderKm copy or fill, or a present into a window's redirection bitmap,
 * done by the host between its Direct3D 11 objects or VRAM.
 *
 * v28 (M25): GDI SYNC, a Direct3D 11 allocation's pixels into its VRAM
 * before the kernel driver copies from there; a blt-model present no
 * longer reads its buffer back first. D11 RES_SCANOUT, a flip's readback
 * the host may finish later (before the display or anything else reads
 * that VRAM: d3dpt_exec_settle).
 *
 * v29 (M25): GDI's other RenderKm operations (d3dpt_gdi2): a BitBlt or a
 * ColorFill with any ROP3, AlphaBlend, StretchBlt, TransparentBlt and
 * ClearTypeBlend, done by the host on the CPU.
 *
 * v30 (M26): SCANOUT, a flip about to show a Direct3D 9 render target:
 * the host may keep the frame on its GPU (the scanout ring) and owe VRAM
 * until a READBACK asks for it; the display drivers no longer read the
 * target back at EndScene, only at a Lock, a blit and (without a ring) a
 * flip.
 *
 * v31 (review): SCOPE, the handles a guest process's records may name.
 * The WDDM kernel driver opens one around each user-mode submission (its
 * allocation list's host handles) and closes it after; while one is
 * open, a surface or Direct3D 11 resource a record or token names from
 * outside it fails as an unknown handle would, so one process cannot
 * name another's by writing its handle raw. Handles a context bound
 * earlier are not named again and stay usable.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_PROTO_H
#define D3DPT_PROTO_H

#include <stdint.h>

#define D3DPT_PROTO_VERSION   31u

#define D3DPT_SHM_SIZE        0x04000000u          /* 64 MiB */
#define D3DPT_CMD_OFFSET      0x00001000u          /* records start after the header page */
#define D3DPT_RET_OFFSET      0x03000000u          /* return area: last 16 MiB */
#define D3DPT_CMD_SIZE        (D3DPT_RET_OFFSET - D3DPT_CMD_OFFSET)
#define D3DPT_RET_SIZE        (D3DPT_SHM_SIZE - D3DPT_RET_OFFSET)

/* the adapter's D3D_STATUS register (d3dpt_fb.h) */
#define D3DPT_STATUS_NO_EXEC  0u      /* adapter present, no executor library on the host */
#define D3DPT_STATUS_READY    1u

/* header page of the window */
typedef struct d3dpt_shm_hdr {
    uint32_t cmd_bytes;     /* guest: bytes of records after D3DPT_CMD_OFFSET */
    uint32_t cmd_count;     /* guest: number of records (checked by the host) */
    uint32_t ret_status;    /* host: D3DPT_ERR_* of the last batch */
    uint32_t ret_index;     /* host: record index the error occurred at */
    uint32_t frames;        /* host: unused since v21 (the DLLs' presents) */
    uint32_t batches;       /* host: doorbells so far */
    uint32_t pad[2];
} d3dpt_shm_hdr;

#define D3DPT_ERR_OK          0u
#define D3DPT_ERR_MALFORMED   1u      /* record size/count inconsistent with cmd_bytes */
#define D3DPT_ERR_BAD_OP      2u
#define D3DPT_ERR_BAD_HANDLE  3u
#define D3DPT_ERR_BAD_ARG     4u      /* out-of-range size, offset, count */
#define D3DPT_ERR_NO_DEVICE   5u
#define D3DPT_ERR_HOST        6u      /* executor missing / DXVK refused */

/* every record: header, fixed body (per op), variable data; size is the
 * whole record in bytes, a multiple of 8 */
typedef struct d3dpt_cmd {
    uint32_t op;
    uint32_t size;
} d3dpt_cmd;
#define D3DPT_ALIGN8(x)       (((x) + 7u) & ~7u)

/* sync records carry ret_off: an offset into the return area where the
 * host writes the record's result (at least a d3dpt_ret) */
typedef struct d3dpt_ret {
    uint32_t hr;            /* HRESULT */
    uint32_t bytes;         /* bytes of payload following */
} d3dpt_ret;

/* Handles are guest-chosen 32-bit ids, unique per attached process; 0 is
 * never valid. The host mirrors handle -> DXVK object. */

enum d3dpt_op {
    D3DPT_OP_NOP = 0,
    D3DPT_OP_RELEASE = 4,           /* body: d3dpt_handle (a query) */
    /* --- queries (the DX9 DDI's occlusion and event queries) --- */
    D3DPT_OP_CREATE_QUERY = 81,         /* body: d3dpt_create_query; ret: d3dpt_ret */
    D3DPT_OP_QUERY_ISSUE = 82,          /* body: d3dpt_u32x2 (handle, flags) */
    D3DPT_OP_QUERY_GET_DATA = 83,       /* body: d3dpt_query_get (sync); ret: d3dpt_ret + data */
    /* --- the display driver's Direct3D DDI (doc 15, M7c): surfaces in guest VRAM --- */
    D3DPT_OP_VRAM_SURFACE = 96,         /* body: d3dpt_vram_surface: a DirectDraw surface in VRAM the host mirrors (forward) */
    D3DPT_OP_VRAM_RELEASE = 97,         /* body: d3dpt_handle: the surface is gone (forward) */
    D3DPT_OP_VRAM_DIRTY = 98,           /* body: d3dpt_handle: the guest wrote the surface's VRAM (re-read before use) */
    D3DPT_OP_CTX_CREATE = 99,           /* body: d3dpt_ctx_create (sync): a Direct3D context on a render target + Z surface */
    D3DPT_OP_CTX_DESTROY = 100,         /* body: d3dpt_handle */
    D3DPT_OP_CTX_SET_RT = 101,          /* body: d3dpt_u32x3 (ctx, rt surface, z surface or 0) */
    D3DPT_OP_CTX_CLEAR = 102,           /* body: d3dpt_ctx_clear + rects (D3DRECT) */
    D3DPT_OP_DP2 = 103,                 /* body: d3dpt_dp2 (sync) + command bytes (8-aligned) + vertex bytes: a DrawPrimitives2 call */
    D3DPT_OP_READBACK = 104,            /* body: d3dpt_sync (handle = surface; sync): copy the host render target into its VRAM */
    D3DPT_OP_VRAM_COLORKEY = 105,       /* body: d3dpt_u32x4 (surface handle, key low, key high, flags: 1 = the surface has a
                                         * source colour key): texels in [low, high] (the surface's own pixel value) render
                                         * transparent while render state 41 (COLORKEYENABLE) is on (v8; forward) */
    D3DPT_OP_VRAM_DIRTY_RANGE = 106,    /* body: d3dpt_u32x3 (handle, byte offset, bytes): the guest wrote that range of a
                                         * VRAM buffer (D3DPT_VS_BUFFER; v9; forward). Informational for now: a DRAW8 reads
                                         * its buffers straight from VRAM, so nothing is cached that the range would refresh */
    D3DPT_OP_VRAM_CUBE_FACE = 107,      /* body: d3dpt_u32x4 (face handle, cube handle, face 1..5, 0): the runtime's handle of
                                         * a cube face's level 0, after the cube's own VRAM_SURFACE (D3DPT_VS_CUBE; face 0 is
                                         * the cube's handle itself). A VRAM_DIRTY of it means the cube; on a render-target
                                         * cube it is a target a SETRENDERTARGET / READBACK can name (v11; forward) */
    D3DPT_OP_VRAM_MIP_LEVEL = 108,      /* body: d3dpt_u32x4 (level handle, texture handle, level 1.., 0): the runtime's handle
                                         * of a render-target texture's mip level, after the texture's VRAM_SURFACE; a
                                         * SETRENDERTARGET / READBACK / VRAM_DIRTY of it is that level of the texture (v19) */
    D3DPT_OP_D11 = 109,                 /* body: d3dpt_d11 + tokens: a Direct3D 11 context's run (v25; see below) */
    D3DPT_OP_GDI = 110,                 /* body: d3dpt_gdi (v27) or d3dpt_gdi2 (v29) + n d3dpt_gdi_rect: a GDI operation (forward) */
    D3DPT_OP_SCANOUT = 111,             /* body: d3dpt_sync: a flip will show this target (v30, M26); S_FALSE: VRAM has its frame */
    D3DPT_OP_SCOPE = 112,               /* body: d3dpt_u32x2 (n, flags) + n handles, 8-aligned: flags 1 opens a scope of the
                                         * n handles, 0 closes it (v31; forward) */
    D3DPT_OP_MAX
};

typedef struct d3dpt_handle { uint32_t handle; uint32_t pad; } d3dpt_handle;
typedef struct d3dpt_sync   { uint32_t handle; uint32_t ret_off; } d3dpt_sync;
typedef struct d3dpt_u32x2  { uint32_t a, b; } d3dpt_u32x2;
typedef struct d3dpt_u32x3  { uint32_t a, b, c, pad; } d3dpt_u32x3;
typedef struct d3dpt_u32x4  { uint32_t a, b, c, d; } d3dpt_u32x4;

typedef struct d3dpt_create_query { uint32_t handle, ret_off, type, pad; } d3dpt_create_query;
typedef struct d3dpt_query_get { uint32_t handle, ret_off, flags, size; } d3dpt_query_get;

/* --- M7c: the display driver's records --- */

/* a DirectDraw surface dxg placed in the VRAM heap: handle = the runtime's
 * dwSurfaceHandle (unique per process), offset relative to the start of
 * VRAM, format = D3DFORMAT (the driver translates the DDPIXELFORMAT),
 * caps = D3DPT_VS_*; levels = mip levels for textures (each level follows
 * the previous one in VRAM at its own pitch, DirectDraw layout). A P8
 * (palettized) texture's palette arrives in the DP2 stream as the runtime's
 * SETPALETTE / UPDATEPALETTE tokens; the host expands the indices (v8) */
/* GDI (v27): the WDDM kernel driver's GDI acceleration (RenderKm) and its
 * presents into a window's redirection bitmap. Each side is an allocation:
 * its handle (0: none), its VRAM place and its size and D3DDDIFORMAT (32
 * bits a pixel only). A side the host has a Direct3D 11 object of is that
 * object (a redirection bitmap's, D3DPT_GDI_TEXTURE, made at its first
 * use); any other is its VRAM, the host's pixels put there first. COPY
 * copies each rectangle from its source corner (SRCCOPY); FILL fills it
 * with `color` (PATCOPY, A8R8G8B8). REFRESH (dst only, no rectangles):
 * the CPU may have drawn into a redirection bitmap's VRAM (GDI draws most
 * of a window there itself); the host uploads what changed since it last
 * looked (its shadow of that VRAM). The host's own fills and copies from
 * VRAM go to the bitmap's VRAM and shadow too; a copy between objects (a
 * present) stays in the object until the CPU draws over it. SYNC (v28;
 * dst's handle only, no rectangles): what the host drew into an
 * allocation goes to its VRAM, which the kernel driver reads next (a
 * present's copy that is not the host's, a blt-model buffer the user-mode
 * driver no longer reads back). */
enum { D3DPT_GDI_COPY = 1, D3DPT_GDI_FILL = 2, D3DPT_GDI_REFRESH = 3, D3DPT_GDI_SYNC = 4 };
#define D3DPT_GDI_TEXTURE 0x1u
#define D3DPT_GDI_VRAM    0x2u      /* this side is its VRAM even where the host has an object of it (a bitmap paged
                                     * in or out: the object written back first, re-read after) */
typedef struct d3dpt_gdi_side {
    uint32_t handle, flags;
    uint32_t offset, pitch;
    uint32_t width, height, format, pad;
} d3dpt_gdi_side;
typedef struct d3dpt_gdi {
    uint32_t op, color, n, pad;         /* D3DPT_GDI_*; rectangles after this */
    d3dpt_gdi_side src, dst;            /* FILL: src unused */
} d3dpt_gdi;
typedef struct d3dpt_gdi_rect { int32_t sx, sy, l, t, r, b; } d3dpt_gdi_rect;   /* a destination rectangle, its source corner */
/* The other RenderKm operations (v29), as the DDI defines them
 * (DXGK_GDIARG_*), done by the host on the CPU over the pixels of each
 * side (an object's read back and written again, VRAM's in place):
 * ROP: a BitBlt (src set) or a ColorFill (src.handle and src.width 0,
 * P = `color`) with the ROP3 `rop` (bit P*4 + S*2 + D); STRETCH: StretchBlt,
 * `rop` its mode (1 BLACKONWHITE, 2 WHITEONBLACK: a shrink ANDs or ORs the
 * pixels; else they are skipped), `flags` 1 MirrorX, 2 MirrorY; ALPHA:
 * AlphaBlend, `flags` the constant alpha | 0x100 if the source has alpha
 * (premultiplied); TRANSPARENT: TransparentBlt, `color` the key, `flags` 1
 * HonorAlpha (the key's alpha compared too); CLEARTYPE: ClearTypeBlend,
 * `color` / `color2` the foreground with and without gamma, `gamma` the
 * row of the lookup table (`lut`, 8 bits, 512 a row: gamma then inverse;
 * ~0u none), `alpha` the coverage (A8R8G8B8) at the destination's place
 * plus (ax, ay). The source rectangle maps onto the destination's by the
 * DDI's stretch (COLORONCOLOR); the rectangles after this are the
 * destination's sub-rectangles (d3dpt_gdi_rect, sx / sy unused). A
 * result into a redirection bitmap is opaque, as a fill's. Starts as
 * d3dpt_gdi does. */
enum { D3DPT_GDI_ROP = 5, D3DPT_GDI_STRETCH = 6, D3DPT_GDI_ALPHA = 7, D3DPT_GDI_TRANSPARENT = 8, D3DPT_GDI_CLEARTYPE = 9 };
typedef struct d3dpt_gdi2 {
    uint32_t op, color, n, rop;
    uint32_t flags, color2, gamma, pad;
    int32_t sl, st, sr, sb;             /* the source rectangle */
    int32_t dl, dt, dr, db;             /* the destination rectangle */
    int32_t ax, ay, pad2[2];            /* CLEARTYPE: destination to alpha surface */
    d3dpt_gdi_side src, dst, alpha, lut;
} d3dpt_gdi2;

typedef struct d3dpt_vram_surface {
    uint32_t handle, offset;
    uint32_t width, height, pitch, format;
    uint32_t caps, levels;
} d3dpt_vram_surface;
#define D3DPT_VS_TEXTURE        0x1u
#define D3DPT_VS_RENDER_TARGET  0x2u
#define D3DPT_VS_ZBUFFER        0x4u
#define D3DPT_VS_PRIMARY        0x8u     /* part of the primary flip chain */
#define D3DPT_VS_BUFFER         0x10u    /* v9: a vertex / index buffer in VRAM (D3DDEVCAPS_HWVERTEXBUFFER /
                                          * HWINDEXBUFFER): width = pitch = its bytes, height 1, format 0, no levels;
                                          * a DRAW8 names it by handle and the host reads the range from VRAM */
#define D3DPT_VS_CUBE           0x20u    /* v11: a cube texture (with D3DPT_VS_TEXTURE, and D3DPT_VS_RENDER_TARGET for a
                                          * render-target cube): width = height = the edge, levels per face, the
                                          * record's offset / pitch face 0's level 0, and the tail 6 * levels - 1
                                          * {offset, pitch} pairs, face-major (face 0's levels 1.., then face 1's
                                          * levels 0.., …), faces in D3DCUBEMAP_FACES order (+X -X +Y -Y +Z -Z) */
#define D3DPT_CUBE_FACES        6u
#define D3DPT_VS_VOLUME         0x40u    /* v12: a volume texture (with D3DPT_VS_TEXTURE, never a render target): width,
                                          * height, pitch and offset are level 0's first slice, the tail the levels - 1
                                          * {offset, pitch} pairs of a 2D texture and then one {depth, slice pitch} pair:
                                          * level 0's slices follow each other at that slice pitch, level l has
                                          * max(1, depth >> l) slices one after the other at its own pitch * rows */
#define D3DPT_VOLUME_MAX_DEPTH  256u
#define D3DPT_VS_SAMPLES_SHIFT  8u       /* v13: a multisampled render target / depth buffer (never a texture): its
                                          * sample count, 2..16, in these bits (0 = not multisampled). The host renders
                                          * it with that many samples and resolves it into VRAM at every readback */
#define D3DPT_VS_SAMPLES_MASK   0x1f00u
#define D3DPT_VS_AUTOGEN        0x80u    /* v15: a texture whose mip levels the host makes (a DX9 D3DUSAGE_AUTOGENMIPMAP
                                          * one; with D3DPT_VS_TEXTURE, and D3DPT_VS_RENDER_TARGET for a target):
                                          * levels 1, the guest keeps level 0 alone. The DP2 stream's
                                          * GENERATEMIPSUBLEVELS (hSurface, D3DTEXTUREFILTERTYPE) makes them again */
#define D3DPT_VS_D3D11          0x2000u  /* v25: a Direct3D 10 / 11 resource's allocation (the D11 record's RES_CREATE
                                          * describes it): without D3DPT_VS_RENDER_TARGET / PRIMARY the record is a
                                          * VRAM range like a buffer's (width = pitch = bytes, height 1, format 0) and
                                          * the host makes no Direct3D 9 object for it; with them (a swap chain's
                                          * buffer, a shared surface) it is also a Direct3D 9 surface as described */

typedef struct d3dpt_ctx_create {
    uint32_t handle, ret_off;       /* the context handle the guest chose; ret: d3dpt_ret */
    uint32_t rt, z;                 /* VRAM surface handles (z may be 0) */
    uint32_t flags;                 /* v20: D3DPT_CTX_* */
    uint32_t pad;
} d3dpt_ctx_create;
/* v20: the caps this context's runtime was shown claim
 * D3DPTEXTURECAPS_NONPOW2CONDITIONAL, so a texture of other sizes samples
 * clamped whatever address mode the app sets, as on the era's cards */
#define D3DPT_CTX_NP2_CONDITIONAL 0x1u

typedef struct d3dpt_ctx_clear {
    uint32_t ctx, flags;            /* D3DCLEAR_* */
    uint32_t color, count;          /* count D3DRECTs follow (0 = whole target) */
    float    z;
    uint32_t stencil;
} d3dpt_ctx_clear;

/* one D3dDrawPrimitives2 call: the DP2 token stream (D3DHAL_DP2COMMAND
 * records, command_bytes) followed at an 8-byte boundary by the vertex
 * buffer (vertex_bytes of fvf vertices, vertex_stride each). The host
 * interprets the tokens on the context's render target; ret: d3dpt_ret
 * whose bytes field is the DP2 error offset when hr fails */
typedef struct d3dpt_dp2 {
    uint32_t ctx, ret_off;
    uint32_t flags, fvf;            /* D3DHALDP2_* flags, dwVertexType */
    uint32_t vertex_stride, command_bytes, vertex_bytes, pad;   /* vertex_bytes may be 0 (DX8: the draws carry their own) */
} d3dpt_dp2;

/* M7c, the DX8 DDI: the display driver rewrites the runtime's DX8 draw
 * tokens (SETSTREAMSOURCE / SETINDICES / DRAWPRIMITIVE… name vertex and
 * index buffers in guest memory the host cannot see) into this
 * self-contained token inside the DP2 command stream: the 4-byte
 * D3DHAL_DP2COMMAND header with bCommand = D3DPT_DP2_DRAW8, this struct,
 * the vertices (nverts * stride bytes, padded to 4), then the 16-bit
 * indices (nindices * 2 bytes, padded to 4; none for a plain draw). The
 * indices are the runtime's, relative to min_index (its MinIndex): vertex
 * 0 of the copied range is index min_index.
 *
 * fvf is the current SETVERTEXSHADER value: an FVF code (bit 0 clear; the
 * host computes its size, which stride must cover), or since v7 a DX8
 * vertex shader handle (bit 0 set): the runtime's CREATEVERTEXSHADER /
 * DELETEVERTEXSHADER / SETVERTEXSHADER(CONST) and CREATEPIXELSHADER /
 * DELETEPIXELSHADER / SETPIXELSHADER(CONST) tokens travel in the DP2
 * stream unchanged, the host keeps the shaders per context (a
 * declaration-only shader is the fixed function on that declaration) and
 * reads the copied vertices through the shader's declaration (stream 0 at
 * the stride above, the others as below).
 *
 * v9: a buffer the driver placed in VRAM (D3DPT_VS_BUFFER) is not copied.
 * With D3DPT_DRAW8_VRAM_VB in flags the vertex bytes are replaced by one
 * d3dpt_u32x2 {buffer handle, byte offset of vertex 0}; with
 * D3DPT_DRAW8_VRAM_IB the index bytes by one {handle, byte offset of index
 * 0}. The host reads nverts * stride (nindices * 2) bytes from the buffer's
 * VRAM at that offset, checked against the buffer's size.
 *
 * v24: with D3DPT_DRAW8_INDEX32 in flags the indices are 32-bit (4 bytes
 * each, inline or in VRAM), and nverts / nindices may go to
 * D3DPT_DRAW8_MAX_VERTS / D3DPT_DRAW8_MAX_INDICES: the WDDM driver reports
 * the MaxVertexIndex and MaxPrimitiveCount Direct3D 11's feature level 9_2
 * requires (Windows 11's WinUI 3 apps refuse 9_1, track M20).
 *
 * v10: more than one vertex stream. Everything above is stream 0. With
 * D3DPT_DRAW8_STREAMS in flags, after the indices come a d3dpt_u32x2
 * {count, 0} and count streams, each a d3dpt_dp2_draw8_stream followed by
 * its vertices: nverts * stride bytes (padded to 4), or with
 * D3DPT_DRAW8_VRAM_VB in its own flags one d3dpt_u32x2 {buffer handle,
 * byte offset of vertex 0} as for stream 0. Stream numbers are 1..15, in
 * increasing order. Every stream covers the same vertex range (vertex i
 * of the draw is element i of each), because a DX8 draw indexes all its
 * streams with one vertex number. Only a draw under a vertex shader handle
 * carries more than stream 0 (an FVF reads stream 0 alone), and the driver
 * sends every stream bound at the time it can resolve: the host takes the
 * ones the shader's declaration reads and skips the draw when one of those
 * is missing.
 *
 * v16: DX9 instancing. The {count, 0} word pair's second word is stream
 * 0's SetStreamSourceFreq value (D3DSTREAMSOURCE_INDEXEDDATA | n: the
 * indexed geometry drawn n times; 0 = not instanced, and count may then
 * be 0), and a stream's freq its own (D3DSTREAMSOURCE_INSTANCEDATA | d:
 * element k / d for instance k; 0 = read per vertex). An instance stream
 * carries ceil(n / d) elements from its first one instead of the draw's
 * vertex range. Only an indexed draw under a declaration is instanced.
 *
 * v17: several render targets. The DP2 stream carries the runtime's own
 * SETRENDERTARGET2 (op 85, {index, surface handle}) for targets 1..3;
 * target 0 stays DX7's SETRENDERTARGET pair.
 *
 * v18: StretchRect between two depth buffers. The runtime's BLT (op 81,
 * D3DHAL_DP2BLT: source, RECTL, level, destination, RECTL, level, flags)
 * travels in the DP2 stream when both are video-memory depth buffers,
 * whose contents only the host has. v22: also between two colour render
 * targets, its rectangles and filter (flags 1 point, 2 linear) as given,
 * for a surface the guest's CPU cannot map; every other BLT is the
 * driver's. v23: also between a plain texture (level 0, no cube or
 * volume) and a colour render target, either way: from the texture's VRAM
 * (no palette / colour-key expansion), or the target read back into the
 * texture's VRAM (one texel size). Windows 11's DWM fills its shared
 * targets from textures, and an acrylic backdrop reads one back.
 *
 * v24: the runtime's COLORFILL (op 82, D3DHAL_DP2COLORFILL: surface,
 * RECTL, A8R8G8B8 colour) travels in the DP2 stream for a colour render
 * target's level 0, filled on the host (IDirect3DDevice9::ColorFill, the
 * colour converted to the surface's format as d3d9 does): a shared target
 * the guest's CPU cannot map (Windows 11's apps fill the small atlases
 * DWM samples, a window's caption colour among them), and any other
 * target without a readback and an upload. Every other COLORFILL is the
 * driver's. */
#define D3DPT_DP2_DRAW8 200u
#define D3DPT_DRAW8_VRAM_VB 0x1u
#define D3DPT_DRAW8_VRAM_IB 0x2u
#define D3DPT_DRAW8_STREAMS 0x4u
#define D3DPT_DRAW8_INDEX32 0x8u
#define D3DPT_DRAW8_MAX_STREAMS 16u
/* v24: the most vertices one DRAW8 may span (nverts), and indices (before:
 * 0x10000 and 0x100000) */
#define D3DPT_DRAW8_MAX_VERTS 0x1000000u
#define D3DPT_DRAW8_MAX_INDICES 0x400000u
typedef struct d3dpt_dp2_draw8 {
    uint32_t prim_type, prim_count;     /* D3DPRIMITIVETYPE, primitives */
    uint32_t fvf, stride;               /* the vertices' format (an FVF or a vertex shader handle), stream 0's stride */
    uint32_t nverts, nindices;          /* vertices; indices (0 = not indexed) */
    uint32_t min_index, flags;          /* D3DPT_DRAW8_* (v9; 0 before: everything inline) */
} d3dpt_dp2_draw8;
typedef struct d3dpt_dp2_draw8_stream { /* v10: one more stream of a DRAW8 */
    uint32_t stream, stride;            /* 1..15; its stride */
    uint32_t flags;                     /* D3DPT_DRAW8_VRAM_VB: its vertices in a VRAM buffer */
    uint32_t freq;                      /* v16: its SetStreamSourceFreq value, 0 = per vertex (was padding) */
} d3dpt_dp2_draw8_stream;

/* --- v25 (M25, ADR-029): Direct3D 10 and 11 ---
 *
 * The WDDM driver's Direct3D 11 user-mode driver (guest-tools/src/
 * d3dptvid/wddm/um11) writes D11 records: one guest device's run of the
 * tokens below, executed on the host's one d3d11 device (DXVK) with that
 * device's own pipeline state (an ID3DDeviceContextState per ctx). ctx is
 * the host context the kernel driver knows (D3DPT_CTX_PRIV, ../guest-
 * tools/src/d3dptvid/wddm/d3dpt_wddm.h): CTX_DESTROY of it releases every
 * object below of that context. ret_off: a d3dpt_ret, and after it the
 * payload of the record's QUERY_GET token (the last one) if it has one;
 * hr is the first failure of the record's tokens, S_OK otherwise.
 *
 * Every token is a d3dpt_d11_tok and its body; size is the whole token,
 * a multiple of 4. The values are the Direct3D 11 API's (DXGI_FORMAT,
 * D3D11_BIND_*, D3D11_USAGE, D3D11_*_DIMENSION, D3D11_BLEND, ...): the
 * user-mode driver translates the DDI's where they differ.
 *
 * Names: a resource is its allocation's host handle (the kernel driver
 * patches it in, D3DPT_PATCH_HANDLE), so every process that opens a
 * shared one names the same object; views, states, shaders, input
 * layouts and queries are the context's, by non-zero ids the guest picks.
 * An id of 0 in a binding unbinds.
 *
 * A resource lives on the host. Its allocation (D3DPT_VS_D3D11) holds its
 * subresources at the offsets RES_CREATE lists when the guest's CPU may
 * see them (a dynamic or staging resource, a swap chain's buffer, a
 * shared one); RES_DIRTY uploads one after the guest wrote it, RES_READBACK
 * writes the host's back. A resource with no layout is the host's alone.
 */
typedef struct d3dpt_d11 {
    uint32_t ctx, ret_off;
    uint32_t bytes;                     /* token bytes after this */
    uint32_t flags;                     /* 0 */
} d3dpt_d11;

typedef struct d3dpt_d11_tok {
    uint16_t op;                        /* D3DPT_D11_* */
    uint16_t pad;
    uint32_t size;                      /* the whole token, this header included */
} d3dpt_d11_tok;

enum d3dpt_d11_op {
    /* resources */
    D3DPT_D11_RES_CREATE = 1,           /* d3dpt_d11_res + nsub d3dpt_d11_sub (initial data follows as RES_UPDATEs) */
    D3DPT_D11_RES_UPDATE = 2,           /* d3dpt_d11_update + bytes: UpdateSubresource */
    D3DPT_D11_RES_DIRTY = 3,            /* d3dpt_d11_ref: the guest wrote the subresource's VRAM (sub ~0: all) */
    D3DPT_D11_RES_READBACK = 4,         /* d3dpt_d11_ref: the host's subresource into its VRAM (sub ~0: all) */
    D3DPT_D11_COPY = 5,                 /* d3dpt_d11_ref {dst, src}: CopyResource */
    D3DPT_D11_COPY_REGION = 6,          /* d3dpt_d11_copy */
    D3DPT_D11_RESOLVE = 7,              /* d3dpt_d11_resolve */
    D3DPT_D11_GEN_MIPS = 8,             /* u32 srv */
    D3DPT_D11_BLT = 9,                  /* d3dpt_d11_blt: a DXGI present's copy, scaled and converted */
    /* objects */
    D3DPT_D11_SRV_CREATE = 16,          /* d3dpt_d11_view */
    D3DPT_D11_RTV_CREATE = 17,          /* d3dpt_d11_view */
    D3DPT_D11_DSV_CREATE = 18,          /* d3dpt_d11_view */
    D3DPT_D11_DESTROY = 19,             /* d3dpt_d11_ref {kind (D3DPT_D11K_*), id} */
    D3DPT_D11_BLEND_CREATE = 20,        /* d3dpt_d11_blend */
    D3DPT_D11_DEPTH_CREATE = 21,        /* d3dpt_d11_depth */
    D3DPT_D11_RASTER_CREATE = 22,       /* d3dpt_d11_raster */
    D3DPT_D11_SAMPLER_CREATE = 23,      /* d3dpt_d11_sampler */
    D3DPT_D11_SHADER_CREATE = 24,       /* d3dpt_d11_shader, the code, the signatures, the stream output */
    D3DPT_D11_LAYOUT_CREATE = 25,       /* d3dpt_d11_list {id, n} + n d3dpt_d11_element */
    D3DPT_D11_QUERY_CREATE = 26,        /* d3dpt_d11_query */
    /* bindings */
    D3DPT_D11_SET_SHADER = 32,          /* d3dpt_d11_ref {stage, id} */
    D3DPT_D11_SET_CB = 33,              /* d3dpt_d11_bind + n resources */
    D3DPT_D11_SET_SRV = 34,             /* d3dpt_d11_bind + n ids */
    D3DPT_D11_SET_SAMPLERS = 35,        /* d3dpt_d11_bind + n ids */
    D3DPT_D11_SET_LAYOUT = 36,          /* u32 id */
    D3DPT_D11_SET_VB = 37,              /* d3dpt_d11_bind {0, start, n} + n d3dpt_d11_vb */
    D3DPT_D11_SET_IB = 38,              /* d3dpt_d11_vb {res, format, offset} */
    D3DPT_D11_SET_TOPOLOGY = 39,        /* u32 D3D11_PRIMITIVE_TOPOLOGY */
    D3DPT_D11_SET_TARGETS = 40,         /* d3dpt_d11_list {dsv id, n} + n rtv ids */
    D3DPT_D11_SET_BLEND = 41,           /* d3dpt_d11_set_blend */
    D3DPT_D11_SET_DEPTH = 42,           /* d3dpt_d11_ref {id, stencil ref} */
    D3DPT_D11_SET_RASTER = 43,          /* u32 id */
    D3DPT_D11_SET_VIEWPORTS = 44,       /* u32 n, then n {x, y, w, h, min z, max z} floats */
    D3DPT_D11_SET_SCISSORS = 45,        /* u32 n, then n {left, top, right, bottom} */
    D3DPT_D11_SET_SO = 46,              /* d3dpt_d11_bind {0, 0, n} + n d3dpt_d11_ref {res, offset} */
    D3DPT_D11_SET_PREDICATION = 47,     /* d3dpt_d11_ref {query id, value} */
    /* work */
    D3DPT_D11_DRAW = 48,                /* u32 [2] {vertices, first} */
    D3DPT_D11_DRAW_INDEXED = 49,        /* u32 [3] {indices, first, base vertex (signed)} */
    D3DPT_D11_DRAW_INSTANCED = 50,      /* u32 [4] {vertices, instances, first, first instance} */
    D3DPT_D11_DRAW_INDEXED_INSTANCED = 51, /* u32 [5] {indices, instances, first, base vertex, first instance} */
    D3DPT_D11_DRAW_AUTO = 52,           /* no body */
    D3DPT_D11_CLEAR_RTV = 53,           /* u32 id, float [4] */
    D3DPT_D11_CLEAR_DSV = 54,           /* u32 id, flags (D3D11_CLEAR_*), float depth, u32 stencil */
    D3DPT_D11_QUERY_BEGIN = 55,         /* u32 id */
    D3DPT_D11_QUERY_END = 56,           /* u32 id */
    D3DPT_D11_QUERY_GET = 57,           /* d3dpt_d11_ref {id, size}: GetData (no wait) into the record's return payload */
    D3DPT_D11_SET_CB1 = 58,             /* v26: d3dpt_d11_bind + n d3dpt_d11_cb1: *SetConstantBuffers1 */
    D3DPT_D11_CLEAR_VIEW = 59,          /* v26: d3dpt_d11_clear_view + n {left, top, right, bottom}: ClearView */
    D3DPT_D11_RES_SCANOUT = 60,         /* v28: d3dpt_d11_ref as RES_READBACK's, for a buffer a flip shows: the
                                         * host may finish it once its GPU is done, and does before anything
                                         * reads that VRAM (the display: d3dpt_exec_settle) */
};

/* D3DPT_D11_DESTROY's kinds */
enum { D3DPT_D11K_SRV = 1, D3DPT_D11K_RTV, D3DPT_D11K_DSV, D3DPT_D11K_BLEND, D3DPT_D11K_DEPTH, D3DPT_D11K_RASTER,
       D3DPT_D11K_SAMPLER, D3DPT_D11K_SHADER, D3DPT_D11K_LAYOUT, D3DPT_D11K_QUERY };
/* shader stages */
enum { D3DPT_D11S_VS = 0, D3DPT_D11S_PS, D3DPT_D11S_GS, D3DPT_D11S_HS, D3DPT_D11S_DS, D3DPT_D11S_CS, D3DPT_D11S_COUNT };

typedef struct d3dpt_d11_ref { uint32_t a, b; } d3dpt_d11_ref;
typedef struct d3dpt_d11_list { uint32_t id, n; } d3dpt_d11_list;
typedef struct d3dpt_d11_bind { uint32_t stage, start, n; } d3dpt_d11_bind;
typedef struct d3dpt_d11_cb1 { uint32_t res, first, num; } d3dpt_d11_cb1;       /* in 16-byte constants; num 0: the whole buffer */
typedef struct d3dpt_d11_clear_view {
    uint32_t kind, id;                  /* D3DPT_D11K_RTV (the one kind yet) */
    float color[4];
    uint32_t n;                         /* rectangles after this (0: the whole view) */
} d3dpt_d11_clear_view;

typedef struct d3dpt_d11_res {
    uint32_t res;                       /* the allocation's host handle */
    uint32_t dim;                       /* D3D11_RESOURCE_DIMENSION */
    uint32_t format;                    /* DXGI_FORMAT */
    uint32_t usage, bind, cpu, misc;    /* D3D11_USAGE, D3D11_BIND_*, D3D11_CPU_ACCESS_*, D3D11_RESOURCE_MISC_* */
    uint32_t width, height, depth;      /* a buffer's width is its bytes */
    uint32_t mips, array;               /* a cube's array counts faces */
    uint32_t samples, quality;
    uint32_t stride;                    /* a structured buffer's */
    uint32_t nsub;                      /* subresources in VRAM (0: the host's alone); D3D11 order, mip-major per slice */
    uint32_t opened;                    /* 1: another process's resource this context opened: ignored if the host has
                                         * it, else (a Direct3D 9 surface) made from its VRAM */
} d3dpt_d11_res;
typedef struct d3dpt_d11_sub {
    uint32_t offset;                    /* inside the allocation */
    uint32_t row_pitch, depth_pitch;
} d3dpt_d11_sub;

typedef struct d3dpt_d11_update {
    uint32_t res, sub;
    uint32_t has_box;
    uint32_t box[6];                    /* D3D11_BOX: left, top, front, right, bottom, back */
    uint32_t row_pitch, depth_pitch;    /* of the bytes that follow */
    uint32_t bytes;
} d3dpt_d11_update;

typedef struct d3dpt_d11_copy {
    uint32_t dst, dst_sub, x, y, z;
    uint32_t src, src_sub;
    uint32_t has_box;
    uint32_t box[6];
} d3dpt_d11_copy;

typedef struct d3dpt_d11_resolve { uint32_t dst, dst_sub, src, src_sub, format; } d3dpt_d11_resolve;

typedef struct d3dpt_d11_blt {
    uint32_t dst, dst_sub;
    uint32_t left, top, right, bottom;  /* in the destination */
    uint32_t src, src_sub;
} d3dpt_d11_blt;

typedef struct d3dpt_d11_view {
    uint32_t id, res;
    uint32_t format;                    /* DXGI_FORMAT */
    uint32_t dim;                       /* D3D11_SRV / RTV / DSV_DIMENSION */
    uint32_t u[4];                      /* the dimension's union of the API's desc, in its order */
    uint32_t flags;                     /* a DSV's D3D11_DSV_READ_ONLY_*; a raw buffer SRV's D3D11_BUFFEREX_SRV_FLAG_RAW */
} d3dpt_d11_view;

typedef struct d3dpt_d11_blend {        /* D3D11_BLEND_DESC */
    uint32_t id;
    uint32_t alpha_to_coverage, independent;
    struct {
        uint32_t enable, src, dst, op, src_a, dst_a, op_a, mask;
    } rt[8];
} d3dpt_d11_blend;

typedef struct d3dpt_d11_depth {        /* D3D11_DEPTH_STENCIL_DESC */
    uint32_t id;
    uint32_t depth_enable, write_mask, func;
    uint32_t stencil_enable, read_mask, write_mask8;
    uint32_t front[4], back[4];         /* fail, depth fail, pass, func */
} d3dpt_d11_depth;

typedef struct d3dpt_d11_raster {       /* D3D11_RASTERIZER_DESC */
    uint32_t id;
    uint32_t fill, cull, front_ccw;
    int32_t bias;
    float bias_clamp, slope_bias;
    uint32_t depth_clip, scissor, multisample, aa_lines;
} d3dpt_d11_raster;

typedef struct d3dpt_d11_sampler {      /* D3D11_SAMPLER_DESC */
    uint32_t id;
    uint32_t filter, address[3];
    float lod_bias;
    uint32_t max_aniso, func;
    float border[4];
    float min_lod, max_lod;
} d3dpt_d11_sampler;

/* A shader: its tokens as the runtime gave them (the D3D10 tokenized
 * program, the DXBC's SHDR / SHEX chunk; dword 1 is its length), the
 * input and output signatures by register (the DDI's, nothing else: the
 * host writes the DXBC container around them), and a geometry shader's
 * stream output declaration. */
typedef struct d3dpt_d11_shader {
    uint32_t id, stage;
    uint32_t code_dwords;
    uint32_t nin, nout;                 /* d3dpt_d11_sig entries after the code */
    uint32_t nso;                       /* d3dpt_d11_so entries after them (a geometry shader with stream output) */
    uint32_t so_strides[4], so_rast;    /* the buffers' strides; the rasterized stream (D3D11_SO_NO_RASTERIZED_STREAM: none) */
} d3dpt_d11_shader;
typedef struct d3dpt_d11_sig {
    uint32_t sv;                        /* D3D10_SB_NAME (0: none) */
    uint32_t reg;
    uint32_t mask;                      /* xyzw in bits 0..3 */
    uint32_t stream;
    uint32_t type;                      /* v26: D3D_REGISTER_COMPONENT_TYPE (0: not given, float) */
} d3dpt_d11_sig;
typedef struct d3dpt_d11_so { uint32_t stream, slot, reg, mask; } d3dpt_d11_so;  /* reg ~0: a gap of popcount(mask) */

typedef struct d3dpt_d11_element {      /* an input element, by the register it feeds */
    uint32_t slot, offset, format;
    uint32_t per_instance, step;
    uint32_t reg;
} d3dpt_d11_element;

typedef struct d3dpt_d11_query { uint32_t id, type, misc; } d3dpt_d11_query;   /* D3D11_QUERY, D3D11_QUERY_MISC_* */

typedef struct d3dpt_d11_vb { uint32_t res, stride, offset; } d3dpt_d11_vb;   /* SET_IB: {res, DXGI_FORMAT, offset} */

typedef struct d3dpt_d11_set_blend { uint32_t id, mask; float factor[4]; } d3dpt_d11_set_blend;

#endif /* D3DPT_PROTO_H */
