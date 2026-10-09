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
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_PROTO_H
#define D3DPT_PROTO_H

#include <stdint.h>

#define D3DPT_PROTO_VERSION   23u

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
 * targets from textures, and an acrylic backdrop reads one back. */
#define D3DPT_DP2_DRAW8 200u
#define D3DPT_DRAW8_VRAM_VB 0x1u
#define D3DPT_DRAW8_VRAM_IB 0x2u
#define D3DPT_DRAW8_STREAMS 0x4u
#define D3DPT_DRAW8_MAX_STREAMS 16u
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

#endif /* D3DPT_PROTO_H */
