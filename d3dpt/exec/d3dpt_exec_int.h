/*
 * d3dpt_exec_int.h: what d3dpt_exec.cpp (the d3d9 and the batch loop,
 * doc 14) and d3dpt_exec_ddi.cpp (the display driver's records, doc 15) share:
 * the executor state, the batch parser state and the record accessors.
 * Internal to libd3dpt_exec.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_EXEC_INT_H
#define D3DPT_EXEC_INT_H

#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "d3dpt_exec.h"
#include "../d3dpt_proto.h"
/* the scanout ring (M26): the player's images on DXVK's Vulkan devices,
 * shared Direct3D 12 resources on Windows, Metal heaps on macOS, opaque
 * fds on Linux */
#if defined(_WIN32) || defined(__APPLE__) || defined(__linux__)
#define D3DPT_RING 1
#ifdef _WIN32
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#elif defined(__APPLE__)
#ifndef VK_USE_PLATFORM_METAL_EXT
#define VK_USE_PLATFORM_METAL_EXT
#endif
#endif
#include <vulkan/vulkan.h>
#endif

/* printf checking where the compiler has it (not MSVC); mingw's plain
 * "printf" is msvcrt's dialect (no %zu), but the PE pair is built with
 * __USE_MINGW_ANSI_STDIO, whose format mingw names __MINGW_PRINTF_FORMAT */
#if defined(__GNUC__) && defined(__MINGW_PRINTF_FORMAT)
#define D3DPT_PRINTF(f, a) __attribute__((format(__MINGW_PRINTF_FORMAT, f, a)))
#elif defined(__GNUC__)
#define D3DPT_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define D3DPT_PRINTF(f, a)
#endif

namespace d3dpt {

/* A variable of the process environment. On Windows not getenv(): the
 * executor is MSVC with its own static C runtime (ADR-026), whose copy of
 * the environment is taken when the DLL loads and never sees what QEMU or
 * the player set after (doc 11, "The C runtime boundary"). The value
 * stays valid for the life of the process; a later change is read again. */
inline const char *env(const char *name)
{
#ifdef _WIN32
    static std::unordered_map<std::string, std::string> seen;
    char buf[1024];
    DWORD n = GetEnvironmentVariableA(name, buf, sizeof buf);
    if (!n || n >= sizeof buf) return nullptr;
    std::string &v = seen[name];
    if (v != buf) v = buf;
    return v.c_str();
#else
    return getenv(name);
#endif
}

struct Exec;
void exec_ddi_release(Exec &x);
void exec_d11_release(Exec &x);

enum Kind : uint8_t { K_NONE, K_QUERY };

/* p is null for a query a device reset released (it holds the Reset
 * back otherwise): made again at its next use, issued again if it was */
struct Obj { Kind kind; IUnknown *p; uint32_t type = 0; uint32_t issued = 0; };

struct Exec {
    d3dpt_exec_ops ops;
    void *dxvk = nullptr;
    IDirect3D9 *d3d = nullptr;
    /* which implementation is behind d3d: DXVK (false) everywhere it runs,
     * or Windows' own Direct3D 9 (true), the fallback for a host below
     * DXVK's Vulkan 1.3 floor. Everything the system implementation
     * refuses and DXVK takes hangs off this flag. d3dpt_exec.cpp's
     * header has the list. */
    bool native = false;
    HWND hwnd = nullptr;            /* native: the window D3D9 will not make a device without */
    D3DPRESENT_PARAMETERS last_pp{}; /* what the live device was created with (a lost device's Reset) */
    bool lost = false;              /* native: the device was lost and has not come back */
    IDirect3DDevice9 *dev = nullptr;
    std::unordered_map<uint32_t, Obj> objs;     /* the queries */
    int attach = 0;
    /* M7c: guest VRAM (d3dpt_exec_set_vram) and the display driver's objects */
    uint8_t *vram = nullptr;
    uint32_t vram_size = 0;
    struct Ddi *ddi = nullptr;
    void *d11 = nullptr;            /* M25: the Direct3D 11 side (d3dpt_exec_d11.cpp), made at its first record */
    bool settles = false;           /* v28: the device calls d3dpt_exec_settle before its display reads VRAM */
    /* the batch's records, copied out of the window before they are read:
     * the window is guest memory, and off TCG the device runs a batch
     * without the BQL, so another vCPU could rewrite a record between
     * its check and its use */
    std::vector<uint8_t> cmds;
    uint32_t ring_held = 0;         /* M26: scanout ring slots the front end may still sample (d3dpt_exec_ring_held) */
    /* v31: SCOPE, the handles a guest process's records may name, open
     * from the kernel driver's SCOPE until its close (across batches: a
     * submission's RETURN runs it in pieces). Only where a record or a
     * token names a surface or a resource; what a context bound before is
     * looked up again unchecked. */
    bool scoped = false;
    std::unordered_set<uint32_t> scope;
    bool named(uint32_t h) const { return !scoped || !h || scope.count(h) != 0; }

