/*
 * d3dpt_exec_d11.cpp: the executor's Direct3D 10 / 11 front end (track
 * M25, ADR-029): the D11 records (d3dpt_proto.h, v25) the WDDM driver's
 * Direct3D 11 user-mode driver (guest-tools/src/d3dptvid/wddm/um11)
 * writes, run on one host d3d11 device for the whole guest.
 *
 * The device is DXVK's d3d11 (ADR-007), opened through
 * D3D11CoreCreateDevice with DXVK's own DXGI adapter; on Windows both are
 * loaded by file name beside dxvk_d3d9.dll (dxvk_d3d11.dll,
 * dxvk_dxgi.dll), since the process already holds Windows' own dxgi.dll.
 * Where the Direct3D 9 side runs on the system's Direct3D 9 (Exec::native:
 * D3DPT_D3D9=system, or Wine's in the out-of-process executor) this side
 * takes the system's Direct3D 11 the same way.
 *
 * Each guest device is a context of its own (the kernel driver's host
 * context id): an ID3DDeviceContextState with every binding it made,
 * swapped in when its record runs, and its objects (views, states,
 * shaders, input layouts, queries) by the ids it picked. Resources are the
 * whole guest's, by the allocation's host handle (the Direct3D 9 side's
 * VRAM surface registration says where its memory is): the host object
 * is the resource, its VRAM only a window the guest's CPU looks through
 * (RES_DIRTY, RES_READBACK).
 *
 * Shaders arrive as the runtime gave the driver: the program's tokens and
 * its input / output signatures by register. This file writes the DXBC
 * container around them (ISGN / OSGN with names of its own, the code
 * chunk, DXBC's MD5 hash, which DXVK checks) and creates the shader from
 * it; input layouts get an input signature of the same names.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3dpt_exec_int.h"
#include <d3d11_1.h>
#include <dxgi.h>
#ifdef D3DPT_RING
/* the scanout ring (track M26): the player's images (shared Direct3D 12
 * resources, Metal heaps) imported as Vulkan images on DXVK's own device
 * (d3dpt_exec_int.h's Vulkan), and wrapped as d3d11 textures */
#include <d3d11_3.h>

/* DXVK's interop interfaces (third_party/dxvk/src/dxgi/dxgi_interfaces.h),
 * declared as DXVK declares them, in vtable order */
MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09323")
IDXGIVkInteropDevice : public IUnknown {
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance *pInstance, VkPhysicalDevice *pPhysDev, VkDevice *pDevice) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue *pQueue, uint32_t *pQueueFamilyIndex) = 0;
    virtual void STDMETHODCALLTYPE TransitionSurfaceLayout(IUnknown *pSurface, const VkImageSubresourceRange *pSubresources,
                                                           VkImageLayout OldLayout, VkImageLayout NewLayout) = 0;
    virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
    virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
    virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
};
MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09324")
IDXGIVkInteropDevice1 : public IDXGIVkInteropDevice {
    virtual void STDMETHODCALLTYPE GetSubmissionQueue1(VkQueue *pQueue, uint32_t *pQueueIndex, uint32_t *pQueueFamilyIndex) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTexture2DFromVkImage(const D3D11_TEXTURE2D_DESC1 *pDesc, VkImage vkImage,
                                                                 ID3D11Texture2D **ppTexture2D) = 0;
};
#ifndef _MSC_VER
__CRT_UUID_DECL(IDXGIVkInteropDevice,  0xe2ef5fa5, 0xdc21, 0x4af7, 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x23);
__CRT_UUID_DECL(IDXGIVkInteropDevice1, 0xe2ef5fa5, 0xdc21, 0x4af7, 0x90, 0xc4, 0xf6, 0x7e, 0xf6, 0xa0, 0x93, 0x24);
#endif
#endif
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define D11_DLOPEN(p)     ((void *)LoadLibraryA(p))
#define D11_DLSYM(h, s)   ((void *)GetProcAddress((HMODULE)(h), (s)))
#else
#include <dlfcn.h>
#define D11_DLOPEN(p)     dlopen((p), RTLD_NOW | RTLD_LOCAL)
#define D11_DLSYM(h, s)   dlsym((h), (s))
#ifdef __APPLE__
#include <cstring>
#include <mach-o/dyld.h>        /* the scanout ring finds DXVK's Vulkan loader (exec_vk_procs) */
#endif
#ifdef __linux__
#include <cerrno>               /* the scanout ring dups the player's fds (exec_vk_ring_image) */
#include <cstring>
#include <unistd.h>
#endif
#endif

namespace d3dpt {

namespace {

/* ------------------------------------------------------------ the state */

struct D11Res {
    d3dpt_d11_res d{};
    std::vector<d3dpt_d11_sub> layout;          /* subresources in VRAM (empty: the host's alone) */
    ID3D11Resource *res = nullptr;
    ID3D11Resource *stage = nullptr;            /* a readback's staging copy */
    ID3D11Texture2D *resolve = nullptr;         /* a multisampled one's resolve target */
    bool staging = false;
    /* the host drew, cleared or copied into it since VRAM last had its
     * pixels: a resource in VRAM that Direct3D 9 code may also see (a
     * shared surface DWM composes) is read back before that code uses it
     * (exec_d11_sync) */
    bool newer = false;
    ID3D11RenderTargetView *gdi_rtv = nullptr;  /* GDI's fills (v27) */
    /* a redirection bitmap's VRAM as the host last saw it (GDI's REFRESH):
     * what differs from it the CPU drew since */
    std::vector<uint8_t> gdi_shadow;
    uint32_t gdi_w = 0, gdi_h = 0;
    uint64_t bytes = 0;                         /* res and stage, in D11::res_bytes (res_drop takes them out) */
    void release() {
        if (res) res->Release();
        if (stage) stage->Release();
        if (resolve) resolve->Release();
        if (gdi_rtv) gdi_rtv->Release();
        res = nullptr; stage = nullptr; resolve = nullptr; gdi_rtv = nullptr;
    }
};

struct D11Obj { uint32_t kind = 0; IUnknown *p = nullptr; uint32_t stage = 0; uint32_t res = 0; /* a view's resource */ };

struct D11Ctx {
    ID3DDeviceContextState *state = nullptr;
    std::unordered_map<uint32_t, D11Obj> objs;
    uint32_t rtv[8] = {}, nrtv = 0, dsv = 0;    /* the bound targets (ids), whose resources a draw marks newer */
    bool pred_skip = false;                     /* the predicate set holds: predicated operations are skipped (SET_PREDICATION) */
    void release() {
        for (auto &kv : objs) if (kv.second.p) kv.second.p->Release();
        objs.clear();
        if (state) state->Release();
        state = nullptr;
    }
};

struct D11 {
    bool tried = false;
    ID3D11Device1 *dev = nullptr;
    ID3D11DeviceContext1 *ctx = nullptr;
    ID3DDeviceContextState *idle = nullptr;     /* the state while no guest context is in */
    uint32_t cur = 0;                           /* the guest context whose state is in */
    std::unordered_map<uint32_t, D11Res> res;
    std::unordered_map<uint32_t, D11Ctx> ctxs;
    uint64_t res_bytes = 0;                     /* every resource's host memory, against D11_RES_BUDGET */
    size_t nobjs = 0;                           /* every context's objects, against D11_MAX_OBJS */
    /* GDI's readbacks (v27, v29): BGRA8 staging textures kept between
     * operations, grown to the largest rectangle read (in steps of 64),
     * one per role, so one operation reads its destination, source and
     * coverage at once; and the copy an overlapping scroll goes through */
    enum { GDI_D, GDI_S, GDI_A, GDI_TMP, GDI_N };
    struct GdiTex { ID3D11Texture2D *tex = nullptr; uint32_t w = 0, h = 0; } gdi_tex[GDI_N];
    std::unordered_map<uint32_t, int> warned;
    bool warn(uint32_t key) { return warned[key]++ == 0; }
    /* D3DPT_D11_PROF set: each token kind's count and time, logged every 5 s;
     * 64 + op the GDI records (v27: COPY, FILL, REFRESH, SYNC); 72 and up
     * are parts of one (not in the total): 72 a readback's wait for its
     * staging copy (the GPU), 73 its copy into VRAM */
    bool prof = false;
    std::chrono::steady_clock::time_point prof_t0;
    uint64_t prof_ns[128] = {}, prof_n[128] = {};
    /* v28: flips' readbacks copied to staging and not yet into VRAM
     * (RES_SCANOUT): an allocation's subresources first..last-1 */
    struct Owed { uint32_t h, first, last; };
    std::vector<Owed> owed;
    /* the scanout ring (M26, d3dpt_exec_ring): the player's images, a
     * flip's frame copied into the next one instead of read back. `scans`
     * says which slot holds the frame of the page at a VRAM offset, the
     * allocation it came from, until the guest's CPU writes that
     * allocation or it goes */
    struct Slot {
        ID3D11Texture2D *tex = nullptr;
        ID3D11Query *done = nullptr;            /* the last copy into it finished */
#ifdef D3DPT_RING
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
#endif
    };
    std::vector<Slot> ring;
    uint32_t ring_w = 0, ring_h = 0, ring_next = 0;
    struct Scan { uint32_t h; uint32_t slot; };
    std::unordered_map<uint32_t, Scan> scans;
#ifdef D3DPT_RING
    VkDevice vk = VK_NULL_HANDLE;
    PFN_vkDestroyImage vkDestroyImage = nullptr;
    PFN_vkFreeMemory vkFreeMemory = nullptr;
#endif
    void prof_part(uint32_t i, std::chrono::steady_clock::time_point &t) {
        auto now = std::chrono::steady_clock::now();
        prof_ns[i] += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - t).count();
        prof_n[i]++;
        t = now;
    }
};

D11 *&d11_of(Exec &x)
{
    return reinterpret_cast<D11 *&>(x.d11);
}

/* How much the guest may make. Each object comes from a few bytes of
 * record, so a broken or hostile driver could exhaust host memory and the
 * GPU's; the caps are far above Windows 11's desktop with its programs
 * open and a game running: a few hundred guest devices, tens of thousands
 * of allocations, a few GiB of them. Past one, the creation fails as one
 * Direct3D 11 refuses (E_OUTOFMEMORY / E_INVALIDARG), said once. */
constexpr size_t D11_MAX_CTXS = 4096;          /* each an ID3DDeviceContextState */
constexpr size_t D11_MAX_RES = 262144;          /* allocations (the kernel driver's handles) */
constexpr size_t D11_MAX_OBJS = 1048576;        /* views, states, shaders, layouts, queries: every context's */
constexpr uint64_t D11_RES_BUDGET = 8ull << 30; /* resources' memory, staging twins included */

/* -------------------------------------------------------------- formats */

/* bytes per element (a block for BC formats) and the block's width; 0: unknown */
uint32_t dxgi_bytes(uint32_t f, uint32_t *block)
{
    *block = 1;
    if (f >= 1 && f <= 4) return 16;
    if (f >= 5 && f <= 8) return 12;
    if (f >= 9 && f <= 22) return 8;
    if ((f >= 23 && f <= 47) || (f >= 67 && f <= 69) || (f >= 87 && f <= 93)) return 4;
    if ((f >= 48 && f <= 59) || f == 85 || f == 86 || f == 115) return 2;
    if (f >= 60 && f <= 65) return 1;
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) { *block = 4; return 8; }
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) { *block = 4; return 16; }
    return 0;
}

/* --------------------------------------------------------- DXBC writing */

/* MD5's block function, for DXBC's hash (an MD5 over everything after the
 * hash field with its own padding; dxbc-spirv's hashDxbcBinary, which
 * DXVK checks every shader against) */
struct Md5 {
    uint32_t s[4] = { 0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u };
    uint8_t buf[64];
    size_t n = 0;
    static uint32_t rol(uint32_t v, int c) { return (v << c) | (v >> (32 - c)); }
    void block(const uint8_t *p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
        static const int R[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                   5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
        uint32_t m[16], a = s[0], b = s[1], c = s[2], d = s[3];
        for (int i = 0; i < 16; i++) m[i] = p[i * 4] | (p[i * 4 + 1] << 8) | (p[i * 4 + 2] << 16) | ((uint32_t)p[i * 4 + 3] << 24);
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            uint32_t t = d;
            d = c;
            c = b;
            b = b + rol(a + f + K[i] + m[g], R[i]);
            a = t;
        }
        s[0] += a; s[1] += b; s[2] += c; s[3] += d;
    }
    void update(const void *data, size_t size) {
        const uint8_t *p = (const uint8_t *)data;
        while (size) {
            size_t k = std::min(size, 64 - n);
            memcpy(buf + n, p, k);
            n += k; p += k; size -= k;
            if (n == 64) { block(buf); n = 0; }
        }
    }
};

void dxbc_hash(const uint8_t *data, size_t size, uint8_t out[16])
{
    const size_t skip = 20;                     /* "DXBC" and the hash itself */
    const uint8_t *bytes = data + skip;
    size -= skip;
    uint32_t a_num = (uint32_t)size * 8u, b_num = (a_num >> 2) | 1u;
    uint8_t a[4], b[4], pad[64] = { 0x80 };
    for (int i = 0; i < 4; i++) { a[i] = (uint8_t)(a_num >> (8 * i)); b[i] = (uint8_t)(b_num >> (8 * i)); }
    size_t rem = size % 64, padding = 64 - rem;
    Md5 h;
    h.update(bytes, size - rem);
    if (rem >= 56) {
        h.update(bytes + size - rem, rem);
        h.update(pad, padding);
        h.update(a, 4);
        h.update(pad + 4, 64 - 4 - 4);
        h.update(b, 4);
    } else {
        h.update(a, 4);
        if (rem) h.update(bytes + size - rem, rem);
        h.update(pad, padding - 4 - 4);
        h.update(b, 4);
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i * 4 + j] = (uint8_t)(h.s[i] >> (8 * j));
}

/* one signature entry as the container holds it */
struct SigEntry { std::string name; uint32_t index, sv, type, reg, mask, rw; };

void put32(std::vector<uint8_t> &v, uint32_t x)
{
    for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}

/* an ISGN / OSGN chunk's body: the count, 8, the entries, their names */
std::vector<uint8_t> sig_chunk(const std::vector<SigEntry> &e)
{
    std::vector<uint8_t> v;
    put32(v, (uint32_t)e.size());
    put32(v, 8);
    uint32_t names = 8 + 24 * (uint32_t)e.size();
    std::vector<uint32_t> at;
    std::string strs;
    for (auto &s : e) { at.push_back(names + (uint32_t)strs.size()); strs += s.name; strs.push_back(0); }
    for (size_t i = 0; i < e.size(); i++) {
        put32(v, at[i]);
        put32(v, e[i].index);
        put32(v, e[i].sv);
        put32(v, e[i].type);
        put32(v, e[i].reg);
        v.push_back((uint8_t)e[i].mask);
        v.push_back((uint8_t)e[i].rw);
        v.push_back(0);
        v.push_back(0);
    }
    v.insert(v.end(), strs.begin(), strs.end());
    while (v.size() % 4) v.push_back(0);
    return v;
}

/* the container: its chunks, then the hash over it */
std::vector<uint8_t> dxbc(const std::vector<std::pair<const char *, std::vector<uint8_t>>> &chunks)
{
    std::vector<uint8_t> v;
    v.insert(v.end(), { 'D', 'X', 'B', 'C' });
    v.resize(4 + 16, 0);
    put32(v, 1);
    size_t size_at = v.size();
    put32(v, 0);
    put32(v, (uint32_t)chunks.size());
    size_t offs_at = v.size();
    for (size_t i = 0; i < chunks.size(); i++) put32(v, 0);
    for (size_t i = 0; i < chunks.size(); i++) {
        uint32_t off = (uint32_t)v.size();
        memcpy(&v[offs_at + 4 * i], &off, 4);
        v.insert(v.end(), chunks[i].first, chunks[i].first + 4);
        put32(v, (uint32_t)chunks[i].second.size());
        v.insert(v.end(), chunks[i].second.begin(), chunks[i].second.end());
    }
    uint32_t total = (uint32_t)v.size();
    memcpy(&v[size_at], &total, 4);
    dxbc_hash(v.data(), v.size(), &v[4]);
    return v;
}

/* a signature's entries from the DDI's: register-named semantics
 * (D3DPT<register> for an input of the vertex shader, which an input
 * layout names the same way; D3DPT<register * 4 + component> otherwise),
 * the system values by their own names. ps_out: a pixel shader's outputs
 * are its targets. */
std::vector<SigEntry> sig_entries(const d3dpt_d11_sig *s, uint32_t n, bool vs_in, bool ps_out, bool input)
{
    static const char *sv_names[] = { nullptr, "SV_Position", "SV_ClipDistance", "SV_CullDistance", "SV_RenderTargetArrayIndex",
                                      "SV_ViewportArrayIndex", "SV_VertexID", "SV_PrimitiveID", "SV_InstanceID", "SV_IsFrontFace",
                                      "SV_SampleIndex", "SV_TessFactor", "SV_InsideTessFactor", "SV_TessFactor", "SV_InsideTessFactor",
                                      "SV_TessFactor", "SV_TessFactor" };
    std::vector<SigEntry> e;
    uint32_t clip = 0, cull = 0;
    for (uint32_t i = 0; i < n; i++) {
        SigEntry x;
        uint32_t first = 0;
        while (first < 4 && !(s[i].mask & (1u << first))) first++;
        x.reg = s[i].reg;
        x.mask = s[i].mask & 0xf;
        x.rw = input ? x.mask : 0;
        x.sv = s[i].sv;
        x.type = s[i].type ? s[i].type : 3;     /* the 11.1 interface's component type; float without one */
        if (ps_out && !x.sv) {
            x.name = "SV_Target";
            x.index = s[i].reg;
            x.sv = 64;
        } else if (x.sv && x.sv < sizeof(sv_names) / sizeof(*sv_names)) {
            x.name = sv_names[x.sv];
            x.index = x.sv == 2 ? clip++ : x.sv == 3 ? cull++ : 0;
            if (x.sv >= 4 && x.sv <= 10 && !s[i].type) x.type = 1;    /* the integer ones */
        } else {
            x.name = "D3DPT";
            x.index = vs_in ? s[i].reg : s[i].reg * 4 + first;
            x.sv = 0;
        }
        e.push_back(x);
    }
    return e;
}

