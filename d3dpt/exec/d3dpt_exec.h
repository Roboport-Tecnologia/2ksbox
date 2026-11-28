/*
 * d3dpt_exec.h: C API of the host-side decoder/executor of the
 * display driver's Direct3D (libd3dpt_exec, C++ over DXVK's d3d9).
 * The d3dpt-vga adapter (hw/d3dpt) dlopens it, so QEMU stays C and the
 * protocol evolves without a QEMU rebuild.
 *
 * Threading: one caller thread (the vCPU that took the doorbell write,
 * BQL held). DXVK runs its own worker threads underneath.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_EXEC_H
#define D3DPT_EXEC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* exported: the library is built with hidden visibility, and on Windows
 * with MSVC (ADR-026), which exports only what is marked */
#ifdef _WIN32
#define D3DPT_EXEC_API __declspec(dllexport)
#else
#define D3DPT_EXEC_API __attribute__((visibility("default")))
#endif

typedef struct d3dpt_exec_ops {
    void *ud;
    void (*log)(void *ud, const char *msg);
    /* a device was created (on) / the last one released (off) */
    void (*active)(void *ud, int on);
    /* M7c: the executor wrote [offset, offset + bytes) of the guest VRAM
     * handed over by d3dpt_exec_set_vram (a READBACK); the device marks the
     * range dirty for its scanout. May be NULL. */
    void (*vram_dirty)(void *ud, uint32_t offset, uint32_t bytes);
} d3dpt_exec_ops;

typedef struct d3dpt_exec d3dpt_exec_t;

/* protocol version this library speaks (D3DPT_PROTO_VERSION) */
D3DPT_EXEC_API uint32_t d3dpt_exec_version(void);
/* load DXVK's d3d9 (env D3DPT_DXVK_LIB, else build/dxvk/src/d3d9/libdxvk_d3d9.so.0
 * relative to the cwd, else the bare soname) and create IDirect3D9; NULL if
 * no usable Vulkan device */
D3DPT_EXEC_API d3dpt_exec_t *d3dpt_exec_create(const d3dpt_exec_ops *ops);
D3DPT_EXEC_API void d3dpt_exec_destroy(d3dpt_exec_t *x);
/* guest process attached (1) / detached (0): detach releases every object */
D3DPT_EXEC_API void d3dpt_exec_attach(d3dpt_exec_t *x, int attach);
/* execute the batch described by the window's header; returns D3DPT_ERR_*
 * and writes ret_status/ret_index into the header */
D3DPT_EXEC_API uint32_t d3dpt_exec_submit(d3dpt_exec_t *x, void *shm, uint32_t shm_size);
/* M7c: the guest VRAM the display driver's D3DPT_OP_VRAM_* records refer
 * to (the d3dpt-vga adapter's BAR 0 below its command window). The host
 * reads texels from it and READBACK writes rendered frames into it; the
 * pointer must stay valid until the executor is destroyed. */
D3DPT_EXEC_API void d3dpt_exec_set_vram(d3dpt_exec_t *x, void *vram, uint32_t size);

/* Optional entry points (the loader takes their absence as "no"):
 *   probe: 1 if a device can be made on this host at all. The loader
 *     asks it before it settles on a library, so the in-process executor
 *     (DXVK) gives way to the out-of-process one (libd3dpt_exec_remote,
 *     Wine on the host, docs/tracks/m15-wine-executor.md) below the
 *     Vulkan 1.3 floor;
 *   shared_alloc / shared_map: only the remote library has them. They give an fd
 *     and offset of its shared file for a region of `size`, which the
 *     device maps as guest RAM (memory_region_init_ram_from_fd) and then
 *     registers the mapping of, so the executor's other process reads and
 *     writes the guest's own bytes. */
D3DPT_EXEC_API int d3dpt_exec_probe(void);
/* Optional too: what this executor runs besides the display driver's
 * Direct3D 9 records, D3DPT_EXEC_FEAT_*; asking opens what it names (M25:
 * the Direct3D 11 device). The adapter turns it into its CAP bits. */
