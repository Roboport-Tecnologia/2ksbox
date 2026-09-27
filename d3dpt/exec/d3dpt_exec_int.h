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
#include <unordered_map>
#include <vector>

#include "d3dpt_exec.h"
#include "../d3dpt_proto.h"

namespace d3dpt {

struct Exec;
void exec_ddi_release(Exec &x);

enum Kind : uint8_t { K_NONE, K_QUERY };

struct Obj { Kind kind; IUnknown *p; };

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
    bool put(uint32_t h, Kind k, IUnknown *p) {
        if (!h || objs.count(h)) { if (p) p->Release(); return false; }
        objs[h] = { k, p };
        return true;
    }
    void release_all() {
        scene_end();
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

/* CreateDevice for both backends (d3dpt_exec.cpp) */
HRESULT exec_create_device(Exec &x, UINT adapter, DWORD flags, D3DPRESENT_PARAMETERS &pp, IDirect3DDevice9 **dev);

} // namespace d3dpt

#endif