/* ------------------------------------------------------------ the device */

bool d11_open(Exec &x, D11 &d)
{
    d.tried = true;
    typedef HRESULT (WINAPI *CoreCreate)(IDXGIFactory *, IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *,
                                          UINT, UINT, ID3D11Device **, D3D_FEATURE_LEVEL *);
    typedef HRESULT (WINAPI *CreateFactory)(REFIID, void **);
    typedef HRESULT (WINAPI *CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
                                            ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
    const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    D3D_FEATURE_LEVEL got = D3D_FEATURE_LEVEL_11_0;
    ID3D11Device *dev = nullptr;
    HRESULT hr = E_FAIL;
    std::string where;

    if (x.native) {
#ifdef _WIN32
        /* the system's Direct3D 11 beside the system's Direct3D 9 */
        char dir[MAX_PATH];
        UINT n = GetSystemDirectoryA(dir, MAX_PATH);
        where = n && n < MAX_PATH ? std::string(dir) + "\\d3d11.dll" : std::string("d3d11.dll");
#else
        where = "d3d11.dll";
#endif
        void *h = D11_DLOPEN(where.c_str());
        auto create = h ? (CreateDevice)D11_DLSYM(h, "D3D11CreateDevice") : nullptr;
        if (create) hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got, nullptr);
    } else {
        /* DXVK's pair: by the environment (a package's player names both,
         * player-core's companions.rs), else beside DXVK's d3d9 when a
         * harness named only that, else the checkout's build */
        std::string d3d11, dxgi;
        const char *e11 = env("D3DPT_DXVK_D3D11_LIB"), *eg = env("D3DPT_DXVK_DXGI_LIB"), *e9 = env("D3DPT_DXVK_LIB");
        std::string dir;
        if (e9) {
            dir = e9;
            size_t s = dir.find_last_of("\\/");
            dir = s == std::string::npos ? std::string() : dir.substr(0, s + 1);
        }
#ifdef _WIN32
        d3d11 = e11 ? e11 : dir + "dxvk_d3d11.dll";
        dxgi = eg ? eg : dir + "dxvk_dxgi.dll";
#else
#ifdef __APPLE__
        const std::string l11 = "libdxvk_d3d11.0.dylib", lg = "libdxvk_dxgi.0.dylib";
#else
        const std::string l11 = "libdxvk_d3d11.so.0", lg = "libdxvk_dxgi.so.0";
#endif
        /* DXVK's build tree keeps each library in a directory of its own
         * (<build>/src/d3d9/, <build>/src/d3d11/, <build>/src/dxgi/) */
        const std::string sub = "/src/d3d9/";
        bool tree = dir.size() >= sub.size() && dir.compare(dir.size() - sub.size(), sub.size(), sub) == 0;
        std::string src = tree ? dir.substr(0, dir.size() - 5) : std::string("build/dxvk/src/");
        d3d11 = e11 ? e11 : e9 && !tree ? dir + l11 : src + "d3d11/" + l11;
        dxgi = eg ? eg : e9 && !tree ? dir + lg : src + "dxgi/" + lg;
#endif
        where = d3d11;
        void *hg = D11_DLOPEN(dxgi.c_str());
#ifndef _WIN32
        if (!hg) hg = D11_DLOPEN(dxgi.substr(dxgi.find_last_of('/') + 1).c_str());
#endif
        void *h11 = hg ? D11_DLOPEN(d3d11.c_str()) : nullptr;
#ifndef _WIN32
        if (hg && !h11) h11 = D11_DLOPEN(d3d11.substr(d3d11.find_last_of('/') + 1).c_str());
#endif
        auto factory = hg ? (CreateFactory)D11_DLSYM(hg, "CreateDXGIFactory1") : nullptr;
        auto core = h11 ? (CoreCreate)D11_DLSYM(h11, "D3D11CoreCreateDevice") : nullptr;
        if (!factory || !core) {
            x.log("d11: no DXVK d3d11 (%s: %s, %s: %s)", dxgi.c_str(), hg ? "loaded" : "not loaded", d3d11.c_str(), h11 ? "loaded" : "not loaded");
            return false;
        }
        IDXGIFactory1 *f = nullptr;
        IDXGIAdapter1 *a = nullptr;
        try {
            hr = factory(__uuidof(IDXGIFactory1), (void **)&f);
            if (SUCCEEDED(hr)) hr = f->EnumAdapters1(0, &a);
            if (SUCCEEDED(hr)) hr = core(f, a, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, &got);
        } catch (...) {
            hr = E_FAIL;
        }
        if (a) a->Release();
        if (f) f->Release();
    }
    if (FAILED(hr) || !dev) {
        x.log("d11: no Direct3D 11 device (%s): 0x%08x", where.c_str(), (unsigned)hr);
        if (dev) dev->Release();
        return false;
    }
    hr = dev->QueryInterface(__uuidof(ID3D11Device1), (void **)&d.dev);
    dev->Release();
    if (FAILED(hr)) {
        x.log("d11: the device has no ID3D11Device1 (0x%08x)", (unsigned)hr);
        return false;
    }
    ID3D11DeviceContext1 *c = nullptr;
    d.dev->GetImmediateContext1(&c);
    d.ctx = c;
    hr = d.dev->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &d.idle);
    if (FAILED(hr)) {
        x.log("d11: no context state (0x%08x)", (unsigned)hr);
        return false;
    }
    d.ctx->SwapDeviceContextState(d.idle, nullptr);
    x.log("d11: Direct3D 11 device (%s), feature level 0x%x", where.c_str(), (unsigned)got);
    d.prof = env("D3DPT_D11_PROF") != nullptr;
    d.prof_t0 = std::chrono::steady_clock::now();
    return true;
}

D11 *d11(Exec &x)
{
    D11 *&d = d11_of(x);
    if (!d) d = new D11;
    if (!d->tried) d11_open(x, *d);
    return d->dev ? d : nullptr;
}

/* the guest context's state in, and its objects */
D11Ctx *enter(Exec &x, D11 &d, uint32_t ctx)
{
    auto it = d.ctxs.find(ctx);
    if (it == d.ctxs.end()) {
        if (d.ctxs.size() >= D11_MAX_CTXS) {
            if (d.warn(0x11000000u)) x.log("d11: context 0x%08x refused: %zu contexts already", ctx, d.ctxs.size());
            return nullptr;
        }
        D11Ctx c;
        const D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = d.dev->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &c.state);
        if (FAILED(hr)) {
            x.log("d11: context 0x%08x: no state (0x%08x)", ctx, (unsigned)hr);
            return nullptr;
        }
        it = d.ctxs.emplace(ctx, c).first;
    }
    if (d.cur != ctx) {
        d.ctx->SwapDeviceContextState(it->second.state, nullptr);
        d.cur = ctx;
    }
    return &it->second;
}

/* ------------------------------------------------------------ resources */

D11Res *res_of(D11 &d, uint32_t h)
{
    auto it = d.res.find(h);
    return it == d.res.end() || !it->second.res ? nullptr : &it->second;
}

/* A resource's host memory, every mip, slice and sample (an unknown
 * format at 16 bytes an element, the most there is); 0 when it is beyond
 * what Direct3D 11 makes at all: 16384 texels a side (2048 in 3D), 2048
 * slices, 15 levels, 32 samples, 2 GiB (its largest resource) */
uint64_t res_size(const d3dpt_d11_res &s)
{
    const uint64_t most = 2ull << 30;
    if (s.dim == 1) return s.width <= most ? s.width : 0;
    uint32_t side = s.dim == 4 ? 2048 : 16384;
    if (s.dim < 2 || s.dim > 4 || s.width > side || (s.dim >= 3 && s.height > side) || (s.dim == 4 && s.depth > side) ||
        s.array > (s.dim == 4 ? 1u : 2048u) || s.mips > 15 || s.samples > 32)
        return 0;
    uint32_t block, bpe = dxgi_bytes(s.format, &block);
    if (!bpe) { bpe = 16; block = 1; }
    uint32_t w = std::max(1u, s.width), h = s.dim >= 3 ? std::max(1u, s.height) : 1, dep = s.dim == 4 ? std::max(1u, s.depth) : 1;
    uint64_t total = 0;
    for (uint32_t m = 0; m < 15 && (s.mips ? m < s.mips : true); m++) {
        total += (uint64_t)((w + block - 1) / block) * ((h + block - 1) / block) * dep * bpe;
        if (!s.mips && w == 1 && h == 1 && dep == 1) break;     /* 0 levels: the whole chain */
        w = std::max(1u, w >> 1);
        h = std::max(1u, h >> 1);
        dep = std::max(1u, dep >> 1);
    }
    total *= (uint64_t)std::max(1u, s.array) * std::max(1u, s.samples);
    return total <= most ? total : 0;
}

/* the resource's memory leaves the budget with it */
void res_drop(D11 &d, D11Res &r)
{
    d.res_bytes -= std::min(d.res_bytes, r.bytes);
    r.bytes = 0;
    r.release();
}

HRESULT res_create(Exec &x, D11 &d, D11Res &r)
{
    const d3dpt_d11_res &s = r.d;
    const uint64_t size = res_size(s);
    if (!size || d.res_bytes + size > D11_RES_BUDGET || (d.res.size() >= D11_MAX_RES && !d.res.count(s.res))) {
        if (d.warn(0x12000000u | (size ? 1u : 0u)))
            x.log("d11: resource dim %u fmt %u %ux%ux%u mips %u array %u samples %u refused: %s", s.dim, s.format, s.width, s.height,
                  s.depth, s.mips, s.array, s.samples,
                  !size ? "beyond Direct3D 11's limits" : d.res_bytes + size > D11_RES_BUDGET ? "the resources' memory is spent" : "too many resources");
        return size ? E_OUTOFMEMORY : E_INVALIDARG;
    }
    r.staging = s.usage == 3;
    D3D11_USAGE usage = r.staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    UINT cpu = r.staging ? D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE : 0;
    UINT bind = r.staging ? 0 : s.bind;
    UINT misc = s.misc & ~(UINT)D3D11_RESOURCE_MISC_SHARED;
    if (r.staging) misc &= ~(UINT)(D3D11_RESOURCE_MISC_GENERATE_MIPS | D3D11_RESOURCE_MISC_TEXTURECUBE);
    HRESULT hr = E_INVALIDARG;
    switch (s.dim) {
    case 1: {
        D3D11_BUFFER_DESC b = { s.width, usage, bind, cpu, misc, s.stride };
        ID3D11Buffer *p = nullptr;
        hr = d.dev->CreateBuffer(&b, nullptr, &p);
        r.res = p;
        break;
    }
    case 2: {
        D3D11_TEXTURE1D_DESC t = { s.width, s.mips, s.array, (DXGI_FORMAT)s.format, usage, bind, cpu, misc };
        ID3D11Texture1D *p = nullptr;
        hr = d.dev->CreateTexture1D(&t, nullptr, &p);
        r.res = p;
        break;
    }
    case 3: {
        D3D11_TEXTURE2D_DESC t = { s.width, s.height, s.mips, s.array, (DXGI_FORMAT)s.format, { s.samples ? s.samples : 1, s.quality },
                                   usage, bind, cpu, misc };
        ID3D11Texture2D *p = nullptr;
        hr = d.dev->CreateTexture2D(&t, nullptr, &p);
        r.res = p;
        break;
    }
    case 4: {
        D3D11_TEXTURE3D_DESC t = { s.width, s.height, s.depth, s.mips, (DXGI_FORMAT)s.format, usage, bind, cpu, misc };
        ID3D11Texture3D *p = nullptr;
        hr = d.dev->CreateTexture3D(&t, nullptr, &p);
        r.res = p;
        break;
    }
    }
    if (FAILED(hr) && d.warn(0x10000000u | (s.format << 8) | s.dim))
        x.log("d11: resource dim %u fmt %u %ux%ux%u mips %u array %u usage %u bind 0x%x misc 0x%x: 0x%08x", s.dim, s.format, s.width,
              s.height, s.depth, s.mips, s.array, s.usage, s.bind, s.misc, (unsigned)hr);
    if (SUCCEEDED(hr)) {
        d.res_bytes += size;
        r.bytes += size;
    }
    return hr;
}

/* a subresource's size: texels and rows (block rows) per slice, slices */
void sub_dims(const D11Res &r, uint32_t sub, uint32_t &w, uint32_t &h, uint32_t &depth, uint32_t &rows, uint32_t &row_bytes)
{
    uint32_t mips = r.d.mips ? r.d.mips : 1, mip = r.d.dim == 1 ? 0 : sub % mips, block;
    uint32_t bpe = dxgi_bytes(r.d.format, &block);
    w = std::max(1u, r.d.width >> mip);
    h = r.d.dim >= 3 ? std::max(1u, r.d.height >> mip) : 1;
    depth = r.d.dim == 4 ? std::max(1u, r.d.depth >> mip) : 1;
    if (r.d.dim == 1) { rows = 1; row_bytes = r.d.width; return; }
    rows = (h + block - 1) / block;
    row_bytes = (w + block - 1) / block * bpe;
}

uint32_t sub_count(const D11Res &r)
{
    return r.d.dim == 1 ? 1 : (r.d.mips ? r.d.mips : 1) * (r.d.array ? r.d.array : 1);
}

/* A guest's box inside the subresource (whole blocks of a BC format, its
 * right / bottom edge allowed at the subresource's), or no box. Every
 * region the guest names is checked here before the host touches memory
 * or hands it to d3d11. */
bool box_ok(const D11Res &r, uint32_t sub, const uint32_t *b)
{
    if (sub >= sub_count(r)) return false;
    if (!b) return true;
    uint32_t w, h, depth, rows, row_bytes, block;
    sub_dims(r, sub, w, h, depth, rows, row_bytes);
    if (r.d.dim == 1) w = r.d.width;
    if (b[0] >= b[3] || b[1] >= b[4] || b[2] >= b[5] || b[3] > w || b[4] > h || b[5] > depth) return false;
    if (r.d.dim != 1 && dxgi_bytes(r.d.format, &block) && block > 1) {
        if (b[0] % block || b[1] % block || (b[3] % block && b[3] != w) || (b[4] % block && b[4] != h)) return false;
    }
    return true;
}

/* the bytes a write of the box (or the subresource) at these pitches
 * reads from its source; 0 when the pitches cannot hold a row / a slice */
uint64_t box_bytes(const D11Res &r, uint32_t sub, const uint32_t *b, uint32_t row_pitch, uint32_t depth_pitch)
{
    uint32_t w, h, depth, rows, row_bytes, block = 1;
    sub_dims(r, sub, w, h, depth, rows, row_bytes);
    uint32_t bpe = r.d.dim == 1 ? 1 : dxgi_bytes(r.d.format, &block);
    if (!bpe) return 0;
    uint32_t x0 = b ? b[0] : 0, y0 = b ? b[1] : 0, z0 = b ? b[2] : 0;
    uint32_t x1 = b ? b[3] : (r.d.dim == 1 ? r.d.width : w), y1 = b ? b[4] : h, z1 = b ? b[5] : depth;
    uint64_t rb = (uint64_t)(x1 - x0 + block - 1) / block * bpe, ny = (y1 - y0 + block - 1) / block, nz = z1 - z0;
    if (row_pitch < rb || (nz > 1 && (uint64_t)depth_pitch < (uint64_t)row_pitch * ny)) return 0;
    return (nz - 1) * depth_pitch + (ny - 1) * (uint64_t)row_pitch + rb;
}

/* the guest's bytes into the host's subresource: UpdateSubresource, or a
 * map for a staging one (which UpdateSubresource refuses) */
void write_sub(D11 &d, D11Res &r, uint32_t sub, const D3D11_BOX *box, const uint8_t *src, uint32_t row_pitch, uint32_t depth_pitch)
{
    if (!r.staging) {
        d.ctx->UpdateSubresource(r.res, sub, box, src, row_pitch, depth_pitch);
        return;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(d.ctx->Map(r.res, sub, D3D11_MAP_WRITE, 0, &m))) return;
    uint32_t w, h, depth, rows, row_bytes, block;
    sub_dims(r, sub, w, h, depth, rows, row_bytes);
    uint32_t bpe = r.d.dim == 1 ? 1 : dxgi_bytes(r.d.format, &block);
    if (r.d.dim == 1) block = 1;
    uint32_t x0 = box ? box->left : 0, y0 = box ? box->top : 0, z0 = box ? box->front : 0;
    uint32_t x1 = box ? box->right : (r.d.dim == 1 ? r.d.width : w), y1 = box ? box->bottom : h, z1 = box ? box->back : depth;
    uint32_t n = (x1 - x0 + block - 1) / block * bpe, ny = (y1 - y0 + block - 1) / block;
    for (uint32_t z = z0; z < z1; z++)
        for (uint32_t y = 0; y < ny; y++)
            memcpy((uint8_t *)m.pData + z * m.DepthPitch + (y0 / block + y) * m.RowPitch + x0 / block * bpe,
                   src + (z - z0) * depth_pitch + y * row_pitch, n);
    d.ctx->Unmap(r.res, sub);
}