    /* A scene, opened by the executor rather than by whatever it is
     * executing. The display driver's DP2 stream carries no scene at all
     * (the DirectDraw/Direct3D 7 DDI has no such call); DXVK draws outside a scene
     * happily, Windows' own Direct3D 9 answers D3DERR_INVALIDCALL and
     * draws nothing. That was "the host drew 60-170 frames/s and every
     * readback was zero". So the executor keeps
     * the scene itself: opened before a draw, closed before every
     * transfer that D3D9 will not do inside one (StretchRect,
     * GetRenderTargetData) and at the end of a batch. Both
     * backends, because a scene is what a D3D9 frame is. */
    bool scene = false;
    void scene_begin() { if (dev && !scene && SUCCEEDED(dev->BeginScene())) scene = true; }
    void scene_end() { if (dev && scene) { dev->EndScene(); scene = false; } }

    void log(const char *fmt, ...) {
        char buf[512];
        va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        if (ops.log) ops.log(ops.ud, buf); else fprintf(stderr, "d3dpt: %s\n", buf);
    }
    template<class T> T *get(uint32_t h, Kind k) {
        auto it = objs.find(h);
        if (it == objs.end() || it->second.kind != k) return nullptr;
        return static_cast<T *>(it->second.p);
    }
    /* the queries a guest may hold at once: a program keeps a few per
     * frame in flight (DWM's events, a game's occlusion queries), and a
     * record of a few bytes makes each */
    static constexpr size_t MAX_OBJS = 65536;
    bool put(uint32_t h, Kind k, IUnknown *p) {
        if (!h || objs.count(h) || objs.size() >= MAX_OBJS) { if (p) p->Release(); return false; }
        objs[h] = { k, p };
        return true;
    }
    void release_all() {
        scene_end();
        exec_d11_release(*this);
        exec_ddi_release(*this);
        for (auto &kv : objs) if (kv.second.p) kv.second.p->Release();
        objs.clear();
        if (dev) {
            dev->Release(); dev = nullptr;
            if (ops.active) ops.active(ops.ud, 0);
        }
    }
};

/* the parse state of one batch */
struct Batch {
    Exec &x;
    uint8_t *shm;
    d3dpt_shm_hdr *hdr;
    uint8_t *ret;       /* return area */
    uint32_t err = D3DPT_ERR_OK;
    uint32_t index = 0;

