/*
 * d3dpt_proto.h: the paravirtual Direct3D device protocol (doc 14, ADR-006).
 *
 * ONE header for every side: the guest d3d9.dll (32-bit mingw, C), the
 * QEMU device model (hw/d3dpt, C), the host decoder/executor
 * (d3dpt/exec, C++ over DXVK) and the host test. Every struct here is
 * laid out with fixed-width fields and explicit padding so a 32-bit
 * guest and a 64-bit host agree byte for byte; all values little-endian.
 *
 * Transport (the qemu-3dfx model): a SysBus device with a 4 KiB register
 * page and a guest-physical shared window at fixed addresses, mapped by
 * the guest through FXPTL.SYS / FXMEMMAP.VXD (the same helper the GL
 * wrapper installs). The guest appends command records to the window
 * and writes the doorbell once per batch (Present, or when the batch is
 * full, or before it needs a result); the host executes the whole batch
 * synchronously inside the MMIO write and returns results through the
 * return area at the offsets the guest chose per record. No interrupts.
 *
 * The same window layout is used a second time by the d3dpt-vga display
 * adapter (doc 15, M7c): the top D3DPT_SHM_SIZE bytes of its VRAM BAR are
 * a window in exactly this layout, its DOORBELL register submits it, and
 * the XP display driver appends the D3DPT_OP_VRAM_* / CTX_* / DP2 records
 * below (Direct3D DDI: surfaces live in guest VRAM, the host reads
 * texels from and writes rendered frames back into that VRAM).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_PROTO_H
#define D3DPT_PROTO_H

#include <stdint.h>

#define D3DPT_PROTO_VERSION   20u
#define D3DPT_MAGIC           0x54503344u          /* "D3PT" read at REG_MAGIC */

/* guest-physical map: below mesapt's 0xe0000000+ windows and SeaBIOS' BAR area */
#define D3DPT_MM_BASE         0xdfffe000u          /* register page, 4 KiB */
#define D3DPT_SHM_BASE        0xd8000000u          /* shared window */
#define D3DPT_SHM_SIZE        0x04000000u          /* 64 MiB */
#define D3DPT_CMD_OFFSET      0x00001000u          /* records start after the header page */
#define D3DPT_RET_OFFSET      0x03000000u          /* return area: last 16 MiB */
#define D3DPT_CMD_SIZE        (D3DPT_RET_OFFSET - D3DPT_CMD_OFFSET)
#define D3DPT_RET_SIZE        (D3DPT_SHM_SIZE - D3DPT_RET_OFFSET)

/* register page offsets (32-bit accesses only) */
#define D3DPT_REG_MAGIC       0x00u   /* R: D3DPT_MAGIC */
#define D3DPT_REG_VERSION     0x04u   /* R: D3DPT_PROTO_VERSION of the host */
#define D3DPT_REG_STATUS      0x08u   /* R: D3DPT_STATUS_* */
#define D3DPT_REG_DOORBELL    0x0cu   /* W: 1 = execute the batch in the window; R: last D3DPT_ERR_* */
#define D3DPT_REG_ATTACH      0x10u   /* W: 1 = a guest process attached, 0 = detached; R: attach count */

#define D3DPT_STATUS_NO_EXEC  0u      /* device present, no executor library on the host */
#define D3DPT_STATUS_READY    1u