/* the allocation's VRAM, by the Direct3D 9 side's registration */
uint8_t *vram_of(Exec &x, uint32_t h, uint32_t &off, uint32_t &bytes)
{
    if (!exec_ddi_vram(x, h, &off, &bytes) || !x.vram || (uint64_t)off + bytes > x.vram_size) return nullptr;
    return x.vram + off;
}

void owed_settle(Exec &x, D11 &d, uint32_t h, bool wait, bool drop = false);
void res_readback(Exec &x, D11 &d, uint32_t h, D11Res &r, uint32_t sub, bool scanout, bool ring);

/* the ring's slots no longer hold the allocation's latest (its VRAM is,
 * or it went) */
void scans_forget(D11 &d, uint32_t h)
{
    for (auto it = d.scans.begin(); it != d.scans.end();)
        it = it->second.h == h ? d.scans.erase(it) : std::next(it);
}

/* A flip's frame into the scanout ring's next slot, on the GPU, instead of
 * a readback (M26): a 2D BGRA8 texture of the ring's size, one sample,
 * whose first subresource is the page the flip shows. VRAM keeps its
 * older pixels and the resource stays `newer`, so whatever reads that
 * VRAM later reads it back first (exec_d11_sync, d3dpt_exec_scanout_vram).
 * False: not one for the ring, read back as before. */
bool ring_take(Exec &x, D11 &d, uint32_t h, D11Res &r)
{
    uint32_t off;
    /* the offset only: vram_of syncs, and the last frame of this buffer
     * would come back into VRAM (a 33 MB readback at 3840x2160 a flip) */
    if (d.ring.empty() || r.d.dim != 3 || r.d.width != d.ring_w || r.d.height != d.ring_h ||
        r.d.samples > 1 || (r.d.format != 87 && r.d.format != 91) || r.layout.empty() || !r.res ||
        !exec_ddi_vram_offset(x, h, &off))
        return false;
    /* the next slot the player does not hold (all held: read back), one
     * with another page's newest frame in it last */
    uint32_t page = off + r.layout[0].offset, latest = 0;
    for (const auto &kv : d.scans)
        if (kv.first != page && kv.second.slot < 32) latest |= 1u << kv.second.slot;
    int pick = exec_ring_pick(x, d.ring_next, (uint32_t)d.ring.size(), latest);
    if (pick < 0) return false;
    uint32_t k = (uint32_t)pick;
    D11::Slot &s = d.ring[k];
    d.ring_next = (k + 1) % (uint32_t)d.ring.size();
    owed_settle(x, d, h, false, true);          /* a readback owed for it is older */
    /* the slot's last frame is overwritten: one that was another page's
     * newest goes back into VRAM first (owed, the display waits for it) */
    std::vector<uint32_t> evicted;
    for (auto it = d.scans.begin(); it != d.scans.end();) {
        if (it->second.slot != k) { ++it; continue; }
        if (it->first != page) evicted.push_back(it->second.h);
        it = d.scans.erase(it);
    }
    for (uint32_t e : evicted) {
        auto er = d.res.find(e);
        if (er != d.res.end() && er->second.newer && er->second.res) res_readback(x, d, e, er->second, ~0u, true, false);
    }
    d.ctx->CopySubresourceRegion(s.tex, 0, 0, 0, 0, r.res, 0, nullptr);
    d.ctx->End(s.done);
    d.ctx->Flush();                             /* submitted now: the display polls it */
    d.scans[page] = { h, k };
    r.newer = true;
    return true;
}

/* VRAM -> host: the subresources the guest's CPU wrote */
void res_dirty(Exec &x, D11 &d, uint32_t h, D11Res &r, uint32_t sub)
{
    uint32_t off, bytes;
    uint8_t *mem = vram_of(x, h, off, bytes);
    if (!mem || r.layout.empty()) return;
    owed_settle(x, d, h, false, true);         /* the guest's CPU wrote it since: what was owed is older */
    scans_forget(d, h);                         /* and VRAM is the latest now, not a slot */
    uint32_t n = sub_count(r), first = sub == ~0u ? 0 : sub, last = sub == ~0u ? n : sub + 1;
    for (uint32_t i = first; i < last && i < r.layout.size(); i++) {
        const d3dpt_d11_sub &l = r.layout[i];
        uint32_t w, ht, depth, rows, row_bytes;
        sub_dims(r, i, w, ht, depth, rows, row_bytes);
        if ((uint64_t)l.offset + (uint64_t)l.depth_pitch * (depth - 1) + (uint64_t)l.row_pitch * (rows - 1) + row_bytes > bytes) continue;
        write_sub(d, r, i, nullptr, mem + l.offset, l.row_pitch, l.depth_pitch);
    }
    if (sub == ~0u || sub_count(r) == 1) r.newer = false;    /* VRAM and the host agree */
}

/* host -> VRAM, the second half: the staging copy's subresources mapped
 * and copied row by row into the layout; false (nothing marked written) if
 * the GPU is not done with them yet and `wait` is false */
bool stage_to_vram(Exec &x, D11 &d, uint32_t h, D11Res &r, uint32_t first, uint32_t last, bool wait)
{
    uint32_t off, bytes;
    uint8_t *mem = vram_of(x, h, off, bytes);
    ID3D11Resource *from = r.staging ? r.res : r.stage;
    if (!mem || !from) return true;
    for (uint32_t i = first; i < last && i < r.layout.size(); i++) {
        auto t = d.prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        D3D11_MAPPED_SUBRESOURCE m;
        HRESULT hr = d.ctx->Map(from, i, D3D11_MAP_READ, wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
        if (hr == DXGI_ERROR_WAS_STILL_DRAWING) return false;
        if (FAILED(hr)) continue;
        if (d.prof) d.prof_part(72, t);
        const d3dpt_d11_sub &l = r.layout[i];
        uint32_t w, ht, depth, rows, row_bytes;
        sub_dims(r, i, w, ht, depth, rows, row_bytes);
        uint32_t row = std::min(row_bytes, l.row_pitch);
        if ((uint64_t)l.offset + (uint64_t)l.depth_pitch * (depth - 1) + (uint64_t)l.row_pitch * (rows - 1) + row <= bytes) {
            for (uint32_t z = 0; z < depth; z++)
                for (uint32_t y = 0; y < rows; y++)
                    memcpy(mem + l.offset + z * l.depth_pitch + y * l.row_pitch,
                           (const uint8_t *)m.pData + z * m.DepthPitch + y * m.RowPitch, row);
        }
        d.ctx->Unmap(from, i);
        if (d.prof) d.prof_part(73, t);
    }
    if (x.ops.vram_dirty) x.ops.vram_dirty(x.ops.ud, off, bytes);
    exec_ddi_vram_written(x, h);
    return true;
}

/* What is owed to an allocation's VRAM (h 0: every one's) goes there:
 * once the GPU is done (wait false), or now. `drop`: forgotten instead
 * (the guest wrote that VRAM since, or a newer copy covers it). */
void owed_settle(Exec &x, D11 &d, uint32_t h, bool wait, bool drop)
{
    for (size_t i = 0; i < d.owed.size();) {
        D11::Owed o = d.owed[i];
        if (h && o.h != h) { i++; continue; }
        /* off the list first: the VRAM lookup syncs the allocation again
         * (exec_d11_sync), which must not find it (a stack overflow) */
        d.owed.erase(d.owed.begin() + (ptrdiff_t)i);
        auto it = d.res.find(o.h);
        if (drop || it == d.res.end() || !it->second.res || stage_to_vram(x, d, o.h, it->second, o.first, o.last, wait))
            continue;
        i = std::min(i, d.owed.size());     /* not done yet: back in its place */
        d.owed.insert(d.owed.begin() + (ptrdiff_t)i, o);
        i++;
    }
}

/* host -> VRAM: a staging copy of the subresource (a resolve first for a
 * multisampled one), mapped and copied row by row into the layout. A
 * flip's (`scanout`) is left owed when the display settles it
 * (d3dpt_exec_settle): the Map waits for the GPU to finish the frame,
 * 2-4 ms of the executor's at each of DWM's compositions with every guest
 * submission queued behind it (track M25) */
void res_readback(Exec &x, D11 &d, uint32_t h, D11Res &r, uint32_t sub, bool scanout = false, bool ring = true)
{
    uint32_t off, bytes;
    if (scanout && ring && (sub == 0 || sub == ~0u) && ring_take(x, d, h, r)) return;
    /* VRAM gets the latest now, not a ring slot: a slot that held an older
     * frame of this allocation must not be shown for it. A flip read back
     * because the player held every slot left that entry, and the page's
     * next show took the slot, DWM's frame from two or three flips before
     * (a Start menu's animation with echoes of itself, on the Mac, where
     * the player gives slots back later; the Direct3D 9 side's readback
     * forgets them too) */
    scans_forget(d, h);
    /* one still owed: this copy is newer where it covers it */
    for (const D11::Owed &o : d.owed)
        if (o.h == h) {
            owed_settle(x, d, h, true, sub == ~0u || (o.first == sub && o.last == sub + 1));
            break;
        }
    if (sub == ~0u || sub_count(r) == 1) r.newer = false;    /* first: the lookups below sync again otherwise */
    uint8_t *mem = vram_of(x, h, off, bytes);
    if (!mem || r.layout.empty()) return;
    if (!r.staging && !r.stage) {
        /* the staging twin: one sample, no binding */
        D11Res t;
        t.d = r.d;
        t.d.usage = 3;
        t.d.samples = 1;
        t.d.quality = 0;
        if (FAILED(res_create(x, d, t))) return;
        r.stage = t.res;
        r.bytes += t.bytes;
        t.res = nullptr;
    }
    uint32_t n = sub_count(r), first = sub == ~0u ? 0 : sub, last = sub == ~0u ? n : sub + 1;
    if (!r.staging) {
        for (uint32_t i = first; i < last && i < r.layout.size(); i++) {
            if (r.d.samples > 1 && r.d.dim == 3) {
                if (!r.resolve) {
                    D3D11_TEXTURE2D_DESC t = { r.d.width, r.d.height, 1, 1, (DXGI_FORMAT)r.d.format, { 1, 0 }, D3D11_USAGE_DEFAULT, 0, 0, 0 };
                    if (FAILED(d.dev->CreateTexture2D(&t, nullptr, &r.resolve))) return;
                }
                d.ctx->ResolveSubresource(r.resolve, 0, r.res, i, (DXGI_FORMAT)r.d.format);
                d.ctx->CopySubresourceRegion(r.stage, i, 0, 0, 0, r.resolve, 0, nullptr);
            } else {
                d.ctx->CopySubresourceRegion(r.stage, i, 0, 0, 0, r.res, i, nullptr);
            }
        }
        if (scanout && x.settles) {
            d.owed.push_back({ h, first, last });
            d.ctx->Flush();     /* submitted now: the display polls it without waiting */
            return;
        }
    }
    stage_to_vram(x, d, h, r, first, last, true);
}

/* --------------------------------------------------------------- tokens */

struct Run {
    Exec &x;
    D11 &d;
    D11Ctx &c;
    HRESULT hr = S_OK;
    std::vector<uint8_t> ret;                   /* a QUERY_GET's answer */
    bool get = false;
    void fail(HRESULT h) { if (SUCCEEDED(hr)) hr = h; }
};

template<class T> T *obj(Run &r, uint32_t id, uint32_t kind)
{
    if (!id) return nullptr;
    auto it = r.c.objs.find(id);
    if (it == r.c.objs.end() || it->second.kind != kind) { r.fail(E_INVALIDARG); return nullptr; }
    return static_cast<T *>(it->second.p);
}

void put_obj(Run &r, uint32_t id, uint32_t kind, IUnknown *p, HRESULT hr, uint32_t stage = 0)
{
    if (FAILED(hr) || !p) {
        r.fail(FAILED(hr) ? hr : E_FAIL);
        if (r.d.warn(0x20000000u | kind)) r.x.log("d11: an object of kind %u: 0x%08x", kind, (unsigned)hr);
        return;
    }
    auto it = r.c.objs.find(id);
    if (it == r.c.objs.end() && r.d.nobjs >= D11_MAX_OBJS) {
        p->Release();
        r.fail(E_OUTOFMEMORY);
        if (r.d.warn(0x13000000u)) r.x.log("d11: an object of kind %u refused: %zu objects already", kind, r.d.nobjs);
        return;
    }
    if (it == r.c.objs.end()) r.d.nobjs++;
    else if (it->second.p) it->second.p->Release();
    r.c.objs[id] = { kind, p, stage };
}

/* a resource the guest names where only a buffer may go (constant,
 * vertex, index, stream output): anything else is refused, never cast */
ID3D11Buffer *buf_ref(Run &r, uint32_t h)
{
    if (!h) return nullptr;
    D11Res *x = r.x.named(h) ? res_of(r.d, h) : nullptr;    /* v31: none outside the scope */
    if (!x || x->d.dim != 1) {
        r.fail(E_INVALIDARG);
        return nullptr;
    }
    return static_cast<ID3D11Buffer *>(x->res);
}

/* the host is about to write the resource: VRAM's copy goes stale */
void mark(Run &r, uint32_t h)
{
    D11Res *x = h ? res_of(r.d, h) : nullptr;
    if (x && !x->layout.empty()) x->newer = true;
}

void mark_targets(Run &r)
{
    for (uint32_t i = 0; i < r.c.nrtv; i++) {
        auto it = r.c.objs.find(r.c.rtv[i]);
        if (it != r.c.objs.end()) mark(r, it->second.res);
    }
    auto it = r.c.objs.find(r.c.dsv);
    if (r.c.dsv && it != r.c.objs.end()) mark(r, it->second.res);
}

ID3D11Resource *res_ref(Run &r, uint32_t h)
{
    if (!h) return nullptr;
    D11Res *x = r.x.named(h) ? res_of(r.d, h) : nullptr;    /* v31: none outside the scope */
    if (!x) {
        r.fail(E_INVALIDARG);
        if (r.d.warn(0x30000000u | (h & 0xfffffff))) r.x.log("d11: resource %u unknown", h);
        return nullptr;
    }
    return x->res;
}

void create_shader(Run &r, const d3dpt_d11_shader *s, uint32_t size)
{
    /* the API's limits first (32 signature registers a side, a few entries
     * per register; 64 stream output entries), then sizes in 64 bits */
    if (s->nin > 128 || s->nout > 128 || s->nso > 64) { r.fail(E_INVALIDARG); return; }
    uint64_t need = sizeof(*s) + (uint64_t)s->code_dwords * 4 + (uint64_t)s->nin * sizeof(d3dpt_d11_sig) +
                    (uint64_t)s->nout * sizeof(d3dpt_d11_sig) + (uint64_t)s->nso * sizeof(d3dpt_d11_so);
    if (need > size || s->code_dwords < 2 || s->stage >= D3DPT_D11S_COUNT) { r.fail(E_INVALIDARG); return; }
    const uint32_t *code = (const uint32_t *)(s + 1);
    if (code[1] != s->code_dwords) { r.fail(E_INVALIDARG); return; }    /* the program's own length (dword 1) */
    const d3dpt_d11_sig *in = (const d3dpt_d11_sig *)(code + s->code_dwords), *out = in + s->nin;
    const d3dpt_d11_so *so = (const d3dpt_d11_so *)(out + s->nout);
    uint32_t major = (code[0] >> 4) & 0xf;
    std::vector<uint8_t> shex((const uint8_t *)code, (const uint8_t *)(code + s->code_dwords));
    auto isgn = sig_entries(in, s->nin, s->stage == D3DPT_D11S_VS, false, true);
    auto osgn = sig_entries(out, s->nout, false, s->stage == D3DPT_D11S_PS, false);
    auto blob = dxbc({ { "ISGN", sig_chunk(isgn) }, { "OSGN", sig_chunk(osgn) }, { major >= 5 ? "SHEX" : "SHDR", shex } });
    HRESULT hr = E_INVALIDARG;
    IUnknown *p = nullptr;
    switch (s->stage) {
    case D3DPT_D11S_VS: { ID3D11VertexShader *v = nullptr; hr = r.d.dev->CreateVertexShader(blob.data(), blob.size(), nullptr, &v); p = v; break; }
    case D3DPT_D11S_PS: { ID3D11PixelShader *v = nullptr; hr = r.d.dev->CreatePixelShader(blob.data(), blob.size(), nullptr, &v); p = v; break; }
    case D3DPT_D11S_GS: {
        ID3D11GeometryShader *v = nullptr;
        if (!s->nso) {
            hr = r.d.dev->CreateGeometryShader(blob.data(), blob.size(), nullptr, &v);
        } else {
            /* stream output by the output signature's names: each entry's
             * register and mask to the entry that holds them */
            std::vector<D3D11_SO_DECLARATION_ENTRY> e;
            for (uint32_t i = 0; i < s->nso; i++) {
                D3D11_SO_DECLARATION_ENTRY d = {};
                uint32_t mask = so[i].mask & 0xf, first = 0, count = 0;
                while (first < 4 && !(mask & (1u << first))) first++;
                for (uint32_t k = 0; k < 4; k++) count += (mask >> k) & 1;
                d.Stream = so[i].stream;
                d.OutputSlot = (BYTE)so[i].slot;
                d.ComponentCount = (BYTE)count;
                if (so[i].reg != ~0u) {
                    for (auto &o : osgn) {
                        if (o.reg == so[i].reg && (o.mask & mask) == mask) {
                            uint32_t base = 0;
                            while (base < 4 && !(o.mask & (1u << base))) base++;
                            d.SemanticName = o.name.c_str();
                            d.SemanticIndex = o.index;
                            d.StartComponent = (BYTE)(first - base);
                            break;
                        }
                    }
                }
                e.push_back(d);
            }
            hr = r.d.dev->CreateGeometryShaderWithStreamOutput(blob.data(), blob.size(), e.data(), (UINT)e.size(), s->so_strides, 4,
                                                               s->so_rast, nullptr, &v);
        }
        p = v;
        break;
    }
    default:
        if (r.d.warn(0x40000000u | s->stage)) r.x.log("d11: a shader of stage %u", s->stage);
        r.fail(E_NOTIMPL);
        return;
    }
    if (FAILED(hr) && r.d.warn(0x41000000u | s->stage))
        r.x.log("d11: a stage %u shader (%u dwords, %u in, %u out): 0x%08x", s->stage, s->code_dwords, s->nin, s->nout, (unsigned)hr);
    put_obj(r, s->id, D3DPT_D11K_SHADER, p, hr, s->stage);
}

void create_layout(Run &r, const d3dpt_d11_list *l, uint32_t size)
{
    if (sizeof(*l) + (uint64_t)l->n * sizeof(d3dpt_d11_element) > size || l->n > 32) { r.fail(E_INVALIDARG); return; }
    const d3dpt_d11_element *e = (const d3dpt_d11_element *)(l + 1);
    std::vector<D3D11_INPUT_ELEMENT_DESC> el;
    std::vector<d3dpt_d11_sig> regs;
    for (uint32_t i = 0; i < l->n; i++) {
        if (e[i].slot >= 32 || e[i].reg >= 32) { r.fail(E_INVALIDARG); return; }
        D3D11_INPUT_ELEMENT_DESC d = { "D3DPT", e[i].reg, (DXGI_FORMAT)e[i].format, e[i].slot, e[i].offset,
                                       e[i].per_instance ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA, e[i].step };
        el.push_back(d);
        bool seen = false;
        for (auto &g : regs) seen |= g.reg == e[i].reg;
        if (!seen) regs.push_back({ 0, e[i].reg, 0xf, 0 });
    }
    /* an input signature of the same names (the vertex shader's are D3DPT<register>) */
    auto blob = dxbc({ { "ISGN", sig_chunk(sig_entries(regs.data(), (uint32_t)regs.size(), true, false, true)) } });
    ID3D11InputLayout *p = nullptr;
    HRESULT hr = r.d.dev->CreateInputLayout(el.data(), (UINT)el.size(), blob.data(), blob.size(), &p);
    put_obj(r, l->id, D3DPT_D11K_LAYOUT, p, hr);
}

void create_view(Run &r, uint32_t op, const d3dpt_d11_view *v)
{
    ID3D11Resource *res = res_ref(r, v->res);
    if (!res) return;
    HRESULT hr;
    if (op == D3DPT_D11_SRV_CREATE) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d = {};
        d.Format = (DXGI_FORMAT)v->format;
        d.ViewDimension = (D3D11_SRV_DIMENSION)v->dim;
        memcpy(&d.Buffer, v->u, sizeof(v->u));
        if (v->dim == D3D11_SRV_DIMENSION_BUFFEREX) d.BufferEx.Flags = v->u[2];
        ID3D11ShaderResourceView *p = nullptr;
        hr = r.d.dev->CreateShaderResourceView(res, &d, &p);
        put_obj(r, v->id, D3DPT_D11K_SRV, p, hr);
    } else if (op == D3DPT_D11_RTV_CREATE) {
        D3D11_RENDER_TARGET_VIEW_DESC d = {};
        d.Format = (DXGI_FORMAT)v->format;
        d.ViewDimension = (D3D11_RTV_DIMENSION)v->dim;
        memcpy(&d.Buffer, v->u, std::min(sizeof(v->u), sizeof(d) - offsetof(D3D11_RENDER_TARGET_VIEW_DESC, Buffer)));
        ID3D11RenderTargetView *p = nullptr;
        hr = r.d.dev->CreateRenderTargetView(res, &d, &p);
        put_obj(r, v->id, D3DPT_D11K_RTV, p, hr);
    } else {
        D3D11_DEPTH_STENCIL_VIEW_DESC d = {};
        d.Format = (DXGI_FORMAT)v->format;
        d.ViewDimension = (D3D11_DSV_DIMENSION)v->dim;
        d.Flags = v->flags;
        memcpy(&d.Texture1D, v->u, std::min(sizeof(v->u), sizeof(d) - offsetof(D3D11_DEPTH_STENCIL_VIEW_DESC, Texture1D)));
        ID3D11DepthStencilView *p = nullptr;
        hr = r.d.dev->CreateDepthStencilView(res, &d, &p);
        put_obj(r, v->id, D3DPT_D11K_DSV, p, hr);
    }
    if (FAILED(hr) && r.d.warn(0x50000000u | (op << 16) | v->dim))
        r.x.log("d11: view op %u dim %u fmt %u (%u %u %u %u): 0x%08x", op, v->dim, v->format, v->u[0], v->u[1], v->u[2], v->u[3], (unsigned)hr);
    auto it = r.c.objs.find(v->id);
    if (SUCCEEDED(hr) && it != r.c.objs.end()) it->second.res = v->res;
    /* a view made again under its id (a flip-model swap chain's buffers
     * rotated their identities): bound as a target, it is bound again */
    if (SUCCEEDED(hr) && op != D3DPT_D11_SRV_CREATE) {
        bool bound = op == D3DPT_D11_DSV_CREATE && r.c.dsv == v->id;
        for (uint32_t i = 0; i < r.c.nrtv && op == D3DPT_D11_RTV_CREATE; i++) bound = bound || r.c.rtv[i] == v->id;
        if (bound) {
            ID3D11RenderTargetView *t[8];
            for (uint32_t i = 0; i < r.c.nrtv; i++) t[i] = obj<ID3D11RenderTargetView>(r, r.c.rtv[i], D3DPT_D11K_RTV);
            r.d.ctx->OMSetRenderTargets(r.c.nrtv, t, obj<ID3D11DepthStencilView>(r, r.c.dsv, D3DPT_D11K_DSV));
        }
    }
}

