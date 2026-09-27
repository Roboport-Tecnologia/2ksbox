/*
 * d3dpt_exec.cpp: the executor's frame around the display driver's
 * records (doc 14, ADR-006/007): opening a d3d9 (DXVK or the host's own),
 * CreateDevice, a lost device, and the batch loop that parses what the
 * guest left in the window (d3dpt_proto.h), validates every record and
 * hands all but the query records to d3dpt_exec_ddi.cpp.
 *
 * Build: scripts/build-d3dpt-exec.sh -> build/d3dpt/libd3dpt_exec.so
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <cstdlib>
#include <string>
#include <vector>
#include <algorithm>
#include <exception>

/*
 * The one dynamic load in here is the D3D9 implementation itself. DXVK is
 * the rasteriser (ADR-007) and the only one whose frames are held against
 * the rig goldens: its native library on Linux and macOS, its own d3d9.dll
 * shipped as dxvk_d3d9.dll on Windows.
 *
 * There is a second backend, on Windows only: the system's own
 * Direct3D 9, for a host below DXVK's Vulkan 1.3 floor (ADR-013:
 * pre-Broadwell Intel, Kepler and older, TeraScale). There the
 * card's own D3D9 driver is the best thing on the machine. `D3DPT_D3D9`
 * picks:
 *
 *   auto (default)  DXVK, falling back to the system d3d9 when DXVK opens
 *                   no adapter at all. The launcher resolves `auto` ahead
 *                   of this with its own Vulkan probe (host_gpu.rs) and
 *                   says `-global d3dpt-vga.d3d9=system` when the host is
 *                   below the bar or has only a software Vulkan device,
 *                   which this cannot tell from a good one.
 *   dxvk            DXVK or no pass-through.
 *   system          Windows' own d3d9, the A/B on a host that has both.
 *
 * What the system implementation refuses and DXVK takes is why the first
 * attempt at this drew black (dxdiag and 3DMark 99 on an
 * RTX 3090: host draws at 60-170 frames/s, every readback zero). All of it
 * hangs off Exec::native:
 *
 *   - a device with no window at all: it gets a hidden one of ours;
 *   - a draw outside a scene: the executor opens the scene itself
 *     (Exec::scene_begin, both backends);
 *   - hardware vertex processing, or a windowed backbuffer format that is
 *     not the desktop's, refused where DXVK's offscreen swapchain takes
 *     anything: both retried;
 *   - a device that can be lost, which DXVK's never is.
 */
#ifdef _WIN32
#include <windows.h>
#define D3DPT_DLOPEN(p)     ((void *)LoadLibraryA(p))
#define D3DPT_DLSYM(h, s)   ((void *)GetProcAddress((HMODULE)(h), (s)))
#define D3DPT_DLCLOSE(h)    FreeLibrary((HMODULE)(h))
#else
#include <dlfcn.h>
#define D3DPT_DLOPEN(p)     dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define D3DPT_DLSYM(h, s)   dlsym((h), (s))
#define D3DPT_DLCLOSE(h)    dlclose(h)
#endif

#include "d3dpt_exec_int.h"
namespace {

using namespace d3dpt;

static bool need_device(Batch &b) {
    if (!b.x.dev) { b.err = D3DPT_ERR_NO_DEVICE; return false; }
    return true;
}

/* The records below are the few this file still answers itself: the
 * queries, and releasing one. Every other op is the display driver's
 * (d3dpt_exec_ddi.cpp). */
static void exec_one(Batch &b, const d3dpt_cmd *c) {
    Exec &x = b.x;
    switch (c->op) {
    case D3DPT_OP_NOP:
        break;
    case D3DPT_OP_RELEASE: {
        auto *a = body<d3dpt_handle>(c, 0, b); if (!a) return;
        auto it = x.objs.find(a->handle);
        if (it == x.objs.end()) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        it->second.p->Release();
        x.objs.erase(it);
        break;
    }
    case D3DPT_OP_CREATE_QUERY: {
        auto *a = body<d3dpt_create_query>(c, 0, b); if (!a) return;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return;
        if (!need_device(b)) return;
        IDirect3DQuery9 *q = nullptr;
        r->hr = (uint32_t)x.dev->CreateQuery((D3DQUERYTYPE)a->type, &q);
        if (SUCCEEDED(r->hr) && !x.put(a->handle, K_QUERY, q)) { r->hr = (uint32_t)D3DERR_INVALIDCALL; b.err = D3DPT_ERR_BAD_HANDLE; }
        break;
    }
    case D3DPT_OP_QUERY_ISSUE: {
        auto *a = body<d3dpt_u32x2>(c, 0, b); if (!a) return;
        IDirect3DQuery9 *q = x.get<IDirect3DQuery9>(a->a, K_QUERY);
        if (!q) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        q->Issue(a->b & (D3DISSUE_BEGIN | D3DISSUE_END));
        break;
    }
    case D3DPT_OP_QUERY_GET_DATA: {
        auto *a = body<d3dpt_query_get>(c, 0, b); if (!a) return;
        if (a->size > 256) { b.err = D3DPT_ERR_BAD_ARG; return; }
        d3dpt_ret *r = b.slot(a->ret_off, a->size); if (!r) return;
        IDirect3DQuery9 *q = x.get<IDirect3DQuery9>(a->handle, K_QUERY);
        if (!q) { b.err = D3DPT_ERR_BAD_HANDLE; return; }
        r->hr = (uint32_t)q->GetData(a->size ? (void *)(r + 1) : nullptr, a->size, a->flags & D3DGETDATA_FLUSH);
        r->bytes = r->hr == S_OK ? a->size : 0;
        break;
    }
    default:
        if (!exec_ddi_op(b, c)) b.err = D3DPT_ERR_BAD_OP;
        break;
    }
}

} // namespace

