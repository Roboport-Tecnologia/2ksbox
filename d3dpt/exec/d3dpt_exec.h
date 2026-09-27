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

#define D3DPT_EXEC_API __attribute__((visibility("default")))

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
D3DPT_EXEC_API int d3dpt_exec_shared_alloc(const char *name, uint64_t size, uint64_t *offset);
D3DPT_EXEC_API void d3dpt_exec_shared_map(uint64_t offset, void *ptr);

#ifdef __cplusplus
}
#endif
#endif