/* a DXGI present's copy: the same size a copy on the host; into a
 * Direct3D 9 surface (DWM's) through VRAM */
void blt(Run &r, const d3dpt_d11_blt *b)
{
    if (!r.x.named(b->src) || !r.x.named(b->dst)) { r.fail(E_INVALIDARG); return; }
    D11Res *src = res_of(r.d, b->src), *dst = res_of(r.d, b->dst);
    if (!src || b->src_sub >= sub_count(*src) || b->right <= b->left || b->bottom <= b->top) { r.fail(E_INVALIDARG); return; }
    uint32_t w = b->right - b->left, h = b->bottom - b->top;
    if (dst) {
        const uint32_t box[6] = { b->left, b->top, 0, b->right, b->bottom, 1 };
        if (!box_ok(*dst, b->dst_sub, box)) { r.fail(E_INVALIDARG); return; }
        uint32_t sw, sh, sd, rows, rb;
        sub_dims(*src, b->src_sub, sw, sh, sd, rows, rb);
        if (sw == w && sh == h) {
            mark(r, b->dst);
            r.d.ctx->CopySubresourceRegion(dst->res, b->dst_sub, b->left, b->top, 0, src->res, b->src_sub, nullptr);
        } else if (r.d.warn(0x60000000u)) {
            r.x.log("d11: a scaled Blt (%ux%u into %ux%u) is not done yet", sw, sh, w, h);
        }
        return;
    }
    if (r.d.warn(0x61000000u)) r.x.log("d11: a Blt into resource %u, which only Direct3D 9 knows: not done yet", b->dst);
}

/* a predicate's answer, waited for (it ended before it was set, the API's
 * rule); false (nothing skipped) if the host never answers */
bool pred_holds(Run &r, ID3D11Predicate *pr)
{
    auto t0 = std::chrono::steady_clock::now();
    BOOL v = FALSE;
    for (;;) {
        HRESULT hr = r.d.ctx->GetData(pr, &v, sizeof(v), 0);
        if (hr == S_OK) return v != FALSE;
        if (FAILED(hr) || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(2)) {
            if (r.d.warn(0x62000000u)) r.x.log("d11: a predicate unanswered (0x%08x): what it guards is done", (unsigned)hr);
            return false;
        }
        std::this_thread::yield();
    }
}

/* what predication skips (the API's list: draws, clears, copies, resolves,
 * GenerateMips). Not RES_UPDATE: the guest's UpdateSubresource and its
 * dynamic resources' maps (which predication never skips) are the same
 * record, and the map's bytes must land */
bool predicated(uint32_t op)
{
    switch (op) {
    case D3DPT_D11_DRAW: case D3DPT_D11_DRAW_INDEXED: case D3DPT_D11_DRAW_INSTANCED: case D3DPT_D11_DRAW_INDEXED_INSTANCED:
    case D3DPT_D11_DRAW_AUTO: case D3DPT_D11_CLEAR_RTV: case D3DPT_D11_CLEAR_DSV: case D3DPT_D11_CLEAR_VIEW:
    case D3DPT_D11_COPY: case D3DPT_D11_COPY_REGION: case D3DPT_D11_RESOLVE: case D3DPT_D11_GEN_MIPS:
        return true;
    default:
        return false;
    }
}