namespace d3dpt {

#ifdef _WIN32
/* The window Windows' own Direct3D 9 will not make a device without:
 * hFocusWindow may only be NULL when hDeviceWindow is not, and a
 * swapchain needs a real HWND either way. It is never shown and never
 * pumped (nothing of the guest's frame goes through it, the pixels
 * leave through GetRenderTargetData), so a 1x1 WS_POPUP off-screen is
 * the whole of it. DXVK's headless WSI needs none and is given none. */
static HWND host_window(Exec &x)
{
    if (x.hwnd) return x.hwnd;
    static const wchar_t *cls = L"2ksboxD3DPT";
    static bool registered;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = cls;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            x.log("no window class for the Direct3D device: error %lu", (unsigned long)GetLastError());
            return nullptr;
        }
        registered = true;
    }
    x.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls, L"2ksbox Direct3D", WS_POPUP,
                             -32000, -32000, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!x.hwnd) x.log("no window for the Direct3D device: error %lu", (unsigned long)GetLastError());
    return x.hwnd;
}
#endif

/* CreateDevice for both backends. DXVK takes every
 * combination the guest asks for; the system implementation has a real
 * driver under it and refuses some, so each refusal is retried once with
 * the nearest thing that works and said out loud. */
HRESULT exec_create_device(Exec &x, UINT adapter, DWORD flags, D3DPRESENT_PARAMETERS &pp, IDirect3DDevice9 **dev)
{
    HWND focus = nullptr;
    *dev = nullptr;
#ifdef _WIN32
    if (x.native) {
        focus = host_window(x);
        if (!focus) return E_FAIL;
        pp.hDeviceWindow = focus;
    }
#endif
    auto attempt = [&](DWORD f) {
        HRESULT hr;
        try { hr = x.d3d->CreateDevice(adapter, D3DDEVTYPE_HAL, focus, f, &pp, dev); }
        catch (...) { hr = E_FAIL; *dev = nullptr; }
        return hr;
    };
    HRESULT hr = attempt(flags);
    if (FAILED(hr) && x.native && (flags & D3DCREATE_HARDWARE_VERTEXPROCESSING)) {
        /* a card whose D3D9 driver has no hardware T&L (and every
         * D3DCREATE_PUREDEVICE with it) */
        DWORD f = (flags & ~(DWORD)(D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE)) |
                  D3DCREATE_SOFTWARE_VERTEXPROCESSING;
        hr = attempt(f);
        if (SUCCEEDED(hr)) x.log("this host's Direct3D 9 refused hardware vertex processing: software vertex processing");
    }
    if (FAILED(hr) && x.native) {
        /* a windowed device's backbuffer format has to be one the driver
         * will present to this desktop; DXVK's swapchain is an offscreen
         * image and takes any. The frame leaves through a readback, whose
         * format comes from the surface description, so the guest's own
         * idea of the format is untouched by this. */
        D3DDISPLAYMODE dm;
        if (SUCCEEDED(x.d3d->GetAdapterDisplayMode(adapter, &dm)) && dm.Format != pp.BackBufferFormat) {
            D3DFORMAT asked = pp.BackBufferFormat;
            pp.BackBufferFormat = dm.Format;
            hr = attempt(flags);
            if (SUCCEEDED(hr)) x.log("this host's Direct3D 9 refused a windowed %u backbuffer: the desktop's %u instead",
                                     (unsigned)asked, (unsigned)dm.Format);
            else pp.BackBufferFormat = asked;
        }
    }
    if (SUCCEEDED(hr) && *dev) { x.last_pp = pp; x.lost = false; }
    return hr;
}