    /* a return slot: the guest's offset must fit the whole payload */
    d3dpt_ret *slot(uint32_t off, uint32_t payload) {
        if (off % 8 || (uint64_t)off + sizeof(d3dpt_ret) + payload > D3DPT_RET_SIZE) { err = D3DPT_ERR_BAD_ARG; return nullptr; }
        d3dpt_ret *r = (d3dpt_ret *)(ret + off);
        r->hr = (uint32_t)E_FAIL; r->bytes = 0;
        return r;
    }
};

template<class T> static const T *body(const d3dpt_cmd *c, uint32_t extra, Batch &b) {
    if (c->size < sizeof(d3dpt_cmd) + sizeof(T) + extra) { b.err = D3DPT_ERR_MALFORMED; return nullptr; }
    return (const T *)(c + 1);
}
template<class T> static const uint8_t *tail(const T *t) { return (const uint8_t *)(t + 1); }

/* the M7c records (d3dpt_exec_ddi.cpp): true if the op was one of theirs */
bool exec_ddi_op(Batch &b, const d3dpt_cmd *c);
/* a lost device came back (native only): the display driver's objects in
 * the default pool are gone with it, so drop them and mark their VRAM
 * dirty. Every one of them is a copy of guest VRAM and is made again on
 * its next use. The surfaces stay registered: the guest driver sends a
 * VRAM_SURFACE once and never again. */
void exec_ddi_device_reset(Exec &x);

/* M25, the Direct3D 11 side (d3dpt_exec_d11.cpp): a D11 record; an
 * allocation the kernel driver released; a guest context it destroyed
 * (true if it was a Direct3D 11 one) */
bool exec_d11_op(Batch &b, const d3dpt_cmd *c);
bool exec_d11_gdi(Batch &b, const d3dpt_cmd *c);   /* v27's GDI record */
bool exec_d11_available(Exec &x);
void exec_d11_sync(Exec &x, uint32_t handle);
/* the flips' readbacks still owed to VRAM (RES_SCANOUT): those the GPU is
 * done with (wait false: at each batch), or all (the display) */
size_t exec_d11_settle(Exec &x, bool wait);   /* the readbacks still owed */
/* the scanout ring (M26): d3dpt_exec_ring, d3dpt_exec_ring_slot and
 * d3dpt_exec_scanout_vram */
int exec_d11_ring(Exec &x, void *const *handles, int count, uint32_t w, uint32_t h);
int exec_d11_ring_slot(Exec &x, uint32_t page, bool wait);
void exec_d11_scanout_vram(Exec &x, uint32_t page);
/* the same for the Direct3D 9 device's flips (D3DPT_OP_SCANOUT, v30) */
int exec_ddi_ring(Exec &x, void *const *handles, int count, uint32_t w, uint32_t h);
/* the ring's next slot for a frame, from `next` on: the first one the
 * front end does not hold, -1 when it holds them all (the frame is read
 * back instead). One in `latest` (the only place another page's newest
 * frame is, its VRAM older) only when no other is free: the caller then
 * puts that frame back into VRAM before the slot is overwritten, or a flip
 * still to show that page (its copy not done, the device's retry) finds no
 * slot and shows VRAM's older frame (M26) */
int exec_ring_pick(const Exec &x, uint32_t next, uint32_t count, uint32_t latest);
int exec_ddi_ring_slot(Exec &x, uint32_t page, bool wait);
void exec_ddi_scanout_vram(Exec &x, uint32_t page);
#ifdef D3DPT_RING
bool exec_vk_procs(VkInstance inst, PFN_vkGetDeviceProcAddr *gdpa, PFN_vkGetPhysicalDeviceMemoryProperties *memprops);
bool exec_vk_ring_image(Exec &x, const char *who, VkDevice dev, VkPhysicalDevice phys, PFN_vkGetDeviceProcAddr gdpa,
                        PFN_vkGetPhysicalDeviceMemoryProperties memprops, void *handle, uint32_t w, uint32_t h,
                        VkImage *img, VkDeviceMemory *mem);
#endif
void exec_d11_vram_release(Exec &x, uint32_t handle);
bool exec_d11_ctx_destroy(Exec &x, uint32_t ctx);
/* ... and what it asks of the display driver's side (d3dpt_exec_ddi.cpp):
 * an allocation's VRAM range by its registration; the host wrote it (the
 * Direct3D 9 object re-reads it); bring VRAM up to date with a Direct3D 9
 * object that drew into it, true if the allocation is a Direct3D 9
 * surface whose pixels VRAM now holds */
bool exec_ddi_vram(Exec &x, uint32_t handle, uint32_t *offset, uint32_t *bytes);
bool exec_ddi_vram_offset(Exec &x, uint32_t handle, uint32_t *offset);   /* no sync (M26) */
void exec_ddi_vram_written(Exec &x, uint32_t handle);
bool exec_ddi_vram_sync(Exec &x, uint32_t handle);

/* CreateDevice for both backends (d3dpt_exec.cpp) */
HRESULT exec_create_device(Exec &x, UINT adapter, DWORD flags, D3DPRESENT_PARAMETERS &pp, IDirect3DDevice9 **dev);

} // namespace d3dpt

#endif