void run_tok(Run &r, const d3dpt_d11_tok *t)
{
    if (r.c.pred_skip && predicated(t->op)) return;
    const uint8_t *body = (const uint8_t *)(t + 1);
    uint32_t size = t->size - (uint32_t)sizeof(*t);
    D11 &d = r.d;
    ID3D11DeviceContext1 *ctx = d.ctx;
#define NEED(T) if (size < sizeof(T)) { r.fail(E_INVALIDARG); return; } const T *a = (const T *)body
#define NEED_N(n) if (size < (n)) { r.fail(E_INVALIDARG); return; }
    switch (t->op) {
    case D3DPT_D11_RES_CREATE: {
        NEED(d3dpt_d11_res);
        if (sizeof(*a) + (uint64_t)a->nsub * sizeof(d3dpt_d11_sub) > size) { r.fail(E_INVALIDARG); return; }
        /* v31: never over another process's resource */
        if (!r.x.named(a->res)) { r.fail(E_INVALIDARG); return; }
        auto it = d.res.find(a->res);
        if (a->opened && it != d.res.end() && it->second.res) {
            /* another process opened it: the host has it */
            if (it->second.layout.empty() && a->nsub) it->second.layout.assign((const d3dpt_d11_sub *)(a + 1), (const d3dpt_d11_sub *)(a + 1) + a->nsub);
            return;
        }
        /* a creator's: whatever had the handle before is gone (the kernel
         * driver numbers allocations from 1 again when its device restarts,
         * a driver update with no machine reset, which leaves the host's
         * old objects under the same handles) */
        D11Res x;
        x.d = *a;
        x.layout.assign((const d3dpt_d11_sub *)(a + 1), (const d3dpt_d11_sub *)(a + 1) + a->nsub);
        if (FAILED(res_create(r.x, d, x))) { r.fail(E_INVALIDARG); return; }
        owed_settle(r.x, d, a->res, false, true);  /* the old one's */
        D11Res &nr = d.res[a->res];
        res_drop(d, nr);
        nr = x;
        /* a surface another API drew first (a Direct3D 9 one a process
         * opened here): its pixels come from VRAM */
        if (a->opened && exec_ddi_vram_sync(r.x, a->res)) res_dirty(r.x, d, a->res, nr, ~0u);
        return;
    }
    case D3DPT_D11_RES_UPDATE: {
        NEED(d3dpt_d11_update);
        D11Res *x = r.x.named(a->res) ? res_of(d, a->res) : nullptr;
        if (!x || sizeof(*a) + (uint64_t)a->bytes > size || !box_ok(*x, a->sub, a->has_box ? a->box : nullptr)) { r.fail(E_INVALIDARG); return; }
        uint64_t need = box_bytes(*x, a->sub, a->has_box ? a->box : nullptr, a->row_pitch, a->depth_pitch);
        if (!need || need > a->bytes) { r.fail(E_INVALIDARG); return; }
        D3D11_BOX box = { a->box[0], a->box[1], a->box[2], a->box[3], a->box[4], a->box[5] };
        mark(r, a->res);
        write_sub(d, *x, a->sub, a->has_box ? &box : nullptr, (const uint8_t *)(a + 1), a->row_pitch, a->depth_pitch);
        return;
    }
    case D3DPT_D11_RES_DIRTY: case D3DPT_D11_RES_READBACK: case D3DPT_D11_RES_SCANOUT: {
        NEED(d3dpt_d11_ref);
        D11Res *x = r.x.named(a->a) ? res_of(d, a->a) : nullptr;
        if (!x) { r.fail(E_INVALIDARG); return; }
        if (t->op == D3DPT_D11_RES_DIRTY) res_dirty(r.x, d, a->a, *x, a->b);
        else res_readback(r.x, d, a->a, *x, a->b, t->op == D3DPT_D11_RES_SCANOUT);
        return;
    }
    case D3DPT_D11_COPY: {
        NEED(d3dpt_d11_ref);
        ID3D11Resource *dst = res_ref(r, a->a), *src = res_ref(r, a->b);
        if (dst && src) { mark(r, a->a); ctx->CopyResource(dst, src); }
        return;
    }
    case D3DPT_D11_COPY_REGION: {
        NEED(d3dpt_d11_copy);
        ID3D11Resource *dst = res_ref(r, a->dst), *src = res_ref(r, a->src);
        D3D11_BOX box = { a->box[0], a->box[1], a->box[2], a->box[3], a->box[4], a->box[5] };
        if (dst && src) {
            /* the source region inside the source, and where it lands inside the destination */
            D11Res *sr = res_of(d, a->src), *dr = res_of(d, a->dst);
            uint32_t sw, sh, sd, rows, rb, b[6];
            if (!box_ok(*sr, a->src_sub, a->has_box ? a->box : nullptr) || a->dst_sub >= sub_count(*dr)) { r.fail(E_INVALIDARG); return; }
            sub_dims(*sr, a->src_sub, sw, sh, sd, rows, rb);
            if (sr->d.dim == 1) sw = sr->d.width;
            uint32_t ew = a->has_box ? a->box[3] - a->box[0] : sw, eh = a->has_box ? a->box[4] - a->box[1] : sh,
                     ed = a->has_box ? a->box[5] - a->box[2] : sd;
            b[0] = a->x; b[1] = a->y; b[2] = a->z;
            b[3] = a->x + ew; b[4] = a->y + eh; b[5] = a->z + ed;
            if (b[3] < b[0] || b[4] < b[1] || b[5] < b[2] || !box_ok(*dr, a->dst_sub, b)) { r.fail(E_INVALIDARG); return; }
            mark(r, a->dst);
        }
        if (dst && src) ctx->CopySubresourceRegion(dst, a->dst_sub, a->x, a->y, a->z, src, a->src_sub, a->has_box ? &box : nullptr);
        return;
    }
    case D3DPT_D11_RESOLVE: {
        NEED(d3dpt_d11_resolve);
        ID3D11Resource *dst = res_ref(r, a->dst), *src = res_ref(r, a->src);
        if (dst && src && (a->src_sub >= sub_count(*res_of(d, a->src)) || a->dst_sub >= sub_count(*res_of(d, a->dst)))) {
            r.fail(E_INVALIDARG);
            return;
        }
        if (dst && src) mark(r, a->dst);
        if (dst && src) ctx->ResolveSubresource(dst, a->dst_sub, src, a->src_sub, (DXGI_FORMAT)a->format);
        return;
    }
    case D3DPT_D11_GEN_MIPS: {
        NEED(uint32_t);
        auto *v = obj<ID3D11ShaderResourceView>(r, *a, D3DPT_D11K_SRV);
        if (v) { mark(r, r.c.objs[*a].res); ctx->GenerateMips(v); }
        return;
    }
    case D3DPT_D11_BLT: { NEED(d3dpt_d11_blt); blt(r, a); return; }
    case D3DPT_D11_SRV_CREATE: case D3DPT_D11_RTV_CREATE: case D3DPT_D11_DSV_CREATE: {
        NEED(d3dpt_d11_view);
        create_view(r, t->op, a);
        return;
    }
    case D3DPT_D11_DESTROY: {
        NEED(d3dpt_d11_ref);
        auto it = r.c.objs.find(a->b);
        if (it != r.c.objs.end() && it->second.kind == a->a) {
            if (it->second.p) it->second.p->Release();
            r.c.objs.erase(it);
            r.d.nobjs--;
        }
        return;
    }
    case D3DPT_D11_BLEND_CREATE: {
        NEED(d3dpt_d11_blend);
        D3D11_BLEND_DESC b = {};
        b.AlphaToCoverageEnable = a->alpha_to_coverage;
        b.IndependentBlendEnable = a->independent;
        for (int i = 0; i < 8; i++) {
            auto &o = b.RenderTarget[i];
            o.BlendEnable = a->rt[i].enable;
            o.SrcBlend = (D3D11_BLEND)a->rt[i].src;
            o.DestBlend = (D3D11_BLEND)a->rt[i].dst;
            o.BlendOp = (D3D11_BLEND_OP)a->rt[i].op;
            o.SrcBlendAlpha = (D3D11_BLEND)a->rt[i].src_a;
            o.DestBlendAlpha = (D3D11_BLEND)a->rt[i].dst_a;
            o.BlendOpAlpha = (D3D11_BLEND_OP)a->rt[i].op_a;
            o.RenderTargetWriteMask = (UINT8)a->rt[i].mask;
        }
        ID3D11BlendState *p = nullptr;
        HRESULT hr = d.dev->CreateBlendState(&b, &p);
        put_obj(r, a->id, D3DPT_D11K_BLEND, p, hr);
        return;
    }
    case D3DPT_D11_DEPTH_CREATE: {
        NEED(d3dpt_d11_depth);
        D3D11_DEPTH_STENCIL_DESC s = {};
        s.DepthEnable = a->depth_enable;
        s.DepthWriteMask = (D3D11_DEPTH_WRITE_MASK)a->write_mask;
        s.DepthFunc = (D3D11_COMPARISON_FUNC)a->func;
        s.StencilEnable = a->stencil_enable;
        s.StencilReadMask = (UINT8)a->read_mask;
        s.StencilWriteMask = (UINT8)a->write_mask8;
        s.FrontFace = { (D3D11_STENCIL_OP)a->front[0], (D3D11_STENCIL_OP)a->front[1], (D3D11_STENCIL_OP)a->front[2], (D3D11_COMPARISON_FUNC)a->front[3] };
        s.BackFace = { (D3D11_STENCIL_OP)a->back[0], (D3D11_STENCIL_OP)a->back[1], (D3D11_STENCIL_OP)a->back[2], (D3D11_COMPARISON_FUNC)a->back[3] };
        ID3D11DepthStencilState *p = nullptr;
        HRESULT hr = d.dev->CreateDepthStencilState(&s, &p);
        put_obj(r, a->id, D3DPT_D11K_DEPTH, p, hr);
        return;
    }
    case D3DPT_D11_RASTER_CREATE: {
        NEED(d3dpt_d11_raster);
        D3D11_RASTERIZER_DESC s = { (D3D11_FILL_MODE)a->fill, (D3D11_CULL_MODE)a->cull, (BOOL)a->front_ccw, a->bias, a->bias_clamp,
                                    a->slope_bias, (BOOL)a->depth_clip, (BOOL)a->scissor, (BOOL)a->multisample, (BOOL)a->aa_lines };
        ID3D11RasterizerState *p = nullptr;
        HRESULT hr = d.dev->CreateRasterizerState(&s, &p);
        put_obj(r, a->id, D3DPT_D11K_RASTER, p, hr);
        return;
    }
    case D3DPT_D11_SAMPLER_CREATE: {
        NEED(d3dpt_d11_sampler);
        D3D11_SAMPLER_DESC s = { (D3D11_FILTER)a->filter, (D3D11_TEXTURE_ADDRESS_MODE)a->address[0], (D3D11_TEXTURE_ADDRESS_MODE)a->address[1],
                                 (D3D11_TEXTURE_ADDRESS_MODE)a->address[2], a->lod_bias, a->max_aniso, (D3D11_COMPARISON_FUNC)a->func,
                                 { a->border[0], a->border[1], a->border[2], a->border[3] }, a->min_lod, a->max_lod };
        ID3D11SamplerState *p = nullptr;
        HRESULT hr = d.dev->CreateSamplerState(&s, &p);
        put_obj(r, a->id, D3DPT_D11K_SAMPLER, p, hr);
        return;
    }
    case D3DPT_D11_SHADER_CREATE: { NEED(d3dpt_d11_shader); create_shader(r, a, size); return; }
    case D3DPT_D11_LAYOUT_CREATE: { NEED(d3dpt_d11_list); create_layout(r, a, size); return; }
    case D3DPT_D11_QUERY_CREATE: {
        NEED(d3dpt_d11_query);
        D3D11_QUERY_DESC q = { (D3D11_QUERY)a->type, a->misc };
        bool pred = a->type == D3D11_QUERY_OCCLUSION_PREDICATE || a->type == D3D11_QUERY_SO_OVERFLOW_PREDICATE ||
                    (a->type >= D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0 && a->type <= D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3);
        HRESULT hr;
        IUnknown *p = nullptr;
        if (pred) { ID3D11Predicate *x = nullptr; hr = d.dev->CreatePredicate(&q, &x); p = x; }
        else { ID3D11Query *x = nullptr; hr = d.dev->CreateQuery(&q, &x); p = x; }
        /* stage: 1 for a predicate, 2 more for one that is only a hint (D3D11_QUERY_MISC_PREDICATEHINT) */
        put_obj(r, a->id, D3DPT_D11K_QUERY, p, hr, pred ? 1u | (a->misc & D3D11_QUERY_MISC_PREDICATEHINT ? 2u : 0u) : 0u);
        return;
    }
    case D3DPT_D11_SET_SHADER: {
        NEED(d3dpt_d11_ref);
        IUnknown *s = a->b ? obj<IUnknown>(r, a->b, D3DPT_D11K_SHADER) : nullptr;
        /* a shader of that stage only: another stage's is another type */
        if (s && r.c.objs[a->b].stage != a->a) { r.fail(E_INVALIDARG); return; }
        switch (a->a) {
        case D3DPT_D11S_VS: ctx->VSSetShader((ID3D11VertexShader *)s, nullptr, 0); break;
        case D3DPT_D11S_PS: ctx->PSSetShader((ID3D11PixelShader *)s, nullptr, 0); break;
        case D3DPT_D11S_GS: ctx->GSSetShader((ID3D11GeometryShader *)s, nullptr, 0); break;
        case D3DPT_D11S_HS: ctx->HSSetShader((ID3D11HullShader *)s, nullptr, 0); break;
        case D3DPT_D11S_DS: ctx->DSSetShader((ID3D11DomainShader *)s, nullptr, 0); break;
        case D3DPT_D11S_CS: ctx->CSSetShader((ID3D11ComputeShader *)s, nullptr, 0); break;
        }
        return;
    }
    case D3DPT_D11_SET_CB: case D3DPT_D11_SET_SRV: case D3DPT_D11_SET_SAMPLERS: {
        NEED(d3dpt_d11_bind);
        /* the API's slots: 14 constant buffers, 128 views, 16 samplers; no wraparound */
        const uint64_t slots = t->op == D3DPT_D11_SET_CB ? 14 : t->op == D3DPT_D11_SET_SRV ? 128 : 16;
        if (sizeof(*a) + (uint64_t)a->n * 4 > size || (uint64_t)a->start + a->n > slots) { r.fail(E_INVALIDARG); return; }
        const uint32_t *ids = (const uint32_t *)(a + 1);
        void *p[128];
        for (uint32_t i = 0; i < a->n; i++) {
            if (t->op == D3DPT_D11_SET_CB) p[i] = buf_ref(r, ids[i]);
            else if (t->op == D3DPT_D11_SET_SRV) p[i] = obj<ID3D11ShaderResourceView>(r, ids[i], D3DPT_D11K_SRV);
            else p[i] = obj<ID3D11SamplerState>(r, ids[i], D3DPT_D11K_SAMPLER);
        }
#define BIND(st, F) case st: { \
            if (t->op == D3DPT_D11_SET_CB) ctx->F##SetConstantBuffers(a->start, a->n, (ID3D11Buffer *const *)p); \
            else if (t->op == D3DPT_D11_SET_SRV) ctx->F##SetShaderResources(a->start, a->n, (ID3D11ShaderResourceView *const *)p); \
            else ctx->F##SetSamplers(a->start, a->n, (ID3D11SamplerState *const *)p); \
            break; }
        switch (a->stage) {
        BIND(D3DPT_D11S_VS, VS)
        BIND(D3DPT_D11S_PS, PS)
        BIND(D3DPT_D11S_GS, GS)
        BIND(D3DPT_D11S_HS, HS)
        BIND(D3DPT_D11S_DS, DS)
        BIND(D3DPT_D11S_CS, CS)
        }
#undef BIND
        return;
    }
    case D3DPT_D11_SET_CB1: {
        NEED(d3dpt_d11_bind);
        if (sizeof(*a) + (uint64_t)a->n * sizeof(d3dpt_d11_cb1) > size || (uint64_t)a->start + a->n > 14) { r.fail(E_INVALIDARG); return; }
        const d3dpt_d11_cb1 *e = (const d3dpt_d11_cb1 *)(a + 1);
        ID3D11Buffer *b[14];
        UINT first[14], num[14];
        for (uint32_t i = 0; i < a->n; i++) {
            b[i] = (ID3D11Buffer *)buf_ref(r, e[i].res);
            first[i] = e[i].first;
            num[i] = e[i].num ? e[i].num : 4096;     /* the whole buffer: the API's largest binding */
        }
#define BIND1(st, F) case st: ctx->F##SetConstantBuffers1(a->start, a->n, b, first, num); break;
        switch (a->stage) {
        BIND1(D3DPT_D11S_VS, VS)
        BIND1(D3DPT_D11S_PS, PS)
        BIND1(D3DPT_D11S_GS, GS)
        BIND1(D3DPT_D11S_HS, HS)
        BIND1(D3DPT_D11S_DS, DS)
        BIND1(D3DPT_D11S_CS, CS)
        }
#undef BIND1
        return;
    }
    case D3DPT_D11_SET_LAYOUT: {
        NEED(uint32_t);
        ctx->IASetInputLayout(obj<ID3D11InputLayout>(r, *a, D3DPT_D11K_LAYOUT));
        return;
    }
    case D3DPT_D11_SET_VB: {
        NEED(d3dpt_d11_bind);
        if ((uint64_t)a->start + a->n > 32 || sizeof(*a) + (uint64_t)a->n * sizeof(d3dpt_d11_vb) > size) { r.fail(E_INVALIDARG); return; }
        const d3dpt_d11_vb *v = (const d3dpt_d11_vb *)(a + 1);
        ID3D11Buffer *b[32];
        UINT strides[32], offs[32];
        for (uint32_t i = 0; i < a->n; i++) {
            b[i] = buf_ref(r, v[i].res);
            strides[i] = v[i].stride;
            offs[i] = v[i].offset;
        }
        ctx->IASetVertexBuffers(a->start, a->n, b, strides, offs);
        return;
    }
    case D3DPT_D11_SET_IB: {
        NEED(d3dpt_d11_vb);
        ctx->IASetIndexBuffer(buf_ref(r, a->res), (DXGI_FORMAT)a->stride, a->offset);
        return;
    }
    case D3DPT_D11_SET_TOPOLOGY: { NEED(uint32_t); ctx->IASetPrimitiveTopology((D3D11_PRIMITIVE_TOPOLOGY)*a); return; }
    case D3DPT_D11_SET_TARGETS: {
        NEED(d3dpt_d11_list);
        if (a->n > 8 || sizeof(*a) + (uint64_t)a->n * 4 > size) { r.fail(E_INVALIDARG); return; }
        const uint32_t *ids = (const uint32_t *)(a + 1);
        ID3D11RenderTargetView *v[8];
        for (uint32_t i = 0; i < a->n; i++) v[i] = obj<ID3D11RenderTargetView>(r, ids[i], D3DPT_D11K_RTV);
        memcpy(r.c.rtv, ids, a->n * 4);
        r.c.nrtv = a->n;
        r.c.dsv = a->id;
        ctx->OMSetRenderTargets(a->n, v, obj<ID3D11DepthStencilView>(r, a->id, D3DPT_D11K_DSV));
        return;
    }
    case D3DPT_D11_SET_BLEND: {
        NEED(d3dpt_d11_set_blend);
        ctx->OMSetBlendState(obj<ID3D11BlendState>(r, a->id, D3DPT_D11K_BLEND), a->factor, a->mask);
        return;
    }
    case D3DPT_D11_SET_DEPTH: {
        NEED(d3dpt_d11_ref);
        ctx->OMSetDepthStencilState(obj<ID3D11DepthStencilState>(r, a->a, D3DPT_D11K_DEPTH), a->b);
        return;
    }
    case D3DPT_D11_SET_RASTER: { NEED(uint32_t); ctx->RSSetState(obj<ID3D11RasterizerState>(r, *a, D3DPT_D11K_RASTER)); return; }
    case D3DPT_D11_SET_VIEWPORTS: {
        NEED(uint32_t);
        if (*a > 16 || 4 + (uint64_t)*a * 24 > size) { r.fail(E_INVALIDARG); return; }
        ctx->RSSetViewports(*a, (const D3D11_VIEWPORT *)(a + 1));
        return;
    }
    case D3DPT_D11_SET_SCISSORS: {
        NEED(uint32_t);
        if (*a > 16 || 4 + (uint64_t)*a * 16 > size) { r.fail(E_INVALIDARG); return; }
        ctx->RSSetScissorRects(*a, (const D3D11_RECT *)(a + 1));
        return;
    }
    case D3DPT_D11_SET_SO: {
        NEED(d3dpt_d11_bind);
        if (a->n > 4 || sizeof(*a) + (uint64_t)a->n * 8 > size) { r.fail(E_INVALIDARG); return; }
        const d3dpt_d11_ref *e = (const d3dpt_d11_ref *)(a + 1);
        ID3D11Buffer *b[4];
        UINT offs[4];
        for (uint32_t i = 0; i < a->n; i++) { b[i] = buf_ref(r, e[i].a); offs[i] = e[i].b; }
        ctx->SOSetTargets(a->n, b, offs);
        return;
    }
    case D3DPT_D11_SET_PREDICATION: {
        NEED(d3dpt_d11_ref);
        ID3D11Predicate *pr = a->a ? obj<ID3D11Predicate>(r, a->a, D3DPT_D11K_QUERY) : nullptr;
        uint32_t kind = pr ? r.c.objs[a->a].stage : 0;
        if (pr && !(kind & 1)) { r.fail(E_INVALIDARG); return; }    /* a plain query is no predicate */
        ctx->SetPredication(pr, a->b);
        /* DXVK's d3d11 keeps the predicate and does everything anyway (its
         * SetPredication is a stub), and only a hint may be ignored: the
         * predicate's answer is waited for here, and while it equals the
         * value the predicated operations are skipped (run_tok) */
        r.c.pred_skip = pr && !(kind & 2) && pred_holds(r, pr) == (a->b != 0);
        return;
    }
    case D3DPT_D11_DRAW: { NEED_N(8); const uint32_t *v = (const uint32_t *)body; mark_targets(r); ctx->Draw(v[0], v[1]); return; }
    case D3DPT_D11_DRAW_INDEXED: { NEED_N(12); const uint32_t *v = (const uint32_t *)body; mark_targets(r); ctx->DrawIndexed(v[0], v[1], (INT)v[2]); return; }
    case D3DPT_D11_DRAW_INSTANCED: { NEED_N(16); const uint32_t *v = (const uint32_t *)body; mark_targets(r); ctx->DrawInstanced(v[0], v[1], v[2], v[3]); return; }
    case D3DPT_D11_DRAW_INDEXED_INSTANCED: {
        NEED_N(20);
        const uint32_t *v = (const uint32_t *)body;
        mark_targets(r);
        ctx->DrawIndexedInstanced(v[0], v[1], v[2], (INT)v[3], v[4]);
        return;
    }
    case D3DPT_D11_DRAW_AUTO: mark_targets(r); ctx->DrawAuto(); return;
    case D3DPT_D11_CLEAR_RTV: {
        NEED_N(20);
        const uint32_t *v = (const uint32_t *)body;
        auto *rtv = obj<ID3D11RenderTargetView>(r, v[0], D3DPT_D11K_RTV);
        if (rtv) mark(r, r.c.objs[v[0]].res);
        if (rtv) ctx->ClearRenderTargetView(rtv, (const FLOAT *)(v + 1));
        return;
    }
    case D3DPT_D11_CLEAR_VIEW: {
        NEED(d3dpt_d11_clear_view);
        if (sizeof(*a) + (uint64_t)a->n * sizeof(D3D11_RECT) > size || a->kind != D3DPT_D11K_RTV) { r.fail(E_INVALIDARG); return; }
        auto *rtv = obj<ID3D11RenderTargetView>(r, a->id, D3DPT_D11K_RTV);
        if (rtv) mark(r, r.c.objs[a->id].res);
        if (rtv) ctx->ClearView(rtv, a->color, a->n ? (const D3D11_RECT *)(a + 1) : nullptr, a->n);
        return;
    }
    case D3DPT_D11_CLEAR_DSV: {
        NEED_N(16);
        const uint32_t *v = (const uint32_t *)body;
        auto *dsv = obj<ID3D11DepthStencilView>(r, v[0], D3DPT_D11K_DSV);
        float depth;
        memcpy(&depth, &v[2], 4);
        if (dsv) mark(r, r.c.objs[v[0]].res);
        if (dsv) ctx->ClearDepthStencilView(dsv, v[1], depth, (UINT8)v[3]);
        return;
    }
    case D3DPT_D11_QUERY_BEGIN: case D3DPT_D11_QUERY_END: {
        NEED(uint32_t);
        auto *q = obj<ID3D11Query>(r, *a, D3DPT_D11K_QUERY);
        if (q) { if (t->op == D3DPT_D11_QUERY_BEGIN) ctx->Begin(q); else ctx->End(q); }
        return;
    }
    case D3DPT_D11_QUERY_GET: {
        NEED(d3dpt_d11_ref);
        auto *q = obj<ID3D11Query>(r, a->a, D3DPT_D11K_QUERY);
        if (!q || a->b > 4000) { r.fail(E_INVALIDARG); return; }
        /* room for any query's answer whatever size the guest names (the
         * largest, D3D11_QUERY_DATA_PIPELINE_STATISTICS, is 88 bytes); the
         * guest gets the size it asked for */
        r.ret.assign(std::max<uint32_t>(a->b, 256), 0);
        /* asked of the host at the query's own size, which DXVK insists
         * on, and the guest gets the first bytes it named: a runtime at
         * feature level 10_x reads PIPELINE_STATISTICS as Direct3D 10's
         * 64 bytes, the first eight of Direct3D 11's 88 (Unigine Heaven's
         * loading screen waited on it forever, track M25) */
        UINT own = a->b ? q->GetDataSize() : 0;
        if (own > r.ret.size()) own = 0;
        HRESULT hr = ctx->GetData(q, own ? r.ret.data() : nullptr, own, 0);
        r.ret.resize(a->b);
        r.get = true;
        if (FAILED(hr)) {
            /* which query, and the size the guest named against the one
             * this host's Direct3D 11 takes (DXVK refuses any other) */
            D3D11_QUERY_DESC qd = {};
            q->GetDesc(&qd);
            if (d.warn(0x50000000u | ((uint32_t)qd.Query << 16) | (uint32_t)(hr & 0xffff)))
                r.x.log("d11: GetData of a query of type %u, %u bytes (the query's %u): 0x%08x",
                        (unsigned)qd.Query, a->b, q->GetDataSize(), (unsigned)hr);
        }
        if (hr != S_OK) r.fail(hr == S_FALSE ? S_FALSE : hr);
        return;
    }
    default:
        if (d.warn(0x70000000u | t->op)) r.x.log("d11: token %u unknown", t->op);
        r.fail(E_NOTIMPL);
        return;
    }