/* Native only: has the device been lost (a driver reset, a mode switch by
 * something else on the desktop), and can it be brought back? DXVK's
 * device is never lost, so nothing asks there. Called once per batch:
 * TestCooperativeLevel is a cheap state read. */
static void check_lost(Exec &x)
{
    if (!x.native || !x.dev) return;
    HRESULT hr = x.dev->TestCooperativeLevel();
    if (SUCCEEDED(hr)) {
        if (x.lost) { x.log("the Direct3D device is back"); x.lost = false; }
        return;
    }
    if (!x.lost) {
        x.lost = true;
        x.scene = false;        /* the scene went with it */
        x.log("the host's Direct3D device was lost (0x%08x)", (unsigned)hr);
    }
    if (hr != D3DERR_DEVICENOTRESET) return;    /* still gone; try again next batch */
    /* everything in the default pool has to go before a Reset, and is
     * made again from guest VRAM afterwards */
    exec_ddi_device_reset(x);
    D3DPRESENT_PARAMETERS pp = x.last_pp;
    hr = x.dev->Reset(&pp);
    x.log("Reset of the lost device -> 0x%08x", (unsigned)hr);
    if (SUCCEEDED(hr)) { x.last_pp = pp; x.lost = false; }
}

/* open one d3d9 library and ask it for an adapter. An interface with no
 * adapter behind it is a failure here: that is what DXVK gives on a host
 * below Vulkan 1.3 (every adapter leaves DxvkDeviceCapabilities early), and
 * it is the case the whole fallback exists for. */
static bool open_d3d9(Exec *x, const char *path, bool dxvk)
{
#ifndef _WIN32
    /* DXVK's own precondition, asked here first: with no Vulkan loader on
     * the host at all its Direct3DCreate9 logs "vkGetInstanceProcAddr not
     * found" and then calls through a null pointer (seen on the Air with
     * DYLD_LIBRARY_PATH unset: a segfault, in the host test and in QEMU's
     * realize alike). A host with the loader and no working device is
     * DXVK's to refuse, and it does, by a C++ exception out of
     * Direct3DCreate9 (its DxvkInstance constructor throws when the ICD
     * gives no GPU: vkEnumeratePhysicalDevices failing, or no ICD at all),
     * which the catch below takes; see the once-per-library rule after
     * the dlopen for what that exception leaves behind. */
    if (dxvk) {
        static void *loader;
        if (!loader) {
#if defined(__APPLE__)
            loader = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
            if (!loader) loader = dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
#else
            loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
#endif
        }
        if (!loader) { x->log("no Vulkan loader on this host (libvulkan): %s not tried", path); return false; }
    }
#endif
    void *h = D3DPT_DLOPEN(path);
    if (!h) return false;
    /* One failed Direct3DCreate9 per library, ever. The candidate list
     * names the same DXVK more than once (a full path from the player,
     * then the bare leaf name, which dlopen / LoadLibrary answer with the
     * image already loaded under that name, the same handle). DXVK's
     * d3d9 keeps its Vulkan instance in a process-wide Singleton whose
     * acquire() counts a user *before* constructing the instance. A
     * constructor that threw leaves the count at one and the object null,
     * and the next Direct3DCreate9 in the process hands the interface that
     * null instance and faults in D3D9Options. The community app on
     * macOS 15 hit this: KosmicKrisp loads and reports no GPU, the player
     * died at the adapter's realize on the *second* candidate, and the
     * Wine executor it should have moved on to was never reached.
     * Our DXVK patch 09 fixes the count; this keeps every other d3d9 with
     * the same shape, and the same DXVK unpatched, from being asked twice. */
    static std::vector<void *> refused;
    if (std::find(refused.begin(), refused.end(), h) != refused.end()) {
        x->log("%s: the library already asked and refused; not asked again", path);
        return false;   /* the extra dlopen reference is nothing; never closed (see below) */
    }
    auto create = (IDirect3D9 *(*)(UINT))D3DPT_DLSYM(h, "Direct3DCreate9");
    if (!create) { x->log("no Direct3DCreate9 in %s", path); D3DPT_DLCLOSE(h); return false; }
    IDirect3D9 *d3d = nullptr;
    try { d3d = create(D3D_SDK_VERSION); } catch (...) { d3d = nullptr; }
    D3DADAPTER_IDENTIFIER9 id;
    if (d3d && (d3d->GetAdapterCount() == 0 || FAILED(d3d->GetAdapterIdentifier(0, 0, &id)))) {
        d3d->Release();
        d3d = nullptr;
        x->log("%s: no adapter", path);
    }
    if (!d3d) {
        if (dxvk) x->log("%s: Direct3DCreate9 found no usable device", path);
        /* left mapped: DXVK has started threads and statics by now, and
         * unloading it under them is not safe */
        refused.push_back(h);
        return false;
    }
    x->dxvk = h;
    x->d3d = d3d;
    x->native = !dxvk;
    x->log("d3d9: %s%s, adapter \"%s\"", path, dxvk ? "" : " (this host's own Direct3D 9)", id.Description);
    return true;
}