#define D3DPT_EXEC_FEAT_D3D11 0x1u
/* The host's Direct3D 11 multisampling (2026-11-27, track M25): bits
 * 8-11 the counts 2x, 4x, 8x, 16x every guest multisample format of up to
 * 64 bits a pixel can be a target at, bits 12-15 the same for the 128-bit
 * ones. The adapter passes them to the guest (CAPS bits 16-23), whose
 * driver claims those counts and no others. */
#define D3DPT_EXEC_FEAT_MSAA_SHIFT 8
#define D3DPT_EXEC_FEAT_MSAA_MASK  0xff00u
D3DPT_EXEC_API uint32_t d3dpt_exec_features(d3dpt_exec_t *x);
/* Optional too (v28): finish what the executor still owes VRAM (a flip's
 * readback, D3DPT_D11_RES_SCANOUT) before the display reads it. Called
 * like the others, serialised with them; an executor that has never
 * been asked defers nothing. `wait` 0: only what the GPU is done with,
 * so the caller never blocks on the GPU. Returns how many readbacks are
 * still owed (0: VRAM is current). */
D3DPT_EXEC_API int d3dpt_exec_settle(d3dpt_exec_t *x, int wait);
/* Optional too, Windows, macOS and Linux (track M26): the scanout ring.
 * `ring` imports the front end's `count` images of w x h (NT handles to
 * shared Direct3D 12 resources on Windows, MTLHeaps on macOS,
 * d3dpt_exec_ring_fd pointers on Linux, BGRA8; the caller's still) and
 * returns how many it took, 0
 * for none (count 0 drops the ring); from then on a flip's frame
 * (D3DPT_D11_RES_SCANOUT) that fits goes into the next image on the GPU
 * instead of back into VRAM. `ring_slot`: the image holding the frame of
 * the page at that VRAM offset, once its copy is done; -1 when VRAM
 * holds the page's latest (no slot does), -2 when the copy is not done
 * and `wait` is 0. `scanout_vram`: that page's frame into VRAM now, for
 * a reader of VRAM (a screendump, a picture the CPU path redraws). */
D3DPT_EXEC_API int d3dpt_exec_ring(d3dpt_exec_t *x, void *const *handles, int count, uint32_t w, uint32_t h);
/* A ring image on Linux: a Vulkan image of w x h, B8G8R8A8_UNORM with
 * MUTABLE_FORMAT, optimal tiling, usage TRANSFER_SRC | TRANSFER_DST |
 * SAMPLED, in a dedicated allocation exported as an opaque fd, which the
 * executor imports on DXVK's device with the same parameters (dup'ing
 * `fd`) once its device's IDs match. It leaves a filled image in GENERAL. */
typedef struct d3dpt_exec_ring_fd {
    int32_t fd;
    uint32_t memory_type;           /* the allocation's memory type index */
    uint64_t size;                  /* and its size */
    uint8_t device_uuid[16];        /* VkPhysicalDeviceIDProperties of the exporter */
    uint8_t driver_uuid[16];
} d3dpt_exec_ring_fd;
D3DPT_EXEC_API int d3dpt_exec_ring_slot(d3dpt_exec_t *x, uint32_t vram_offset, int wait);
D3DPT_EXEC_API void d3dpt_exec_scanout_vram(d3dpt_exec_t *x, uint32_t vram_offset);
/* Optional too: the ring's images the front end may still be sampling
 * (bit n = image n), set before each batch; a frame goes into one it does
 * not hold, or is read back when it holds them all. */
D3DPT_EXEC_API void d3dpt_exec_ring_held(d3dpt_exec_t *x, uint32_t held);
D3DPT_EXEC_API int d3dpt_exec_shared_alloc(const char *name, uint64_t size, uint64_t *offset);
D3DPT_EXEC_API void d3dpt_exec_shared_map(uint64_t offset, void *ptr);

#ifdef __cplusplus
}
#endif
#endif