#undef NEED
#undef NEED_N
}

void prof_tok(Exec &x, D11 &d, uint32_t op, std::chrono::steady_clock::time_point t0)
{
    auto now = std::chrono::steady_clock::now();
    d.prof_ns[op & 127] += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count();
    d.prof_n[op & 127]++;
    auto span = std::chrono::duration_cast<std::chrono::milliseconds>(now - d.prof_t0).count();
    if (span < 5000) return;
    char line[1024];
    size_t n = (size_t)snprintf(line, sizeof line, "d11: prof %lld ms: token count us", (long long)span);
    uint64_t total = 0;
    for (uint32_t i = 0; i < 128; i++) {
        if (i != 72 && i != 73) total += d.prof_ns[i];      /* a readback's parts are in its token's */
        if (d.prof_ns[i] >= 1000000 && n < sizeof line - 40)
            n += (size_t)snprintf(line + n, sizeof line - n, " %u:%llu:%llu", i, (unsigned long long)d.prof_n[i],
                                  (unsigned long long)(d.prof_ns[i] / 1000));
    }
    x.log("%s; all %llu us", line, (unsigned long long)(total / 1000));
    memset(d.prof_ns, 0, sizeof d.prof_ns);
    memset(d.prof_n, 0, sizeof d.prof_n);
    d.prof_t0 = now;
}

/* ------------------------------------------------------------------ GDI */

/* One side of a GDI operation (v27): the host's object of it, or its VRAM
 * (mem); a bitmap's VRAM (vram) is known either way when it is valid */
struct GdiSide {
    D11Res *obj = nullptr;
    uint8_t *mem = nullptr, *vram = nullptr;
    uint32_t pitch = 0, w = 0, h = 0, handle = 0;
};

/* a redirection bitmap's object, made at its first use (GDI may draw into
 * it before DWM opens it; DWM's RES_CREATE then finds it here) */
D11Res *gdi_object(Exec &x, D11 &d, const d3dpt_gdi_side &s)
{
    D11Res r;
    r.d.res = s.handle;
    r.d.dim = 3;                                /* D3D11_RESOURCE_DIMENSION_TEXTURE2D */
    r.d.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    r.d.bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    r.d.width = s.width;
    r.d.height = s.height;
    r.d.depth = r.d.mips = r.d.array = r.d.samples = 1;
    if (FAILED(res_create(x, d, r))) return nullptr;
    D11Res &nr = d.res[s.handle];
    res_drop(d, nr);
    nr = r;
    return &nr;
}

void gdi_refresh(D11 &d, const GdiSide &s);

bool gdi_side(Exec &x, D11 &d, const d3dpt_gdi_side &s, GdiSide &o)
{
    /* 32 bits a pixel: D3DDDIFMT_A8R8G8B8, X8R8G8B8 */
    if ((s.format != 21 && s.format != 22) || !s.width || !s.height || s.width > 16384 || s.height > 16384) return false;
    o.w = s.width;
    o.h = s.height;
    o.handle = s.handle;
    const uint64_t rowb = (uint64_t)s.width * 4;    /* in 64 bits whatever the limit above */
    if (x.vram && s.pitch >= rowb && (uint64_t)s.offset + (uint64_t)s.pitch * (s.height - 1) + rowb <= x.vram_size) {
        o.vram = x.vram + s.offset;
        o.pitch = s.pitch;
    }
    if (s.handle && !(s.flags & D3DPT_GDI_VRAM)) {
        D11Res *r = res_of(d, s.handle);
        if (!r && (s.flags & D3DPT_GDI_TEXTURE)) r = gdi_object(x, d, s);
        if (r && r->res && r->d.dim == 3 && r->d.samples <= 1 && r->d.width >= s.width && r->d.height >= s.height &&
            (r->d.format == DXGI_FORMAT_B8G8R8A8_UNORM || r->d.format == DXGI_FORMAT_B8G8R8X8_UNORM ||
             r->d.format == DXGI_FORMAT_B8G8R8A8_TYPELESS)) {
            o.obj = r;
            /* a bitmap's first touch: what the CPU drew so far comes over
             * and the shadow starts, before the host writes anything */
            if (o.vram && (s.flags & D3DPT_GDI_TEXTURE) && r->gdi_shadow.empty()) gdi_refresh(d, o);
            return true;
        }
    }
    if (!o.vram) return false;
    if (s.handle && !(s.flags & D3DPT_GDI_VRAM)) {
        /* what the host drew into it goes to VRAM first */
        exec_d11_sync(x, s.handle);
        exec_ddi_vram_sync(x, s.handle);
    }
    o.mem = o.vram;
    return true;
}

/* a rectangle inside both sides (the source's at its corner) */
bool gdi_rect_ok(const d3dpt_gdi_rect &r, const GdiSide &dst, const GdiSide *src)
{
    if (r.l < 0 || r.t < 0 || r.r <= r.l || r.b <= r.t || (uint32_t)r.r > dst.w || (uint32_t)r.b > dst.h) return false;
    if (!src) return true;
    int64_t sr = (int64_t)r.sx + (r.r - r.l), sb = (int64_t)r.sy + (r.b - r.t);
    return r.sx >= 0 && r.sy >= 0 && sr <= src->w && sb <= src->h;
}

/* A bitmap's shadow: its VRAM as the host last saw it, rows of w pixels.
 * Its rectangle's VRAM now holds what the host knows (it wrote it there,
 * or uploaded it from there). */
void shadow_take(D11Res *r, const GdiSide &s, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    if (!r || !s.vram || r->gdi_shadow.empty() || r->gdi_w != s.w || r->gdi_h != s.h) return;
    for (uint32_t y = y0; y < y1; y++)
        memcpy(&r->gdi_shadow[((size_t)y * s.w + x0) * 4], s.vram + (uint64_t)y * s.pitch + (uint64_t)x0 * 4, (x1 - x0) * 4);
}

/* the host wrote a rectangle of a side's VRAM (mem): whoever keeps a copy
 * of it hears so */
void gdi_mem_written(Exec &x, D11 &d, const GdiSide &s, const d3dpt_gdi_rect &r)
{
    if (s.handle) {
        exec_ddi_vram_written(x, s.handle);
        shadow_take(res_of(d, s.handle), s, (uint32_t)r.l, (uint32_t)r.t, (uint32_t)r.r, (uint32_t)r.b);
    }
    if (x.ops.vram_dirty)
        x.ops.vram_dirty(x.ops.ud, (uint32_t)(s.mem - x.vram) + (uint32_t)r.t * s.pitch, (uint32_t)(r.b - r.t) * s.pitch);
}

void gdi_obj_written(D11Res *obj)
{
    if (!obj->layout.empty()) obj->newer = true;
}

/* REFRESH: the spans of the bitmap's VRAM the CPU changed since the shadow
 * was taken, uploaded row by row (rows with the same span together), so
 * what the host drew elsewhere in the object stays */
void gdi_refresh(D11 &d, const GdiSide &s)
{
    if (!s.obj || !s.vram) return;
    D11Res &r = *s.obj;
    const size_t row = (size_t)s.w * 4;
    bool all = r.gdi_shadow.empty() || r.gdi_w != s.w || r.gdi_h != s.h;
    if (all) {
        r.gdi_shadow.assign((size_t)s.w * s.h * 4, 0);
        r.gdi_w = s.w;
        r.gdi_h = s.h;
    }
    auto span = [&](uint32_t y, uint32_t &x0, uint32_t &x1) {
        const uint32_t *v = (const uint32_t *)(s.vram + (uint64_t)y * s.pitch), *sh = (const uint32_t *)&r.gdi_shadow[y * row];
        if (all) { x0 = 0; x1 = s.w; return true; }
        if (!memcmp(v, sh, row)) return false;
        x0 = 0;
        x1 = s.w;
        while (x0 < x1 && v[x0] == sh[x0]) x0++;
        while (x1 > x0 && v[x1 - 1] == sh[x1 - 1]) x1--;
        return true;
    };
    bool any = false;
    for (uint32_t y = 0, x0, x1; y < s.h;) {
        if (!span(y, x0, x1)) { y++; continue; }
        uint32_t y1 = y + 1, a, b;
        while (y1 < s.h && span(y1, a, b) && a == x0 && b == x1) y1++;
        D3D11_BOX box = { x0, y, 0, x1, y1, 1 };
        d.ctx->UpdateSubresource(r.res, 0, &box, s.vram + (uint64_t)y * s.pitch + (uint64_t)x0 * 4, s.pitch, 0);
        for (uint32_t k = y; k < y1; k++)
            memcpy(&r.gdi_shadow[k * row + (size_t)x0 * 4], s.vram + (uint64_t)k * s.pitch + (uint64_t)x0 * 4, (x1 - x0) * 4);
        any = true;
        y = y1;
    }
    if (any) gdi_obj_written(&r);
}

void gdi_fill(Exec &x, D11 &d, const d3dpt_gdi &g, const d3dpt_gdi_rect *rc, const GdiSide &dst)
{
    /* opaque: GDI leaves a fill's alpha 0, and DWM composes a redirection
     * bitmap with its alpha (a white fill came out black) */
    uint32_t c = g.color | 0xff000000u;
    if (dst.obj) {
        if (!dst.obj->gdi_rtv && FAILED(d.dev->CreateRenderTargetView(dst.obj->res, nullptr, &dst.obj->gdi_rtv))) return;
        const float col[4] = { ((c >> 16) & 255) / 255.0f, ((c >> 8) & 255) / 255.0f, (c & 255) / 255.0f, 1.0f };
        std::vector<D3D11_RECT> rects;
        for (uint32_t i = 0; i < g.n; i++)
            if (gdi_rect_ok(rc[i], dst, nullptr)) rects.push_back({ rc[i].l, rc[i].t, rc[i].r, rc[i].b });
        if (rects.empty()) return;
        d.ctx->ClearView(dst.obj->gdi_rtv, col, rects.data(), (UINT)rects.size());
        gdi_obj_written(dst.obj);
    }
    /* VRAM too: the bitmap's (and its shadow), so the CPU's view and the
     * next REFRESH agree with the object */
    if (!dst.vram) return;
    for (uint32_t i = 0; i < g.n; i++) {
        const d3dpt_gdi_rect &r = rc[i];
        if (!gdi_rect_ok(r, dst, nullptr)) continue;
        for (int32_t y = r.t; y < r.b; y++) {
            uint32_t *p = (uint32_t *)(dst.vram + (uint64_t)y * dst.pitch) + r.l;
            for (int32_t k = 0; k < r.r - r.l; k++) p[k] = c;
        }
        if (dst.obj) shadow_take(dst.obj, dst, (uint32_t)r.l, (uint32_t)r.t, (uint32_t)r.r, (uint32_t)r.b);
        else gdi_mem_written(x, d, dst, r);
    }
}

/* the role's texture (D11::gdi_tex), at least w x h: grown, never
 * shrunk, and kept for the next operation */
ID3D11Texture2D *gdi_tex(D11 &d, int role, uint32_t w, uint32_t h)
{
    D11::GdiTex &t = d.gdi_tex[role];
    if (t.tex && t.w >= w && t.h >= h) return t.tex;
    const uint32_t nw = std::max(t.w, (w + 63) & ~63u), nh = std::max(t.h, (h + 63) & ~63u);
    const bool stage = role != D11::GDI_TMP;
    D3D11_TEXTURE2D_DESC desc = { nw, nh, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, stage ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT, 0,
                                  stage ? (UINT)D3D11_CPU_ACCESS_READ : 0u, 0 };
    ID3D11Texture2D *n = nullptr;
    if (FAILED(d.dev->CreateTexture2D(&desc, nullptr, &n)) || !n) return nullptr;
    if (t.tex) t.tex->Release();
    t.tex = n;
    t.w = nw;
    t.h = nh;
    return n;
}

/* A rectangle of a side's pixels on the CPU (v29's operations, a copy's
 * readback): read from its object through the role's staging texture, or
 * from its VRAM */
struct GdiPx {
    std::vector<uint32_t> p;
    int32_t x0 = 0, y0 = 0, w = 0, h = 0;
    ID3D11Texture2D *stg = nullptr;             /* copied there, not read yet (gdi_read_end) */
    uint32_t &at(int32_t x, int32_t y) { return p[(size_t)(y - y0) * (size_t)w + (size_t)(x - x0)]; }
};

/* The read's first half: VRAM is read now, an object's rectangle copied
 * into staging. gdi_read_end reads that, so an operation copies all its
 * rectangles before it maps the first and waits for the GPU once. */
bool gdi_read(D11 &d, const GdiSide &s, int32_t x0, int32_t y0, int32_t x1, int32_t y1, GdiPx &o, int role)
{
    if (x0 < 0 || y0 < 0 || x1 <= x0 || y1 <= y0 || (uint32_t)x1 > s.w || (uint32_t)y1 > s.h) return false;
    o.x0 = x0;
    o.y0 = y0;
    o.w = x1 - x0;
    o.h = y1 - y0;
    o.p.assign((size_t)o.w * (size_t)o.h, 0);
    o.stg = nullptr;
    if (s.obj) {
        ID3D11Texture2D *stg = gdi_tex(d, role, (uint32_t)o.w, (uint32_t)o.h);
        if (!stg) return false;
        D3D11_BOX box = { (UINT)x0, (UINT)y0, 0, (UINT)x1, (UINT)y1, 1 };
        d.ctx->CopySubresourceRegion(stg, 0, 0, 0, 0, s.obj->res, 0, &box);
        o.stg = stg;
        return true;
    }
    if (!s.mem) return false;
    for (int32_t y = 0; y < o.h; y++)
        memcpy(&o.p[(size_t)y * o.w], s.mem + (uint64_t)(y0 + y) * s.pitch + (uint64_t)x0 * 4, (size_t)o.w * 4);
    return true;
}