static bool open_dxvk(Exec *x)
{
    const char *lib = getenv("D3DPT_DXVK_LIB");
    const char *candidates[] = { lib,
#ifdef _WIN32
        /* not "d3d9.dll": that name is Windows' own implementation's too.
         * The player names the packaged copy by its full path. */
        "dxvk_d3d9.dll",
#else
        "build/dxvk/src/d3d9/libdxvk_d3d9.so.0",
#ifdef __APPLE__
        "build/dxvk/src/d3d9/libdxvk_d3d9.0.dylib", "libdxvk_d3d9.0.dylib",
#else
        "libdxvk_d3d9.so.0",
#endif
#endif
        nullptr };
#ifdef _WIN32
    /* DXVK reads it through GetEnvironmentVariableW, so the process
     * environment rather than this module's C runtime. Not overwritten if set. */
    if (!GetEnvironmentVariableA("DXVK_WSI_DRIVER", nullptr, 0)) SetEnvironmentVariableA("DXVK_WSI_DRIVER", "Headless");
#else
    setenv("DXVK_WSI_DRIVER", "Headless", 0);
#endif
    for (const char **c = candidates; *c || c == candidates; c++)
        if (*c && open_d3d9(x, *c, true)) return true;
    return false;
}

static bool open_native(Exec *x)
{
#ifdef _WIN32
    /* by its full path: a bare "d3d9.dll" would take whatever sits beside
     * the player's own exe, which in the package is DXVK's copy */
    char dir[MAX_PATH];
    UINT n = GetSystemDirectoryA(dir, MAX_PATH);
    std::string path = n && n < MAX_PATH ? std::string(dir) + "\\d3d9.dll" : std::string("d3d9.dll");
    if (open_d3d9(x, path.c_str(), false)) return true;
    x->log("this host's own Direct3D 9 (%s) has no adapter either", path.c_str());
    return false;
#else
    (void)x;
    return false;
#endif
}

} // namespace d3dpt