/* header page of the window */
typedef struct d3dpt_shm_hdr {
    uint32_t cmd_bytes;     /* guest: bytes of records after D3DPT_CMD_OFFSET */
    uint32_t cmd_count;     /* guest: number of records (checked by the host) */
    uint32_t ret_status;    /* host: D3DPT_ERR_* of the last batch */
    uint32_t ret_index;     /* host: record index the error occurred at */
    uint32_t frames;        /* host: presents so far (diagnostics) */
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
    D3DPT_OP_LOG = 90,              /* body: d3dpt_u32x2 (bytes, 0) + text: the guest DLL's log line, printed by the host (diagnostics) */
    /* --- adapter / device (sync) --- */
    D3DPT_OP_GET_ADAPTER = 1,       /* body: d3dpt_get_adapter; ret: d3dpt_ret + d3dpt_adapter_info */
    D3DPT_OP_CREATE_DEVICE = 2,     /* body: d3dpt_create_device; ret: d3dpt_ret */
    D3DPT_OP_RESET_DEVICE = 3,      /* body: d3dpt_create_device (handle = device); ret: d3dpt_ret */
    D3DPT_OP_RELEASE = 4,           /* body: d3dpt_handle (any object; the device releases everything) */
    D3DPT_OP_PRESENT = 5,           /* body: d3dpt_sync; ret: d3dpt_ret. Host presents the frame */
    /* --- state (forward) --- */
    D3DPT_OP_CLEAR = 16,            /* body: d3dpt_clear + rects */
    D3DPT_OP_BEGIN_SCENE = 17,
    D3DPT_OP_END_SCENE = 18,
    D3DPT_OP_SET_RENDER_STATE = 19, /* body: d3dpt_u32x2 (state, value) */
    D3DPT_OP_SET_FVF = 20,          /* body: d3dpt_u32x2 (fvf, 0) */
    D3DPT_OP_SET_VIEWPORT = 21,     /* body: d3dpt_viewport */
    D3DPT_OP_SET_TRANSFORM = 22,    /* body: d3dpt_transform */
    D3DPT_OP_SET_TEXTURE_STAGE_STATE = 23, /* body: d3dpt_u32x3 (stage, type, value) */
    D3DPT_OP_SET_SAMPLER_STATE = 24,       /* body: d3dpt_u32x3 (sampler, type, value) */
    D3DPT_OP_SET_LIGHT = 25,        /* body: d3dpt_light */
    D3DPT_OP_LIGHT_ENABLE = 26,     /* body: d3dpt_u32x2 (index, enable) */
    D3DPT_OP_SET_MATERIAL = 27,     /* body: d3dpt_material */
    D3DPT_OP_SET_TEXTURE = 28,      /* body: d3dpt_u32x2 (stage, texture handle or 0) */
    D3DPT_OP_SET_STREAM_SOURCE = 29,/* body: d3dpt_u32x4 (stream, vb handle or 0, offset, stride) */
    D3DPT_OP_SET_INDICES = 30,      /* body: d3dpt_u32x2 (ib handle or 0, 0) */
    D3DPT_OP_SET_VERTEX_SHADER = 31,/* body: d3dpt_u32x2 (handle or 0, 0) */
    D3DPT_OP_SET_PIXEL_SHADER = 32, /* body: d3dpt_u32x2 (handle or 0, 0) */
    D3DPT_OP_SET_VS_CONST_F = 33,   /* body: d3dpt_u32x2 (start, count4) + count4*16 bytes */
    D3DPT_OP_SET_PS_CONST_F = 34,   /* body: d3dpt_u32x2 (start, count4) + count4*16 bytes */
    D3DPT_OP_SET_RENDER_TARGET = 35,/* body: d3dpt_u32x2 (index, surface handle or 0 = backbuffer) */
    D3DPT_OP_SET_DEPTH_STENCIL = 36,/* body: d3dpt_u32x2 (surface handle or 0 = auto depth, 0) */
    D3DPT_OP_SET_SCISSOR_RECT = 37, /* body: d3dpt_u32x4 (left, top, right, bottom) */
    D3DPT_OP_SET_CLIP_PLANE = 38,   /* body: d3dpt_clip_plane */
    D3DPT_OP_SET_VERTEX_DECL = 39,  /* body: d3dpt_u32x2 (decl handle or 0, 0) */
    D3DPT_OP_SET_VS_CONST_I = 40,   /* body: d3dpt_u32x2 (start, count4) + count4*16 bytes of int32 */
    D3DPT_OP_SET_PS_CONST_I = 41,
    D3DPT_OP_SET_VS_CONST_B = 42,   /* body: d3dpt_u32x2 (start, count) + count*4 bytes of BOOL */
    D3DPT_OP_SET_PS_CONST_B = 43,
    /* --- draws (forward) --- */
    D3DPT_OP_DRAW_PRIMITIVE = 48,   /* body: d3dpt_u32x3 (type, start, primcount) */
    D3DPT_OP_DRAW_INDEXED_PRIMITIVE = 49, /* body: d3dpt_draw_indexed */
    D3DPT_OP_DRAW_PRIMITIVE_UP = 50,/* body: d3dpt_draw_up + vertex bytes */
    D3DPT_OP_DRAW_INDEXED_PRIMITIVE_UP = 51, /* body: d3dpt_draw_indexed_up + index bytes + vertex bytes */
    /* --- resources (create: sync; update: forward) --- */
    D3DPT_OP_CREATE_VERTEX_BUFFER = 64, /* body: d3dpt_create_buffer; ret: d3dpt_ret */
    D3DPT_OP_CREATE_INDEX_BUFFER = 65,  /* body: d3dpt_create_buffer; ret: d3dpt_ret */
    D3DPT_OP_CREATE_TEXTURE = 66,       /* body: d3dpt_create_texture; ret: d3dpt_ret */
    D3DPT_OP_CREATE_DEPTH_STENCIL = 67, /* body: d3dpt_create_texture (w,h,format,ms); ret */
    D3DPT_OP_CREATE_RENDER_TARGET = 68, /* body: d3dpt_create_texture; ret */
    D3DPT_OP_GET_SURFACE = 69,          /* body: d3dpt_get_surface (texture level -> surface handle); ret */
    D3DPT_OP_BUFFER_UPDATE = 70,        /* body: d3dpt_update (handle, offset, bytes) + bytes */
    D3DPT_OP_TEXTURE_UPDATE = 71,       /* body: d3dpt_tex_update + rows*pitch bytes */
    D3DPT_OP_CREATE_VERTEX_SHADER = 72, /* body: d3dpt_create_shader + bytecode; ret */
    D3DPT_OP_CREATE_PIXEL_SHADER = 73,  /* body: d3dpt_create_shader + bytecode; ret */
    D3DPT_OP_GET_RENDER_TARGET_DATA = 74, /* body: d3dpt_sync (handle = surface); ret: d3dpt_ret + pixels (pitch = w*bpp) */
    D3DPT_OP_STRETCH_RECT = 75,         /* body: d3dpt_stretch_rect */
    D3DPT_OP_CREATE_VERTEX_DECL = 76,   /* body: d3dpt_create_shader (bytes of D3DVERTEXELEMENT9, END included); ret */
    D3DPT_OP_CREATE_CUBE_TEXTURE = 77,  /* body: d3dpt_create_texture (width = edge); ret. Faces: level | face << 8 in TEXTURE_UPDATE / GET_SURFACE */
    D3DPT_OP_CREATE_OFFSCREEN = 78,     /* body: d3dpt_create_texture (pool); ret: a host offscreen plain surface */
    D3DPT_OP_SURFACE_UPDATE = 79,       /* body: d3dpt_tex_update (handle = surface, level ignored) + rows*pitch bytes */
    D3DPT_OP_COLOR_FILL = 80,           /* body: d3dpt_color_fill */
    D3DPT_OP_CREATE_QUERY = 81,         /* body: d3dpt_create_query; ret */
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

typedef struct d3dpt_get_adapter {
    uint32_t adapter;
    uint32_t ret_off;
} d3dpt_get_adapter;

/* D3DADAPTER_IDENTIFIER9 differs between 32- and 64-bit only in its tail
 * padding (1100 vs 1104 bytes), so it travels as this explicit layout;
 * D3DCAPS9 (only DWORD/INT/float members) is 304 bytes on both and is
 * copied raw. Both sides static_assert the sizes. */
typedef struct d3dpt_adapter_identifier {
    char     description[512];
    char     driver[512];
    char     device_name[32];
    uint32_t driver_version_lo, driver_version_hi;
    uint32_t vendor_id, device_id, subsys_id, revision;
    uint8_t  guid[16];
    uint32_t whql_level;
    uint32_t pad;
} d3dpt_adapter_identifier;
#define D3DPT_SIZEOF_CAPS9                304u
typedef struct d3dpt_adapter_info {
    d3dpt_adapter_identifier identifier;
    uint8_t  caps[D3DPT_SIZEOF_CAPS9];
    uint32_t mode_count;    /* the host's mode list follows: mode_count * d3dpt_mode */
    uint32_t pad;
} d3dpt_adapter_info;
typedef struct d3dpt_mode { uint32_t width, height, refresh, format; } d3dpt_mode;

/* D3DPRESENT_PARAMETERS without the HWND */
typedef struct d3dpt_present_params {
    uint32_t width, height, format, backbuffer_count;
    uint32_t multisample, multisample_quality, swap_effect, windowed;
    uint32_t auto_depth, depth_format, flags, refresh;
    uint32_t interval, pad;
} d3dpt_present_params;

typedef struct d3dpt_create_device {
    uint32_t handle;        /* the device handle the guest chose */
    uint32_t ret_off;
    uint32_t adapter, devtype, behavior, pad;
    d3dpt_present_params pp;
} d3dpt_create_device;

typedef struct d3dpt_clear {
    uint32_t count;         /* D3DRECT (4 x int32) list follows */
    uint32_t flags;
    uint32_t color;
    float    z;
    uint32_t stencil;
    uint32_t pad;
} d3dpt_clear;

typedef struct d3dpt_viewport { uint32_t x, y, w, h; float minz, maxz; } d3dpt_viewport;
typedef struct d3dpt_transform { uint32_t state, pad; float m[16]; } d3dpt_transform;
typedef struct d3dpt_light { uint32_t index, pad; uint8_t light[104]; } d3dpt_light;       /* D3DLIGHT9 */
typedef struct d3dpt_material { uint8_t material[68]; uint32_t pad; } d3dpt_material;      /* D3DMATERIAL9 */

typedef struct d3dpt_draw_indexed {
    uint32_t type, base_vertex, min_index, num_vertices, start_index, prim_count;
} d3dpt_draw_indexed;
typedef struct d3dpt_draw_up {
    uint32_t type, prim_count, stride, bytes;   /* vertex data follows */
} d3dpt_draw_up;
/* the indices are the application's; the vertices are its min_index ..
 * min_index + num_vertices - 1 (vertex 0 of the data is index min_index), so
 * every index lies in that range */
typedef struct d3dpt_draw_indexed_up {
    uint32_t type, min_index, num_vertices, prim_count;
    uint32_t index_format, index_bytes, stride, vertex_bytes;  /* indices then (8-aligned) vertices follow */
} d3dpt_draw_indexed_up;

typedef struct d3dpt_create_buffer {
    uint32_t handle, ret_off;
    uint32_t length, usage, fvf_or_format, pool;
} d3dpt_create_buffer;
typedef struct d3dpt_create_texture {
    uint32_t handle, ret_off;
    uint32_t width, height, levels, usage, format, pool;
    uint32_t multisample, ms_quality, lockable, pad;
} d3dpt_create_texture;
typedef struct d3dpt_get_surface {
    uint32_t handle, ret_off;   /* the new surface handle */
    uint32_t texture, level;    /* texture 0 = the device backbuffer / auto depth (level 1) */
} d3dpt_get_surface;
typedef struct d3dpt_update {
    uint32_t handle, offset, bytes, flags; /* flags: D3DLOCK_DISCARD / NOOVERWRITE hints; data follows */
} d3dpt_update;
typedef struct d3dpt_tex_update {
    uint32_t handle, level;
    uint32_t x, y, w, h;        /* dirty box in pixels (blocks for DXT) */
    uint32_t pitch, bytes;      /* guest row pitch; rows*pitch bytes follow */
} d3dpt_tex_update;
typedef struct d3dpt_create_shader {
    uint32_t handle, ret_off, bytes, pad;   /* DWORD tokens follow */
} d3dpt_create_shader;
typedef struct d3dpt_clip_plane { uint32_t index, pad; float plane[4]; } d3dpt_clip_plane;
typedef struct d3dpt_color_fill {
    uint32_t handle, color, has_rect, pad;
    int32_t  rect[4];
} d3dpt_color_fill;
typedef struct d3dpt_create_query { uint32_t handle, ret_off, type, pad; } d3dpt_create_query;
typedef struct d3dpt_query_get { uint32_t handle, ret_off, flags, size; } d3dpt_query_get;
typedef struct d3dpt_stretch_rect {
    uint32_t src, dst, filter, has_rects;
    int32_t  src_rect[4], dst_rect[4];
} d3dpt_stretch_rect;

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
 * whose contents only the host has; every other BLT is the driver's. */
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