bool gdi_read_end(D11 &d, GdiPx &o)
{
    if (!o.stg) return true;
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(d.ctx->Map(o.stg, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        for (int32_t y = 0; y < o.h; y++)
            memcpy(&o.p[(size_t)y * o.w], (const uint8_t *)m.pData + (uint64_t)y * m.RowPitch, (size_t)o.w * 4);
        d.ctx->Unmap(o.stg, 0);
    }
    o.stg = nullptr;
    return ok;
}

void gdi_copy(Exec &x, D11 &d, const d3dpt_gdi &g, const d3dpt_gdi_rect *rc, const GdiSide &src, const GdiSide &dst)
{
    /* a readback: the source under every rectangle in one staging copy,
     * one wait for the GPU (writing the destination's VRAM changes no
     * object, so the source reads the same before as between) */
    GdiPx back;
    if (src.obj && !dst.obj) {
        int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
        for (uint32_t i = 0; i < g.n; i++) {
            const d3dpt_gdi_rect &r = rc[i];
            if (!gdi_rect_ok(r, dst, &src)) continue;
            x0 = std::min(x0, r.sx);
            y0 = std::min(y0, r.sy);
            x1 = std::max(x1, r.sx + (r.r - r.l));
            y1 = std::max(y1, r.sy + (r.b - r.t));
        }
        if (x1 <= x0 || !gdi_read(d, src, x0, y0, x1, y1, back, D11::GDI_S) || !gdi_read_end(d, back)) return;
    }
    for (uint32_t i = 0; i < g.n; i++) {
        const d3dpt_gdi_rect &r = rc[i];
        if (!gdi_rect_ok(r, dst, &src)) continue;
        uint32_t w = (uint32_t)(r.r - r.l), h = (uint32_t)(r.b - r.t);
        D3D11_BOX box = { (UINT)r.sx, (UINT)r.sy, 0, (UINT)r.sx + w, (UINT)r.sy + h, 1 };
        if (src.obj && dst.obj) {
            /* object to object (a present, a bitmap to a bitmap): the
             * destination's VRAM keeps what it had, and the object keeps
             * this until the CPU draws over it */
            bool overlap = src.obj == dst.obj && r.sx < r.r && r.l < (int32_t)(r.sx + w) && r.sy < r.b && r.t < (int32_t)(r.sy + h);
            if (!overlap) {
                d.ctx->CopySubresourceRegion(dst.obj->res, 0, (UINT)r.l, (UINT)r.t, 0, src.obj->res, 0, &box);
            } else {
                /* within one bitmap, overlapping (a scroll): through a copy of the source */
                ID3D11Texture2D *tmp = gdi_tex(d, D11::GDI_TMP, w, h);
                if (!tmp) continue;
                const D3D11_BOX tb = { 0, 0, 0, w, h, 1 };
                d.ctx->CopySubresourceRegion(tmp, 0, 0, 0, 0, src.obj->res, 0, &box);
                d.ctx->CopySubresourceRegion(dst.obj->res, 0, (UINT)r.l, (UINT)r.t, 0, tmp, 0, &tb);
            }
            gdi_obj_written(dst.obj);
        } else if (dst.obj) {
            const uint8_t *from = src.mem + (uint64_t)r.sy * src.pitch + (uint64_t)r.sx * 4;
            D3D11_BOX db = { (UINT)r.l, (UINT)r.t, 0, (UINT)r.r, (UINT)r.b, 1 };
            d.ctx->UpdateSubresource(dst.obj->res, 0, &db, from, src.pitch, 0);
            gdi_obj_written(dst.obj);
            if (dst.vram) {
                /* and into the bitmap's VRAM and shadow (rows bottom up
                 * when they overlap downwards) */
                bool up = src.mem == dst.vram && r.t > r.sy;
                for (uint32_t k = 0; k < h; k++) {
                    uint32_t y = up ? h - 1 - k : k;
                    memmove(dst.vram + (uint64_t)(r.t + y) * dst.pitch + (uint64_t)r.l * 4, from + (uint64_t)y * src.pitch, w * 4);
                }
                shadow_take(dst.obj, dst, (uint32_t)r.l, (uint32_t)r.t, (uint32_t)r.r, (uint32_t)r.b);
            }
        } else if (src.obj) {
            /* a readback: from the copy read above */
            for (uint32_t y = 0; y < h; y++)
                memcpy(dst.mem + (uint64_t)(r.t + y) * dst.pitch + (uint64_t)r.l * 4, &back.at(r.sx, r.sy + (int32_t)y), w * 4);
            gdi_mem_written(x, d, dst, r);
        } else {
            /* VRAM to VRAM, bottom up when the rows overlap downwards */
            bool up = src.mem == dst.mem && r.t > r.sy;
            for (uint32_t k = 0; k < h; k++) {
                uint32_t y = up ? h - 1 - k : k;
                memmove(dst.mem + (uint64_t)(r.t + y) * dst.pitch + (uint64_t)r.l * 4,
                        src.mem + (uint64_t)(r.sy + y) * src.pitch + (uint64_t)r.sx * 4, w * 4);
            }
            gdi_mem_written(x, d, dst, r);
        }
    }
}

/* the rectangles of `px` back where they came from, as a copy's: into the
 * object, and its bitmap's VRAM and shadow; or into VRAM */
void gdi_write(Exec &x, D11 &d, const GdiSide &s, const GdiPx &px, const d3dpt_gdi_rect *rc, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        const d3dpt_gdi_rect &r = rc[i];
        if (r.l < px.x0 || r.t < px.y0 || r.r > px.x0 + px.w || r.b > px.y0 + px.h || r.r <= r.l || r.b <= r.t) continue;
        const uint32_t *from = &px.p[(size_t)(r.t - px.y0) * px.w + (size_t)(r.l - px.x0)];
        uint32_t w = (uint32_t)(r.r - r.l);
        if (s.obj) {
            D3D11_BOX box = { (UINT)r.l, (UINT)r.t, 0, (UINT)r.r, (UINT)r.b, 1 };
            d.ctx->UpdateSubresource(s.obj->res, 0, &box, from, (UINT)px.w * 4, 0);
            gdi_obj_written(s.obj);
            if (!s.vram) continue;
        }
        uint8_t *to = s.obj ? s.vram : s.mem;
        for (int32_t y = r.t; y < r.b; y++)
            memcpy(to + (uint64_t)y * s.pitch + (uint64_t)r.l * 4, from + (size_t)(y - r.t) * px.w, (size_t)w * 4);
        if (s.obj) shadow_take(s.obj, s, (uint32_t)r.l, (uint32_t)r.t, (uint32_t)r.r, (uint32_t)r.b);
        else gdi_mem_written(x, d, s, r);
    }
}

/* x / 255, rounded */
inline uint32_t div255(uint32_t v) { v += 128; return (v + (v >> 8)) >> 8; }

/* the DDI's stretch: a destination column or row's source one */
inline int32_t gdi_map(int32_t xd, int32_t dl, int32_t dr, int32_t sl, int32_t sr)
{
    int64_t ws = sr - sl, wd = dr - dl;
    if (wd <= 0) return sl;
    int64_t v = sl + (((int64_t)(xd - dl) * 2 + 1) * ws) / (2 * wd);      /* truncate((xd - dl + 0.5) * ws / wd + sl) */
    return (int32_t)std::min<int64_t>(std::max<int64_t>(v, sl), sr - 1);
}

/* a ROP3 over 32-bit pixels: bit P*4 + S*2 + D of `rop` */
inline uint32_t rop3(uint32_t rop, uint32_t P, uint32_t S, uint32_t D)
{
    /* the ROPs GDI sends most, in one operation each */
    switch (rop & 0xff) {
    case 0x00: return 0;            /* BLACKNESS */
    case 0x33: return ~S;           /* NOTSRCCOPY */
    case 0x55: return ~D;           /* DSTINVERT */
    case 0x66: return S ^ D;        /* SRCINVERT */
    case 0x88: return S & D;        /* SRCAND */
    case 0xcc: return S;            /* SRCCOPY */
    case 0xee: return S | D;        /* SRCPAINT */
    case 0xf0: return P;            /* PATCOPY */
    case 0xff: return ~0u;          /* WHITENESS */
    }
    uint32_t out = 0;
    for (uint32_t i = 0; i < 8; i++)
        if (rop & (1u << i)) out |= ((i & 4) ? P : ~P) & ((i & 2) ? S : ~S) & ((i & 1) ? D : ~D);
    return out;
}

/* v29: GDI's other operations, on the CPU */
void gdi_ex(Exec &x, D11 &d, const d3dpt_gdi2 &g, const d3dpt_gdi_rect *rc, const GdiSide *src, const GdiSide &dst)
{
    /* the destination's pixels under every sub-rectangle */
    int32_t x0 = INT32_MAX, y0 = INT32_MAX, x1 = INT32_MIN, y1 = INT32_MIN;
    for (uint32_t i = 0; i < g.n; i++) {
        if (!gdi_rect_ok(rc[i], dst, nullptr)) continue;
        x0 = std::min(x0, rc[i].l);
        y0 = std::min(y0, rc[i].t);
        x1 = std::max(x1, rc[i].r);
        y1 = std::max(y1, rc[i].b);
    }
    GdiPx D, S, A;
    if (x1 <= x0 || !gdi_read(d, dst, x0, y0, x1, y1, D, D11::GDI_D)) return;
    const bool stretch = g.op != D3DPT_GDI_ROP && g.op != D3DPT_GDI_CLEARTYPE;
    if (src) {
        /* the source rectangle (a ROP's: the corner, the destination's size) */
        int32_t sx0 = g.sl, sy0 = g.st, sx1 = g.sr, sy1 = g.sb;
        if (!stretch) {
            sx0 = g.sl + (x0 - g.dl);
            sy0 = g.st + (y0 - g.dt);
            sx1 = sx0 + (x1 - x0);
            sy1 = sy0 + (y1 - y0);
        }
        if (!gdi_read(d, *src, sx0, sy0, sx1, sy1, S, D11::GDI_S)) return;
    }
    const uint8_t *gam = nullptr, *inv = nullptr;
    if (g.op == D3DPT_GDI_CLEARTYPE) {
        /* the coverage, at the destination's place plus the offset */
        GdiSide as;
        if (!gdi_side(x, d, g.alpha, as) || !gdi_read(d, as, x0 + g.ax, y0 + g.ay, x1 + g.ax, y1 + g.ay, A, D11::GDI_A)) return;
        if (g.gamma != ~0u) {
            const d3dpt_gdi_side &l = g.lut;
            if (g.gamma > 15 || !x.vram || l.pitch < 512 || (uint64_t)l.offset + (uint64_t)l.pitch * g.gamma + 512 > x.vram_size) return;
            gam = x.vram + l.offset + (uint64_t)l.pitch * g.gamma;
            inv = gam + 256;
        }
    }
    if (!gdi_read_end(d, D) || !gdi_read_end(d, S) || !gdi_read_end(d, A)) return;
    const bool opaque = (g.dst.flags & D3DPT_GDI_TEXTURE) && (g.op == D3DPT_GDI_ROP);
    const int32_t sw = g.sr - g.sl, sh = g.sb - g.st, dw = g.dr - g.dl, dh = g.db - g.dt;
    for (uint32_t i = 0; i < g.n; i++) {
        const d3dpt_gdi_rect &r = rc[i];
        if (!gdi_rect_ok(r, dst, nullptr)) continue;
        for (int32_t yd = r.t; yd < r.b; yd++) {
            int32_t ys = 0, ys1 = 0;
            if (src && stretch) {
                const bool mirror_y = g.op == D3DPT_GDI_STRETCH && (g.flags & 2);
                ys = gdi_map(yd, g.dt, g.db, g.st, g.sb);
                if (mirror_y) ys = g.sb - 1 - (ys - g.st);
                /* a mono mode's shrink: every source row onto this one */
                ys1 = (g.op == D3DPT_GDI_STRETCH && (g.rop == 1 || g.rop == 2) && dh > 0 && sh > dh && !mirror_y)
                          ? std::max(ys + 1, g.st + (int32_t)(((int64_t)(yd + 1 - g.dt) * sh) / dh)) : ys + 1;
                ys1 = std::min(ys1, g.sb);
            }
            for (int32_t xd = r.l; xd < r.r; xd++) {
                uint32_t &out = D.at(xd, yd), dv = out, v = dv;
                switch (g.op) {
                case D3DPT_GDI_ROP:
                    v = rop3(g.rop, g.color, src ? S.at(g.sl + (xd - g.dl), g.st + (yd - g.dt)) : 0, dv);
                    break;
                case D3DPT_GDI_STRETCH:
                case D3DPT_GDI_ALPHA:
                case D3DPT_GDI_TRANSPARENT: {
                    int32_t xs = gdi_map(xd, g.dl, g.dr, g.sl, g.sr);
                    if (g.op == D3DPT_GDI_STRETCH && (g.flags & 1)) xs = g.sr - 1 - (xs - g.sl);
                    uint32_t sv = S.at(xs, ys);
                    if (g.op == D3DPT_GDI_STRETCH) {
                        int32_t xs1 = (g.rop == 1 || g.rop == 2) && dw > 0 && sw > dw && !(g.flags & 1)
                                          ? std::max(xs + 1, g.sl + (int32_t)(((int64_t)(xd + 1 - g.dl) * sw) / dw)) : xs + 1;
                        xs1 = std::min(xs1, g.sr);
                        for (int32_t yy = ys; yy < ys1; yy++)
                            for (int32_t xx = xs; xx < xs1; xx++)
                                sv = g.rop == 1 ? (sv & S.at(xx, yy)) : g.rop == 2 ? (sv | S.at(xx, yy)) : sv;
                        v = sv;
                    } else if (g.op == D3DPT_GDI_TRANSPARENT) {
                        if ((g.flags & 1) ? sv != g.color : (sv & 0xffffffu) != (g.color & 0xffffffu)) v = sv;
                    } else {
                        uint32_t ca = g.flags & 255;
                        v = 0;
                        if (g.flags & 0x100) {
                            /* premultiplied: the source scaled by the constant, then over */
                            uint32_t s4[4], sa;
                            for (int k = 0; k < 4; k++) s4[k] = div255(((sv >> (8 * k)) & 255) * ca);
                            sa = s4[3];
                            for (int k = 0; k < 4; k++)
                                v |= std::min<uint32_t>(255, s4[k] + div255(((dv >> (8 * k)) & 255) * (255 - sa))) << (8 * k);
                        } else {
                            for (int k = 0; k < 4; k++)
                                v |= div255(((sv >> (8 * k)) & 255) * ca + ((dv >> (8 * k)) & 255) * (255 - ca)) << (8 * k);
                        }
                    }
                    break;
                }
                case D3DPT_GDI_CLEARTYPE: {
                    uint32_t a = A.at(xd + g.ax, yd + g.ay);
                    v = dv & 0xff000000u;
                    for (int k = 0; k < 3; k++) {       /* b, g, r */
                        uint32_t dc = (dv >> (8 * k)) & 255, cc = (g.color >> (8 * k)) & 255, ak = (a >> (8 * k)) & 255, o;
                        if (gam) {
                            uint32_t t = gam[dc];
                            o = ak == 0 ? dc : ak == 255 ? (g.color2 >> (8 * k)) & 255
                                                         : inv[std::min<uint32_t>(255, div255(t * (255 - ak) + cc * ak))];
                        } else {
                            uint32_t ar = (a >> 16) & 255, ag = (a >> 8) & 255, f = cc >= dc ? ar : ag;
                            o = div255(dc * (255 - f) + cc * f);
                        }
                        v |= o << (8 * k);
                    }
                    break;
                }
                default:
                    break;
                }
                out = opaque ? (v | 0xff000000u) : v;
            }
        }
    }
    gdi_write(x, d, dst, D, rc, g.n);
}

} // namespace

/* ------------------------------------------------------------- the API */

/* v27's GDI record (d3dpt_exec_ddi.cpp's dispatch) */
bool exec_d11_gdi(Batch &b, const d3dpt_cmd *c)
{
    Exec &x = b.x;
    auto *g = body<d3dpt_gdi>(c, 0, b);
    if (!g) return true;
    if (sizeof(d3dpt_cmd) + sizeof(*g) + (uint64_t)g->n * sizeof(d3dpt_gdi_rect) > c->size) { b.err = D3DPT_ERR_MALFORMED; return true; }
    D11 *d = d11(x);
    if (!d) return true;
    const d3dpt_gdi_rect *rc = (const d3dpt_gdi_rect *)(g + 1);
    GdiSide src, dst;
    /* D3DPT_D11_PROF: GDI records count as 64 + the op (op 8 and 9: 74
     * and 75, past a readback's 72 / 73) */
    struct Prof {
        Exec &x; D11 &d; uint32_t op; std::chrono::steady_clock::time_point t0;
        ~Prof() { if (d.prof) prof_tok(x, d, op < 8 ? 64 + op : 74 + ((op - 8) & 1), t0); }
    } prof{ x, *d, g->op, d->prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point() };
    if (g->op == D3DPT_GDI_SYNC) {
        exec_d11_sync(x, g->dst.handle);       /* nothing if VRAM has it already */
        return true;
    }
    if (g->op >= D3DPT_GDI_ROP && g->op <= D3DPT_GDI_CLEARTYPE) {
        auto *g2 = body<d3dpt_gdi2>(c, 0, b);
        if (!g2) return true;
        if (sizeof(d3dpt_cmd) + sizeof(*g2) + (uint64_t)g2->n * sizeof(d3dpt_gdi_rect) > c->size) { b.err = D3DPT_ERR_MALFORMED; return true; }
        /* a ColorFill's ROP has no source */
        bool has_src = g2->op != D3DPT_GDI_CLEARTYPE && (g2->op != D3DPT_GDI_ROP || g2->src.width);
        if (!gdi_side(x, *d, g2->dst, dst) || (has_src && !gdi_side(x, *d, g2->src, src))) {
            if (d->warn(0x66000000u | g2->op))
                x.log("d11: GDI operation %u from format %u %ux%u into %u %ux%u: not done", g2->op, g2->src.format,
                      g2->src.width, g2->src.height, g2->dst.format, g2->dst.width, g2->dst.height);
            return true;
        }
        gdi_ex(x, *d, *g2, (const d3dpt_gdi_rect *)(g2 + 1), has_src ? &src : nullptr, dst);
        return true;
    }
    if (!gdi_side(x, *d, g->dst, dst)) {
        if (d->warn(0x63000000u)) x.log("d11: a GDI operation with a destination of format %u, %ux%u: not done", g->dst.format, g->dst.width, g->dst.height);
        return true;
    }
    switch (g->op) {
    case D3DPT_GDI_FILL:
        gdi_fill(x, *d, *g, rc, dst);
        break;
    case D3DPT_GDI_COPY:
        if (!gdi_side(x, *d, g->src, src)) {
            if (d->warn(0x64000000u)) x.log("d11: a GDI copy from format %u, %ux%u: not done", g->src.format, g->src.width, g->src.height);
            return true;
        }
        gdi_copy(x, *d, *g, rc, src, dst);
        break;
    case D3DPT_GDI_REFRESH:
        gdi_refresh(*d, dst);
        break;
    default:
        if (d->warn(0x65000000u | (g->op & 0xffff))) x.log("d11: GDI operation %u unknown", g->op);
        break;
    }
    return true;
}