extern "C" {

uint32_t d3dpt_exec_version(void) { return D3DPT_PROTO_VERSION; }

d3dpt_exec_t *d3dpt_exec_create(const d3dpt_exec_ops *ops)
{
    Exec *x = new Exec;
    x->ops = *ops;
    /* D3DPT_D3D9=auto|dxvk|system (the adapter's `d3d9=` property is how a
     * machine says it); the file header has what each one means. QEMU sets
     * it in the *process* environment, which is not this module's C
     * runtime's copy of it on Windows. */
    const char *pick = getenv("D3DPT_D3D9");
#ifdef _WIN32
    char picked[16];
    if (!pick || !*pick) {
        DWORD n = GetEnvironmentVariableA("D3DPT_D3D9", picked, sizeof picked);
        if (n && n < sizeof picked) pick = picked;
    }
#endif
    if (!pick || !*pick) pick = "auto";
    bool ok;
    if (!strcmp(pick, "system")) {
        /* A machine file is portable and this setting is about the host:
         * a machine set to the system Direct3D 9 on Windows and then
         * opened on Linux or macOS gets DXVK rather than no 3D at all. */
        ok = open_native(x);
        if (!ok) {
            x->log("d3d9=system: no system Direct3D 9 on this host; DXVK instead");
            ok = open_dxvk(x);
        }
    } else if (!strcmp(pick, "dxvk")) {
        ok = open_dxvk(x);
    } else {
        if (strcmp(pick, "auto")) x->log("D3DPT_D3D9=%s is not auto, dxvk or system: taking auto", pick);
        ok = open_dxvk(x) || open_native(x);
    }
    if (!ok) { x->log("no usable d3d9 implementation (D3DPT_D3D9=%s)", pick); delete x; return nullptr; }
    return (d3dpt_exec_t *)x;
}

void d3dpt_exec_destroy(d3dpt_exec_t *xp)
{
    Exec *x = (Exec *)xp;
    if (!x) return;
    x->release_all();
    if (x->d3d) x->d3d->Release();
#ifdef _WIN32
    if (x->hwnd) DestroyWindow(x->hwnd);
#endif
    /* DXVK keeps worker threads; leave the library mapped */
    delete x;
}

void d3dpt_exec_attach(d3dpt_exec_t *xp, int attach)
{
    Exec *x = (Exec *)xp;
    if (!x) return;
    if (attach) x->attach++;
    else if (x->attach > 0 && --x->attach == 0) x->release_all();
}

void d3dpt_exec_set_vram(d3dpt_exec_t *xp, void *vram, uint32_t size)
{
    Exec *x = (Exec *)xp;
    if (!x) return;
    x->vram = (uint8_t *)vram;
    x->vram_size = vram ? size : 0;
}

uint32_t d3dpt_exec_submit(d3dpt_exec_t *xp, void *shm, uint32_t shm_size)
{
    Exec *x = (Exec *)xp;
    if (!x || shm_size < D3DPT_SHM_SIZE) return D3DPT_ERR_HOST;
    d3dpt_shm_hdr *hdr = (d3dpt_shm_hdr *)shm;
    check_lost(*x);
    Batch b = { *x, (uint8_t *)shm, hdr, (uint8_t *)shm + D3DPT_RET_OFFSET };
    uint32_t bytes = hdr->cmd_bytes, count = hdr->cmd_count;
    hdr->batches++;
    if (bytes > D3DPT_CMD_SIZE || bytes % 8) { b.err = D3DPT_ERR_MALFORMED; }
    const uint8_t *p = (const uint8_t *)shm + D3DPT_CMD_OFFSET, *end = p + (b.err ? 0 : bytes);
    while (!b.err && p < end) {
        if (end - p < (ptrdiff_t)sizeof(d3dpt_cmd)) { b.err = D3DPT_ERR_MALFORMED; break; }
        const d3dpt_cmd *c = (const d3dpt_cmd *)p;
        if (c->size < sizeof(d3dpt_cmd) || c->size % 8 || c->size > (uint32_t)(end - p)) { b.err = D3DPT_ERR_MALFORMED; break; }
        /* a record must never take the process down: an exception (a
         * std::bad_alloc out of DXVK on a garbage count, say) refuses the
         * batch like a malformed record does */
        try { exec_one(b, c); }
        catch (const std::exception &e) { x->log("record %u (op %u) threw: %s", b.index, c->op, e.what()); if (!b.err) b.err = D3DPT_ERR_HOST; }
        catch (...) { x->log("record %u (op %u) threw", b.index, c->op); if (!b.err) b.err = D3DPT_ERR_HOST; }
        if (b.err) break;       /* index and p stay on the record that failed: the log and ret_index name it */
        p += c->size;
        b.index++;
    }
    if (!b.err && b.index != count) b.err = D3DPT_ERR_MALFORMED;
    if (b.err) x->log("batch error %u at record %u (op %u), %u bytes %u records", b.err, b.index,
                      b.index < count && p < end ? ((const d3dpt_cmd *)p)->op : 0, bytes, count);
    hdr->ret_status = b.err;
    hdr->ret_index = b.index;
    hdr->cmd_bytes = 0;
    hdr->cmd_count = 0;
    return b.err;
}

static void probe_log(void *, const char *msg) { fprintf(stderr, "d3dpt: probe: %s\n", msg); }

/* Is there a Direct3D 9 to run on here at all? A create and a destroy,
 * with nothing between: the loader asks this before it settles on a
 * library, so a host below the Vulkan floor can move on to the executor
 * in another process (libd3dpt_exec_remote) instead of keeping this one. */
int d3dpt_exec_probe(void)
{
    d3dpt_exec_ops ops = { nullptr, probe_log, nullptr, nullptr };
    d3dpt_exec_t *x = d3dpt_exec_create(&ops);
    if (!x) return 0;
    d3dpt_exec_destroy(x);
    return 1;
}

} // extern "C"