bool exec_d11_op(Batch &b, const d3dpt_cmd *c)
{
    Exec &x = b.x;
    auto *a = body<d3dpt_d11>(c, 0, b);
    if (!a) return true;
    if (sizeof(d3dpt_cmd) + sizeof(*a) + (uint64_t)a->bytes > c->size) { b.err = D3DPT_ERR_MALFORMED; return true; }
    D11 *d = d11(x);
    if (!d) {
        static int warned;
        if (warned++ == 0) x.log("d11: a Direct3D 11 record and no Direct3D 11 device: dropped");
        return true;
    }
    D11Ctx *ctx = enter(x, *d, a->ctx);
    if (!ctx) { b.err = D3DPT_ERR_HOST; return true; }
    Run r{ x, *d, *ctx };
    const uint8_t *p = (const uint8_t *)(a + 1), *end = p + a->bytes;
    while (p + sizeof(d3dpt_d11_tok) <= end) {
        const d3dpt_d11_tok *t = (const d3dpt_d11_tok *)p;
        if (t->size < sizeof(*t) || t->size % 4 || t->size > (uint32_t)(end - p)) { b.err = D3DPT_ERR_MALFORMED; return true; }
        HRESULT before = r.hr;
        auto t0 = d->prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
        run_tok(r, t);
        if (d->prof) prof_tok(x, *d, t->op, t0);
        if (FAILED(r.hr) && SUCCEEDED(before) && d->warn(0x7f000000u | t->op))
            x.log("d11: context 0x%08x token %u: 0x%08x (said once per kind)", a->ctx, t->op, (unsigned)r.hr);
        p += t->size;
    }
    if (r.get) {
        d3dpt_ret *ret = b.slot(a->ret_off, (uint32_t)r.ret.size());
        if (ret) {
            ret->hr = (uint32_t)r.hr;
            ret->bytes = r.hr == S_OK ? (uint32_t)r.ret.size() : 0;
            if (r.hr == S_OK && !r.ret.empty()) memcpy(ret + 1, r.ret.data(), r.ret.size());
        }
    }
    return true;
}

/* d3dpt_exec_features: a Direct3D 11 device opens here, or not */
bool exec_d11_available(Exec &x)
{
    return d11(x) != nullptr;
}

/* Direct3D 9 code is about to use the allocation (d3dpt_exec_ddi.cpp's
 * surface lookup): what this side drew into it goes to VRAM first, and
 * the Direct3D 9 object re-reads it from there */
void exec_d11_sync(Exec &x, uint32_t h)
{
    D11 *d = d11_of(x);
    if (!d) return;
    auto it = d->res.find(h);
    owed_settle(x, *d, h, true);                /* a flip's readback still owed */
    if (it == d->res.end() || !it->second.newer || !it->second.res) return;
    res_readback(x, *d, h, it->second, ~0u);
}

size_t exec_d11_settle(Exec &x, bool wait)
{
    D11 *d = d11_of(x);
    if (!d) return 0;
    if (!d->owed.empty()) owed_settle(x, *d, 0, wait);
    return d->owed.size();
}

/* the allocation went (the kernel driver's VRAM_RELEASE) */
void exec_d11_vram_release(Exec &x, uint32_t h)
{
    D11 *d = d11_of(x);
    if (!d) return;
    owed_settle(x, *d, h, false, true);
    scans_forget(*d, h);
    auto it = d->res.find(h);
    if (it == d->res.end()) return;
    res_drop(*d, it->second);
    d->res.erase(it);
}

/* ---------------------------------------------------- the scanout ring */

/* The ring's images go: after the GPU is done with every copy into them
 * (DXVK wraps our Vulkan images, it does not own them) */
static void ring_drop(D11 &d)
{
    if (d.ring.empty()) return;
    d.ctx->Flush();
    for (D11::Slot &s : d.ring) {
        while (s.done && d.ctx->GetData(s.done, nullptr, 0, 0) == S_FALSE) std::this_thread::yield();
        if (s.done) s.done->Release();
        if (s.tex) s.tex->Release();
#ifdef D3DPT_RING
        if (s.img && d.vkDestroyImage) d.vkDestroyImage(d.vk, s.img, nullptr);
        if (s.mem && d.vkFreeMemory) d.vkFreeMemory(d.vk, s.mem, nullptr);
#endif
    }
    d.ring.clear();
    d.scans.clear();
    d.ring_w = d.ring_h = d.ring_next = 0;
}

#ifdef D3DPT_RING
/* The Vulkan loader DXVK runs on, and the two calls the ring needs of it
 * before it has a device's own */
#ifdef __linux__
/* the instance's, for exec_vk_ring_image's device check (set with the rest) */
static PFN_vkGetPhysicalDeviceProperties2 vk_props2;
#endif

bool exec_vk_procs(VkInstance inst, PFN_vkGetDeviceProcAddr *gdpa, PFN_vkGetPhysicalDeviceMemoryProperties *memprops)
{
#ifdef _WIN32
    HMODULE loader = GetModuleHandleA("vulkan-1.dll");
    auto gipa = loader ? (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(loader, "vkGetInstanceProcAddr") : nullptr;
#elif defined(__linux__)
    void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_NOLOAD);
    auto gipa = loader ? (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr") : nullptr;
    vk_props2 = gipa ? (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2") : nullptr;
#else
    /* the loader image already in the process (DXVK's: the package's own
     * beside it, DXVK patch 06, or the one by leaf name open_d3d9 probed),
     * whatever path it came from */
    void *loader = nullptr;
    for (uint32_t i = 0, n = _dyld_image_count(); i < n && !loader; i++) {
        const char *name = _dyld_get_image_name(i), *leaf = name ? strrchr(name, '/') : nullptr;
        leaf = leaf ? leaf + 1 : name;
        if (leaf && !strncmp(leaf, "libvulkan.", 10)) loader = dlopen(name, RTLD_NOW | RTLD_NOLOAD);
    }
    auto gipa = loader ? (PFN_vkGetInstanceProcAddr)dlsym(loader, "vkGetInstanceProcAddr") : nullptr;
#endif
    *gdpa = gipa ? (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr") : nullptr;
    *memprops = gipa ? (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties") : nullptr;
    return *gdpa && *memprops;
}

/* One of the player's images (an NT handle to a shared Direct3D 12
 * resource, B8G8R8A8 typeless; on macOS an MTLHeap, which KosmicKrisp's
 * VK_EXT_external_memory_metal takes and its textures and buffers it does
 * not) as a Vulkan image on one of DXVK's devices (its d3d11's or its
 * d3d9's), a copy's source and destination. On the heap the image is
 * KosmicKrisp's MTLTexture at offset 0, and the player's own texture
 * there is the same one only as long as both describe it alike
 * (player-core metalring.rs): MUTABLE_FORMAT, Metal's PixelFormatView,
 * keeps it uncompressed, which both sides' layouts then agree on. False,
 * logged, on any refusal. On Linux a d3dpt_exec_ring_fd: an opaque fd is
 * good only on the device and driver that made it, and its import takes
 * the exporter's memory type and size. */
bool exec_vk_ring_image(Exec &x, const char *who, VkDevice dev, VkPhysicalDevice phys, PFN_vkGetDeviceProcAddr gdpa,
                        PFN_vkGetPhysicalDeviceMemoryProperties memprops, void *handle, uint32_t w, uint32_t h,
                        VkImage *img, VkDeviceMemory *mem)
{
#define VKFN(n) auto n = (PFN_##n)gdpa(dev, #n)
    VKFN(vkCreateImage); VKFN(vkGetImageMemoryRequirements); VKFN(vkAllocateMemory);
    VKFN(vkBindImageMemory);
#ifdef _WIN32
    VKFN(vkGetMemoryWin32HandlePropertiesKHR);
    auto props = vkGetMemoryWin32HandlePropertiesKHR;
    const char *ext_name = "VK_KHR_external_memory_win32";
    const VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT;
#elif defined(__linux__)
    VKFN(vkGetMemoryFdKHR);                         /* DXVK patch 20 enables the extension */
    auto props = vkGetMemoryFdKHR;                  /* only asked to be there */
    const char *ext_name = "VK_KHR_external_memory_fd";
    const VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    const d3dpt_exec_ring_fd *rf = (const d3dpt_exec_ring_fd *)handle;
#else
    VKFN(vkGetMemoryMetalHandlePropertiesEXT);      /* DXVK patch 19 enables the extension */
    auto props = vkGetMemoryMetalHandlePropertiesEXT;
    const char *ext_name = "VK_EXT_external_memory_metal";
    const VkExternalMemoryHandleTypeFlagBits type = VK_EXTERNAL_MEMORY_HANDLE_TYPE_MTLHEAP_BIT_EXT;
#endif
#undef VKFN
    if (!vkCreateImage || !vkGetImageMemoryRequirements || !vkAllocateMemory || !vkBindImageMemory || !props) {
        x.log("%s: scanout ring: DXVK's device has no %s", who, ext_name);
        return false;
    }
#ifdef __linux__
    {
        /* DXVK may have picked another GPU than the player's */
        VkPhysicalDeviceIDProperties idp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 pp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp };
        if (!vk_props2) { x.log("%s: scanout ring: no vkGetPhysicalDeviceProperties2", who); return false; }
        vk_props2(phys, &pp);
        if (memcmp(idp.deviceUUID, rf->device_uuid, VK_UUID_SIZE) || memcmp(idp.driverUUID, rf->driver_uuid, VK_UUID_SIZE)) {
            x.log("%s: scanout ring: DXVK runs on another GPU than the player (%s)", who, pp.properties.deviceName);
            return false;
        }
    }
#endif
    VkExternalMemoryImageCreateInfo ext = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, nullptr, (VkExternalMemoryHandleTypeFlags)type };
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &ext };
    ici.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;     /* the resource is typeless */
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_B8G8R8A8_UNORM;
    ici.extent = { w, h, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkResult vr = vkCreateImage(dev, &ici, nullptr, img);
    if (vr != VK_SUCCESS) { x.log("%s: scanout ring: vkCreateImage %d", who, (int)vr); return false; }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, *img, &req);
#ifdef __linux__
    /* the exporter's own type and size, which an opaque fd's import must
     * match (the same image on the same device asks the same) */
    (void)memprops;
    uint32_t type_index = rf->memory_type;
    VkDeviceSize size = rf->size;
    if (type_index >= 32 || !(req.memoryTypeBits & (1u << type_index)) || req.size > size) {
        x.log("%s: scanout ring: the image's memory (type %u, %llu bytes) does not fit ours (types 0x%x, %llu)", who,
              type_index, (unsigned long long)size, req.memoryTypeBits, (unsigned long long)req.size);
        return false;
    }
    int fd = dup(rf->fd);                           /* the import owns it, the player keeps its own */
    if (fd < 0) { x.log("%s: scanout ring: dup: %s", who, strerror(errno)); return false; }
    VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, nullptr, type, fd };
#else
#ifdef _WIN32
    VkMemoryWin32HandlePropertiesKHR hp = { VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR };
    vr = props(dev, type, (HANDLE)handle, &hp);
#else
    VkMemoryMetalHandlePropertiesEXT hp = { VK_STRUCTURE_TYPE_MEMORY_METAL_HANDLE_PROPERTIES_EXT };
    vr = props(dev, type, handle, &hp);
#endif
    if (vr != VK_SUCCESS) { x.log("%s: scanout ring: the handle's properties: %d", who, (int)vr); return false; }
    VkDeviceSize size = req.size;
    VkPhysicalDeviceMemoryProperties mp;
    memprops(phys, &mp);
    uint32_t bits = req.memoryTypeBits & hp.memoryTypeBits, type_index = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)       /* device-local first */
        if ((bits & (1u << i)) && (type_index == UINT32_MAX ||
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
            if (type_index == UINT32_MAX || !(mp.memoryTypes[type_index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type_index = i;
    if (type_index == UINT32_MAX) { x.log("%s: scanout ring: no memory type takes the handle", who); return false; }
#ifdef _WIN32
    VkImportMemoryWin32HandleInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, nullptr, type, (HANDLE)handle, nullptr };
#else
    VkImportMemoryMetalHandleInfoEXT imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_METAL_HANDLE_INFO_EXT, nullptr, type, handle };
#endif
#endif
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &imp, *img, VK_NULL_HANDLE };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, size, type_index };
    vr = vkAllocateMemory(dev, &mai, nullptr, mem);
    if (vr != VK_SUCCESS) {
#ifdef __linux__
        close(fd);
#endif
        x.log("%s: scanout ring: importing the memory: %d", who, (int)vr);
        return false;
    }
    vr = vkBindImageMemory(dev, *img, *mem, 0);
    if (vr != VK_SUCCESS) { x.log("%s: scanout ring: vkBindImageMemory %d", who, (int)vr); return false; }
    return true;
}

/* A ring image on the d3d11 device, and that as a d3d11 texture a copy
 * can target */
static bool ring_import(Exec &x, D11 &d, IDXGIVkInteropDevice1 *interop, VkPhysicalDevice phys,
                 PFN_vkGetDeviceProcAddr gdpa, PFN_vkGetPhysicalDeviceMemoryProperties memprops,
                 void *handle, uint32_t w, uint32_t h, D11::Slot &s)
{
    if (!exec_vk_ring_image(x, "d11", d.vk, phys, gdpa, memprops, handle, w, h, &s.img, &s.mem)) return false;
    /* no bind flags: a copy's destination and source only */
    D3D11_TEXTURE2D_DESC1 td = { w, h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, 0, 0, 0,
                                 D3D11_TEXTURE_LAYOUT_UNDEFINED };
    HRESULT hr = interop->CreateTexture2DFromVkImage(&td, s.img, &s.tex);
    if (FAILED(hr)) { x.log("d11: scanout ring: CreateTexture2DFromVkImage 0x%08x", (unsigned)hr); return false; }
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    hr = d.dev->CreateQuery(&qd, &s.done);
    if (FAILED(hr)) { x.log("d11: scanout ring: an event query 0x%08x", (unsigned)hr); return false; }
    return true;
}
#endif

int exec_d11_ring(Exec &x, void *const *handles, int count, uint32_t w, uint32_t h)
{
    D11 *d = d11(x);
    if (!d) return 0;
    ring_drop(*d);
    if (count <= 0 || !handles || !w || !h) return 0;
#ifdef D3DPT_RING
    IDXGIVkInteropDevice1 *interop = nullptr;
    if (FAILED(d->dev->QueryInterface(__uuidof(IDXGIVkInteropDevice1), (void **)&interop)) || !interop) {
        x.log("d11: scanout ring: the device is not DXVK's (no Vulkan interop)");
        return 0;
    }
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    interop->GetVulkanHandles(&inst, &phys, &d->vk);
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkGetPhysicalDeviceMemoryProperties memprops;
    if (!exec_vk_procs(inst, &gdpa, &memprops)) {
        x.log("d11: scanout ring: no Vulkan loader beside DXVK");
        interop->Release();
        return 0;
    }
    d->vkDestroyImage = (PFN_vkDestroyImage)gdpa(d->vk, "vkDestroyImage");
    d->vkFreeMemory = (PFN_vkFreeMemory)gdpa(d->vk, "vkFreeMemory");
    d->ring.resize((size_t)count);
    d->ring_w = w;
    d->ring_h = h;
    bool ok = true;
    for (int i = 0; ok && i < count; i++)
        ok = ring_import(x, *d, interop, phys, gdpa, memprops, handles[i], w, h, d->ring[(size_t)i]);
    interop->Release();
    if (!ok) {
        ring_drop(*d);
        return 0;
    }
    x.log("d11: scanout ring: %d images of %ux%u, flips copied into them on the GPU", count, w, h);
    return count;
#else
    return 0;
#endif
}

int exec_d11_ring_slot(Exec &x, uint32_t page, bool wait)
{
    D11 *d = d11_of(x);
    if (!d) return -1;
    auto it = d->scans.find(page);
    if (it == d->scans.end()) return -1;
    D11::Slot &s = d->ring[it->second.slot];
    HRESULT hr;
    while ((hr = d->ctx->GetData(s.done, nullptr, 0, wait ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE && wait)
        std::this_thread::yield();
    return hr == S_OK ? (int)it->second.slot : -2;
}

void exec_d11_scanout_vram(Exec &x, uint32_t page)
{
    D11 *d = d11_of(x);
    if (!d) return;
    auto it = d->scans.find(page);
    if (it == d->scans.end()) return;
    auto r = d->res.find(it->second.h);
    if (r == d->res.end() || !r->second.res) return;
    res_readback(x, *d, it->second.h, r->second, ~0u);     /* now, and the slot still holds it too */
}

/* a guest device went (the kernel driver's CTX_DESTROY): its state and
 * objects; true if it was one of this side's */
bool exec_d11_ctx_destroy(Exec &x, uint32_t ctx)
{
    D11 *d = d11_of(x);
    if (!d) return false;
    auto it = d->ctxs.find(ctx);
    if (it == d->ctxs.end()) return false;
    if (d->cur == ctx) {
        d->ctx->SwapDeviceContextState(d->idle, nullptr);
        d->cur = 0;
    }
    d->nobjs -= std::min(d->nobjs, it->second.objs.size());
    it->second.release();
    d->ctxs.erase(it);
    return true;
}

void exec_d11_release(Exec &x)
{
    D11 *&d = d11_of(x);
    if (!d) return;
    if (d->ctx && d->idle) d->ctx->SwapDeviceContextState(d->idle, nullptr);
    if (d->ctx) ring_drop(*d);
    for (auto &kv : d->ctxs) kv.second.release();
    for (auto &kv : d->res) kv.second.release();
    for (D11::GdiTex &t : d->gdi_tex) if (t.tex) t.tex->Release();
    if (d->idle) d->idle->Release();
    if (d->ctx) d->ctx->Release();
    if (d->dev) d->dev->Release();
    delete d;
    d = nullptr;
}

} // namespace d3dpt
