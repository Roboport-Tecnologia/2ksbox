/*
 * d3dpt-dp2-test: drives the display driver's records of the paravirtual
 * Direct3D executor (libd3dpt_exec, doc 15 M7c) without a guest. A
 * malloc'ed stand-in for the d3dpt-vga adapter's VRAM holds a render
 * target, a Z buffer and a texture exactly as dxg's heap would; the test
 * registers them (VRAM_SURFACE), opens a context (CTX_CREATE), sends the
 * D3D7TEST scene as the DP2 token stream the DX7 runtime emits for a
 * non-T&L HAL (guest-tools/src/d3dptvid/d3d7test.c draws the same scene
 * through IDirect3DDevice7 on XP), reads the frame back into "VRAM"
 * (READBACK) and checks pixels; then it feeds hostile records (a surface
 * outside VRAM, a truncated token stream, a lying record size) and
 * expects them refused. The frame is written as a BMP for the eye and for
 * the guest-side diff.
 *
 * Build: c++ -std=c++17 -O2 -o build/d3dpt-dp2-test tools/d3dpt-dp2-test.cpp \
 *          -Ithird_party/dxvk/include/native -Ithird_party/dxvk/include/native/windows \
 *          -Ithird_party/dxvk/include/native/directx -ldl
 * Run:   build/d3dpt-dp2-test [out.bmp]   (from the repo root, or D3DPT_EXEC_LIB / D3DPT_DXVK_LIB)
 *
 * Windows (the Windows executor on DXVK's d3d9.dll), inside
 * scripts/win-cross.sh, then under wine or on the PC:
 *        x86_64-w64-mingw32-g++ -std=c++17 -O2 -static -o build/win/d3dpt-dp2-test.exe tools/d3dpt-dp2-test.cpp
 *        D3DPT_EXEC_LIB=build/win/d3dpt/d3dpt_exec.dll D3DPT_DXVK_LIB=<dxvk_d3d9.dll> wine build/win/d3dpt-dp2-test.exe
 */
#include <windows.h>
#include <d3d9.h>
#ifdef _WIN32
#define dlopen(p, f)  ((void *)LoadLibraryA(p))
#define dlsym(h, s)   ((void *)GetProcAddress((HMODULE)(h), (s)))
#define dlerror()     "LoadLibrary failed"
#else
#include <dlfcn.h>
#endif
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "../d3dpt/d3dpt_enc.h"
#include "../d3dpt/exec/d3dpt_exec.h"

static d3dpt_exec_t *X;
static uint32_t (*p_submit)(d3dpt_exec_t *, void *, uint32_t);
static uint8_t *vram;
static uint32_t dirty_calls, dirty_bytes;
static int failures;

static void cb_log(void *, const char *m) { printf("exec: %s\n", m); }
static void cb_active(void *, int on) { printf("exec: 3D %s\n", on ? "on" : "off"); }
static void cb_dirty(void *, uint32_t off, uint32_t bytes) { dirty_calls++; dirty_bytes += bytes; printf("vram dirty: %u +%u\n", off, bytes); }
static void doorbell(d3dpt_enc *e) { p_submit(X, e->shm, D3DPT_SHM_SIZE); }

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } else { printf("ok: " __VA_ARGS__); printf("\n"); } } while (0)

/* --- the scene, shared with guest-tools/src/d3dptvid/d3d7test.c --- */
enum { W = 640, H = 480, TEX = 64 };
enum { RT_OFF = 0, RT_PITCH = W * 4, Z_OFF = 0x200000, Z_PITCH = W * 2, TEX_OFF = 0x300000, TEX_PITCH = TEX * 4, RT2_OFF = 0x400000,
       VB_OFF = 0x580000, VB_SIZE = 0x10000, IB_OFF = 0x590000, IB_SIZE = 0x1000, VRAM_SIZE = 0x600000 };   /* VB / IB: v9 VRAM buffers */
enum { H_VB = 6, H_IB = 7 };
enum { TEX16_OFF = 0x310000, TEX16_PITCH = TEX * 2, TEXP8_OFF = 0x320000, TEXP8_PITCH = TEX };   /* v8: a 16-bit keyed texture, a palettized one */
enum { H_RT = 1, H_Z = 2, H_TEX = 3, H_TEX16 = 4, H_TEXP8 = 5, CTX = 1 };
#define CLEAR_COLOR 0xff203040u

struct tlv { float x, y, z, rhw; uint32_t diffuse, specular; float tu, tv; };
#define FVF_TLVERTEX 0x1c4   /* XYZRHW | DIFFUSE | SPECULAR | TEX1 */

static void fill_texture(uint8_t *p, int pitch) {
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) {
            uint32_t c = ((x / 8) + (y / 8)) & 1 ? 0xffffffffu : 0xff2040ffu;
            if (y >= 24 && y < 40) c = (c & 0x00ffffffu) | 0x80000000u;      /* a half-transparent band */
            memcpy(p + y * pitch + x * 4, &c, 4);
        }
}

/* the DP2 token stream */
struct Dp2Buf {
    std::vector<uint8_t> b;
    void cmd(uint8_t op, uint16_t count) { b.push_back(op); b.push_back(0); b.push_back(count & 0xff); b.push_back(count >> 8); }
    void u16(uint16_t v) { b.push_back(v & 0xff); b.push_back(v >> 8); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 0xff); }
    void f32(float f) { uint32_t v; memcpy(&v, &f, 4); u32(v); }
    void rs(uint32_t s, uint32_t v) { cmd(8, 1); u32(s); u32(v); }
    void tss(uint16_t stage, uint16_t st, uint32_t v) { cmd(25, 1); u16(stage); u16(st); u32(v); }
    void viewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h) { cmd(28, 1); u32(x); u32(y); u32(w); u32(h); }
    void zrange(float a, float z) { cmd(32, 1); f32(a); f32(z); }
    void clear(uint32_t flags, uint32_t color, float z) { cmd(42, 0); u32(flags); u32(color); f32(z); u32(0); }
    void trilist(uint16_t start, uint16_t count) { cmd(18, count); u16(start); }
    void trifan(uint16_t start, uint16_t count) { cmd(21, count); u16(start); }
    void tristrip(uint16_t start, uint16_t count) { cmd(19, count); u16(start); }
    void indexed_trilist2(uint16_t base, const std::vector<uint16_t> &idx) {
        cmd(26, idx.size() / 3); u16(base); for (uint16_t i : idx) u16(i);
    }
    /* the DX8 DDI's self-contained draw (the display driver's rewrite of the
     * runtime's DRAWPRIMITIVE / DRAWINDEXEDPRIMITIVE): vertices and 16-bit
     * indices inline, indices relative to min_index */
    template <class T> void draw8(uint32_t prim, uint32_t prims, uint32_t fvf, const std::vector<T> &v,
                                  const std::vector<uint16_t> &idx = {}, uint32_t min_index = 0, uint32_t stride_lie = 0) {
        cmd(D3DPT_DP2_DRAW8, 0);
        u32(prim); u32(prims); u32(fvf); u32(stride_lie ? stride_lie : (uint32_t)sizeof(T));
        u32((uint32_t)v.size()); u32((uint32_t)idx.size()); u32(min_index); u32(0);
        for (const T &e : v) { const uint8_t *q = (const uint8_t *)&e; b.insert(b.end(), q, q + sizeof(T)); }
        while (b.size() % 4) b.push_back(0xcc);
        for (uint16_t i : idx) u16(i);
        while (b.size() % 4) b.push_back(0xcc);
    }
    /* the same draw with its vertices (vb != 0: handle + byte offset) and /
     * or its indices (ib != 0) in VRAM buffers the host reads (protocol v9);
     * what is not in VRAM travels inline as above */
    template <class T> void draw8x(uint32_t prim, uint32_t prims, uint32_t fvf, const std::vector<T> &v, uint32_t vb, uint32_t voff,
                                   const std::vector<uint16_t> &idx, uint32_t ib, uint32_t ioff, uint32_t min_index = 0, uint32_t flags_lie = 0) {
        cmd(D3DPT_DP2_DRAW8, 0);
        u32(prim); u32(prims); u32(fvf); u32((uint32_t)sizeof(T));
        u32((uint32_t)v.size()); u32((uint32_t)idx.size()); u32(min_index);
        u32(flags_lie ? flags_lie : (vb ? D3DPT_DRAW8_VRAM_VB : 0) | (ib && !idx.empty() ? D3DPT_DRAW8_VRAM_IB : 0));
        if (vb) { u32(vb); u32(voff); }
        else { for (const T &e : v) { const uint8_t *q = (const uint8_t *)&e; b.insert(b.end(), q, q + sizeof(T)); } while (b.size() % 4) b.push_back(0xcc); }
        if (idx.empty()) return;
        if (ib) { u32(ib); u32(ioff); }
        else { for (uint16_t i : idx) u16(i); while (b.size() % 4) b.push_back(0xcc); }
    }
    /* v10: a DRAW8 with more streams than stream 0 (the vertices above, inline):
     * each either inline (its bytes, nverts * stride) or in a VRAM buffer
     * (vb != 0: handle + byte offset); the lies make hostile records */
    struct Strm { uint32_t stream, stride, vb, voff; std::vector<uint8_t> data; uint32_t flags_lie; uint32_t freq = 0; };
    template <class T> static Strm strm(uint32_t n, const std::vector<T> &v) {
        Strm s = { n, (uint32_t)sizeof(T), 0, 0, {}, 0 };
        s.data.assign((const uint8_t *)v.data(), (const uint8_t *)(v.data() + v.size()));
        return s;
    }
    static Strm strm_vram(uint32_t n, uint32_t stride, uint32_t vb, uint32_t voff) { return { n, stride, vb, voff, {}, 0 }; }
    template <class T> void draw8m(uint32_t prim, uint32_t prims, uint32_t fvf, const std::vector<T> &v, const std::vector<uint16_t> &idx,
                                   uint32_t min_index, const std::vector<Strm> &st, uint32_t count_lie = 0, uint32_t freq0 = 0) {
        cmd(D3DPT_DP2_DRAW8, 0);
        u32(prim); u32(prims); u32(fvf); u32((uint32_t)sizeof(T));
        u32((uint32_t)v.size()); u32((uint32_t)idx.size()); u32(min_index); u32(D3DPT_DRAW8_STREAMS);
        for (const T &e : v) { const uint8_t *q = (const uint8_t *)&e; b.insert(b.end(), q, q + sizeof(T)); }
        while (b.size() % 4) b.push_back(0xcc);
        for (uint16_t i : idx) u16(i);
        while (b.size() % 4) b.push_back(0xcc);
        u32(count_lie ? count_lie : (uint32_t)st.size()); u32(freq0);     /* v16: stream 0's frequency */
        for (const Strm &s : st) {
            u32(s.stream); u32(s.stride); u32(s.flags_lie ? s.flags_lie : s.vb ? D3DPT_DRAW8_VRAM_VB : 0); u32(s.freq);
            if (s.vb) { u32(s.vb); u32(s.voff); }
            else { b.insert(b.end(), s.data.begin(), s.data.end()); while (b.size() % 4) b.push_back(0xcc); }
        }
    }
    void stateset(uint32_t op, uint32_t handle, uint32_t type = 0) { cmd(39, 1); u32(op); u32(handle); u32(type); }
    /* the DX8 shader tokens as the runtime lays them out (protocol v7: the
     * driver forwards them; the host keeps the shaders per context) */
    void create_vs(uint32_t handle, const std::vector<uint32_t> &decl, const std::vector<uint32_t> &code, uint32_t decl_lie = 0) {
        cmd(45, 1); u32(handle); u32(decl_lie ? decl_lie : (uint32_t)decl.size() * 4); u32((uint32_t)code.size() * 4);
        for (uint32_t t : decl) u32(t);
        for (uint32_t t : code) u32(t);
    }
    void create_ps(uint32_t handle, const std::vector<uint32_t> &code) { cmd(54, 1); u32(handle); u32((uint32_t)code.size() * 4); for (uint32_t t : code) u32(t); }
    void set_vs(uint32_t h) { cmd(47, 1); u32(h); }
    void set_ps(uint32_t h) { cmd(56, 1); u32(h); }
    void delete_vs(uint32_t h) { cmd(46, 1); u32(h); }
    void delete_ps(uint32_t h) { cmd(55, 1); u32(h); }
    void vs_const(uint32_t reg, float a, float b, float c, float d) { cmd(48, 1); u32(reg); u32(1); f32(a); f32(b); f32(c); f32(d); }
    void ps_const(uint32_t reg, float a, float b, float c, float d) { cmd(57, 1); u32(reg); u32(1); f32(a); f32(b); f32(c); f32(d); }
    void transform(uint32_t t, const float *m) { cmd(36, 1); u32(t); for (int i = 0; i < 16; i++) f32(m[i]); }
    /* the runtime's palette tokens (v8): SETPALETTE binds palette handle ->
     * surface, UPDATEPALETTE carries PALETTEENTRYs (r, g, b, flags) */
    void set_palette(uint32_t pal, uint32_t flags, uint32_t surface) { cmd(30, 1); u32(pal); u32(flags); u32(surface); }
    void update_palette(uint32_t pal, uint16_t start, const std::vector<uint32_t> &rgb) {
        cmd(31, 1); u32(pal); u16(start); u16((uint16_t)rgb.size());
        for (uint32_t c : rgb) { b.push_back((c >> 16) & 0xff); b.push_back((c >> 8) & 0xff); b.push_back(c & 0xff); b.push_back((c >> 24) & 0xff); }
    }
    /* TRIANGLEFAN_IMM as the runtime lays it out: the 4-byte header (which
     * may sit at offset 2 mod 4), padding to a DWORD boundary, the edge
     * flags, the vertices inline, padding so the next token starts at a
     * DWORD-aligned offset */
    template <class T> void trifan_imm(const std::vector<T> &v) {
        cmd(23, v.size() - 2);
        while (b.size() % 4) b.push_back(0xcc);                 /* the pad bytes are not data */
        u32(0);
        for (const T &e : v) { const uint8_t *q = (const uint8_t *)&e; b.insert(b.end(), q, q + sizeof(T)); }
        while (b.size() % 4) b.push_back(0xcc);
    }
};

static tlv V(float x, float y, float z, uint32_t c, float u, float v) { return { x, y, z, 1.0f, c, 0xff000000u, u, v }; }

static void build_scene(Dp2Buf &d, std::vector<tlv> &vtx) {
    /* vertex buffer: quad (0..5), fan (6..11), triangle (12..14), alpha strip (15..18) */
    vtx = {
        V(100, 80, 0.5f, 0xffffffff, 0, 0), V(420, 80, 0.5f, 0xffffffff, 2, 0), V(100, 320, 0.5f, 0xffffffff, 0, 2),
        V(100, 320, 0.5f, 0xffffffff, 0, 2), V(420, 80, 0.5f, 0xffffffff, 2, 0), V(420, 320, 0.5f, 0xffffffff, 2, 2),
    };
    for (int i = 0; i < 6; i++) {
        float a = i ? (float)(i - 1) * 6.2831853f / 4.0f : 0.0f;
        uint32_t col[] = { 0xffffffff, 0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffff00, 0xffff0000 };
        vtx.push_back(i == 0 ? V(480, 240, 0.3f, col[0], 0, 0)
                             : V(480 + 100 * cosf(a), 240 + 100 * sinf(a), 0.3f, col[i], 0, 0));
    }
    vtx.push_back(V(300, 40, 0.7f, 0xff00ffff, 0, 0)); vtx.push_back(V(600, 420, 0.7f, 0xff00ffff, 0, 0)); vtx.push_back(V(60, 460, 0.7f, 0xff00ffff, 0, 0));
    vtx.push_back(V(20, 360, 0.1f, 0x80ff0000, 0, 0)); vtx.push_back(V(220, 360, 0.1f, 0x80ff0000, 0, 0));
    vtx.push_back(V(20, 470, 0.1f, 0x80ff0000, 0, 0)); vtx.push_back(V(220, 470, 0.1f, 0x80ff0000, 0, 0));

    d.viewport(0, 0, W, H);
    d.zrange(0.0f, 1.0f);
    d.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
    d.rs(D3DRS_ZENABLE, 1); d.rs(D3DRS_ZWRITEENABLE, 1); d.rs(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d.rs(D3DRS_LIGHTING, 0); d.rs(D3DRS_CULLMODE, D3DCULL_NONE); d.rs(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    d.rs(D3DRS_ALPHABLENDENABLE, 0); d.rs(D3DRS_DITHERENABLE, 0); d.rs(D3DRS_SPECULARENABLE, 0);
    d.rs(3, 1); d.rs(41, 0);                                    /* DX7-only states: dropped by the host */
    /* the cyan triangle first, behind the quad (z 0.7) */
    d.tss(0, 0, 0); d.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); d.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    d.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); d.tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    /* twice, so the next token lands at offset 2 mod 4 (2 + 6 × 2 index words) */
    d.indexed_trilist2(12, { 0, 1, 2, 0, 1, 2 });
    /* the coloured fan in front, as inline vertices at that odd offset (the
     * DX8 runtime's legacy path does this; the parser must realign after it) */
    d.trifan_imm(std::vector<tlv>(vtx.begin() + 6, vtx.begin() + 12));
    /* the textured quad */
    d.tss(0, 0, H_TEX); d.tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE); d.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); d.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); d.tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    d.tss(0, 16, 1); d.tss(0, 17, 1); d.tss(0, 18, 1); d.tss(0, 12, D3DTADDRESS_WRAP);   /* point sampling, no mips */
    d.trilist(0, 2);
    d.tss(0, 0, 0); d.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); d.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
    /* a half-transparent red strip */
    d.rs(D3DRS_ALPHABLENDENABLE, 1); d.rs(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); d.rs(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    d.tristrip(15, 2);
    d.rs(D3DRS_ALPHABLENDENABLE, 0);
}

static bool bmp_write(const char *path, const uint8_t *bits, int pitch, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    int row = (w * 3 + 3) & ~3;
    unsigned char hdr[54] = {}; unsigned size = 54 + row * h;
    hdr[0] = 'B'; hdr[1] = 'M'; memcpy(hdr + 2, &size, 4); hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4); hdr[26] = 1; hdr[28] = 24;
    fwrite(hdr, 1, 54, f);
    std::vector<unsigned char> line(row);
    for (int y = h - 1; y >= 0; y--) {
        const unsigned char *src = bits + y * pitch;
        for (int x = 0; x < w; x++) { line[x * 3] = src[x * 4]; line[x * 3 + 1] = src[x * 4 + 1]; line[x * 3 + 2] = src[x * 4 + 2]; }
        fwrite(line.data(), 1, row, f);
    }
    fclose(f);
    return true;
}

static uint32_t px(int x, int y) { uint32_t v; memcpy(&v, vram + RT_OFF + y * RT_PITCH + x * 4, 4); return v & 0xffffff; }
static bool near_(uint32_t a, uint32_t b, int tol) {
    for (int i = 0; i < 24; i += 8) if (abs((int)((a >> i) & 255) - (int)((b >> i) & 255)) > tol) return false;
    return true;
}

static void vram_surface(d3dpt_enc *e, uint32_t handle, uint32_t off, uint32_t w, uint32_t h, uint32_t pitch, uint32_t fmt, uint32_t caps) {
    d3dpt_vram_surface *s = (d3dpt_vram_surface *)d3dpt_enc_cmd(e, D3DPT_OP_VRAM_SURFACE, sizeof *s, 0);
    *s = { handle, off, w, h, pitch, fmt, caps, 1 };
}

static uint32_t send_dp2(d3dpt_enc *e, const Dp2Buf &d, const std::vector<tlv> &vtx, uint32_t cmd_bytes_claim = 0, uint32_t *err_off = nullptr,
                         uint32_t ctx = CTX) {
    uint32_t cb = (uint32_t)d.b.size(), vb = (uint32_t)(vtx.size() * sizeof(tlv));
    uint32_t off = d3dpt_enc_ret(e, 0);
    d3dpt_dp2 *a = (d3dpt_dp2 *)d3dpt_enc_cmd(e, D3DPT_OP_DP2, sizeof *a, D3DPT_ALIGN8(cb) + vb);
    *a = { ctx, off, 0, FVF_TLVERTEX, sizeof(tlv), cmd_bytes_claim ? cmd_bytes_claim : cb, vb, 0 };
    memcpy(a + 1, d.b.data(), cb);
    memcpy((uint8_t *)(a + 1) + D3DPT_ALIGN8(cb), vtx.data(), vb);
    d3dpt_enc_flush(e);
    if (e->last_status) return 0xffff0000u | e->last_status;
    d3dpt_ret *r = d3dpt_enc_result(e, off);
    if (err_off) *err_off = r->bytes;
    return r->hr;
}

static uint32_t readback(d3dpt_enc *e, uint32_t handle) { return d3dpt_enc_sync(e, D3DPT_OP_READBACK, handle); }

int main(int argc, char **argv) {
    const char *out = argc > 1 ? argv[1] : "d3dpt-dp2-test.bmp";
    const char *lib = getenv("D3DPT_EXEC_LIB");
    if (!lib) lib = "build/d3dpt/libd3dpt_exec.so";
    void *h = dlopen(lib, RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 1; }
    auto p_version = (uint32_t (*)(void))dlsym(h, "d3dpt_exec_version");
    auto p_create = (d3dpt_exec_t *(*)(const d3dpt_exec_ops *))dlsym(h, "d3dpt_exec_create");
    auto p_attach = (void (*)(d3dpt_exec_t *, int))dlsym(h, "d3dpt_exec_attach");
    auto p_destroy = (void (*)(d3dpt_exec_t *))dlsym(h, "d3dpt_exec_destroy");
    auto p_set_vram = (void (*)(d3dpt_exec_t *, void *, uint32_t))dlsym(h, "d3dpt_exec_set_vram");
    p_submit = (uint32_t (*)(d3dpt_exec_t *, void *, uint32_t))dlsym(h, "d3dpt_exec_submit");
    if (!p_version || !p_create || !p_attach || !p_destroy || !p_submit || !p_set_vram) { fprintf(stderr, "bad executor library\n"); return 1; }
    printf("executor protocol %u (header %u)\n", p_version(), D3DPT_PROTO_VERSION);
    if (p_version() != D3DPT_PROTO_VERSION) { fprintf(stderr, "protocol mismatch\n"); return 1; }
    d3dpt_exec_ops ops = { nullptr, cb_log, cb_active, cb_dirty };
    X = p_create(&ops);
    if (!X) { printf("no executor (no DXVK / Vulkan device)\n"); return 77; }
    p_attach(X, 1);

    uint8_t *shm = (uint8_t *)calloc(1, D3DPT_SHM_SIZE);
    vram = (uint8_t *)calloc(1, VRAM_SIZE);
    p_set_vram(X, vram, VRAM_SIZE);
    fill_texture(vram + TEX_OFF, TEX_PITCH);
    d3dpt_enc enc;
    d3dpt_enc_init(&enc, shm, doorbell);

    /* --- the scene --- */
    vram_surface(&enc, H_RT, RT_OFF, W, H, RT_PITCH, D3DFMT_X8R8G8B8, D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY);
    vram_surface(&enc, H_Z, Z_OFF, W, H, Z_PITCH, D3DFMT_D16, D3DPT_VS_ZBUFFER);
    vram_surface(&enc, H_TEX, TEX_OFF, TEX, TEX, TEX_PITCH, D3DFMT_A8R8G8B8, D3DPT_VS_TEXTURE);
    {
        uint32_t off = d3dpt_enc_ret(&enc, 0);
        d3dpt_ctx_create *c = (d3dpt_ctx_create *)d3dpt_enc_cmd(&enc, D3DPT_OP_CTX_CREATE, sizeof *c, 0);
        *c = { CTX, off, H_RT, H_Z };
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == 0 && d3dpt_enc_result(&enc, off)->hr == 0, "context created (status %u hr 0x%08x)", enc.last_status, d3dpt_enc_result(&enc, off)->hr);
    }
    Dp2Buf d; std::vector<tlv> vtx;
    build_scene(d, vtx);
    uint32_t hr = send_dp2(&enc, d, vtx);
    CHECK(hr == 0, "DP2 scene (%zu token bytes, %zu vertices) -> 0x%08x", d.b.size(), vtx.size(), hr);
    hr = readback(&enc, H_RT);
    CHECK(hr == 0, "readback -> 0x%08x, vram_dirty %u calls %u bytes", hr, dirty_calls, dirty_bytes);
    CHECK(dirty_calls == 1 && dirty_bytes == RT_PITCH * H, "readback marked the whole target dirty");
    bmp_write(out, vram + RT_OFF, RT_PITCH, W, H);
    printf("frame -> %s\n", out);

    /* pixels: clear colour, the texture (white / blue cells, 8 px each, wrapped twice over 320 px = 2.5 px per texel),
     * the fan centre (white), the cyan triangle where nothing covers it, the blended strip */
    CHECK(px(620, 20) == (CLEAR_COLOR & 0xffffff), "background is the clear colour (0x%06x)", px(620, 20));
    uint32_t q1 = px(104, 84), q2 = px(124, 84);                 /* first cell blue, second cell white */
    CHECK(near_(q1, 0x2040ff, 2) && near_(q2, 0xffffff, 2), "textured quad cells 0x%06x 0x%06x", q1, q2);
    CHECK(near_(px(480, 240), 0xffffff, 2), "fan centre 0x%06x", px(480, 240));
    CHECK(near_(px(560, 400), 0x00ffff, 2), "cyan triangle 0x%06x", px(560, 400));
    CHECK(near_(px(380, 300), 0xffffff, 2) || near_(px(380, 300), 0x2040ff, 2), "quad hides the triangle (z test) 0x%06x", px(380, 300));
    uint32_t bl = px(40, 365);                                    /* 50% red over the clear colour */
    CHECK(near_(bl, 0x8f1820, 6), "alpha-blended strip 0x%06x", bl);

    /* the second readback without new drawing is a no-op (S_FALSE) */
    hr = readback(&enc, H_RT);
    CHECK(hr == 1, "readback without drawing -> S_FALSE (0x%08x)", hr);

    /* VRAM_DIRTY on the texture + a re-render picks up new texels */
    memset(vram + TEX_OFF, 0x40, TEX * TEX_PITCH);                /* dark grey, alpha 0x40 */
    { d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_DIRTY, sizeof *hh, 0); *hh = { H_TEX, 0 }; }
    Dp2Buf d2; d2.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
    d2.tss(0, 0, H_TEX); d2.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); d2.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    d2.trilist(0, 2);
    hr = send_dp2(&enc, d2, vtx);
    hr |= readback(&enc, H_RT);
    CHECK(hr == 0 && near_(px(200, 200), 0x404040, 2), "texture re-read after VRAM_DIRTY (0x%06x)", px(200, 200));

    uint32_t err_off = 0;

    /* --- the DX8 DDI: the driver's self-contained draws, state sets --- */
    {
        /* the same scene through DRAW8 tokens: the quad as an indexed
         * triangle list whose indices are relative to a MinIndex of 10, the
         * fan as a plain draw; then a recorded state set that switches the
         * quad to its diffuse colour, executed, captured back, deleted */
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6), fan(vtx.begin() + 6, vtx.begin() + 12);
        Dp2Buf e8;
        e8.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        e8.tss(0, 0, H_TEX); e8.tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE); e8.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        e8.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); e8.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); e8.tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        e8.draw8(4, 2, FVF_TLVERTEX, quad, { 10, 11, 12, 13, 14, 15 }, 10);
        e8.tss(0, 0, 0); e8.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); e8.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2);
        e8.draw8(6, 4, FVF_TLVERTEX, fan);
        hr = send_dp2(&enc, e8, vtx);
        hr |= readback(&enc, H_RT);
        /* the texture is the re-read case's grey by now */
        CHECK(hr == 0 && near_(px(104, 84), 0x404040, 2) && near_(px(124, 84), 0x404040, 2) && near_(px(480, 240), 0xffffff, 2),
              "DRAW8 tokens: indexed quad (0x%06x 0x%06x) and fan (0x%06x)", px(104, 84), px(124, 84), px(480, 240));
        Dp2Buf sb;
        sb.stateset(0, 77);                                     /* BEGIN handle 77: record */
        sb.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);           /* the quad's colour from its diffuse (white) */
        sb.stateset(1, 77);                                     /* END */
        sb.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        sb.tss(0, 0, H_TEX); sb.tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        sb.stateset(3, 77);                                     /* EXECUTE: SELECTARG2 again */
        sb.draw8(4, 2, FVF_TLVERTEX, quad, { 10, 11, 12, 13, 14, 15 }, 10);
        sb.stateset(4, 77);                                     /* CAPTURE and DELETE: accepted */
        sb.stateset(2, 77);
        sb.stateset(3, 77);                                     /* EXECUTE of a deleted set: logged, not fatal */
        hr = send_dp2(&enc, sb, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0xffffff, 2) && near_(px(124, 84), 0xffffff, 2),
              "state set recorded, executed: quad drawn from diffuse (0x%06x 0x%06x)", px(104, 84), px(124, 84));
        Dp2Buf bad8;
        bad8.draw8(4, 2, FVF_TLVERTEX, quad, { 10, 11, 12, 13, 14, 99 }, 10);    /* an index beyond the copied range */
        hr = send_dp2(&enc, bad8, vtx);
        CHECK(hr == 0, "DRAW8 with an index beyond its vertices: skipped, not fatal (0x%08x)", hr);
        Dp2Buf lie8;
        lie8.draw8(4, 2, FVF_TLVERTEX, quad, {}, 0, 20);                        /* a stride that is not the FVF's */
        hr = send_dp2(&enc, lie8, vtx, 0, &err_off);
        CHECK(hr == 0x88760BB8u, "DRAW8 lying about its stride -> D3DERR_COMMAND_UNPARSED (0x%08x)", hr);

        /* --- v9: vertex / index buffers in VRAM (D3DPT_VS_BUFFER), named by
         * the draws instead of copied: the quad's vertices at an offset of a
         * VRAM buffer, its indices in another, both / either in VRAM --- */
        memcpy(vram + VB_OFF + 64, quad.data(), quad.size() * sizeof(tlv));
        const uint16_t qidx[6] = { 10, 11, 12, 13, 14, 15 };
        memcpy(vram + IB_OFF + 32, qidx, sizeof qidx);
        vram_surface(&enc, H_VB, VB_OFF, VB_SIZE, 1, VB_SIZE, 0, D3DPT_VS_BUFFER);
        vram_surface(&enc, H_IB, IB_OFF, IB_SIZE, 1, IB_SIZE, 0, D3DPT_VS_BUFFER);
        Dp2Buf v8;
        v8.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        v8.tss(0, 0, H_TEX); v8.tss(0, D3DTSS_COLOROP, D3DTOP_MODULATE); v8.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        v8.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE); v8.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); v8.tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        v8.draw8x(4, 2, FVF_TLVERTEX, quad, H_VB, 64, { 10, 11, 12, 13, 14, 15 }, H_IB, 32, 10);
        hr = send_dp2(&enc, v8, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x404040, 2) && near_(px(124, 84), 0x404040, 2) && near_(px(480, 240), CLEAR_COLOR & 0xffffff, 2),
              "DRAW8 from VRAM buffers: vertices and indices read at their offsets (0x%06x 0x%06x, background 0x%06x)", px(104, 84), px(124, 84), px(480, 240));
        Dp2Buf m8;                                              /* mixed: VRAM vertices + inline indices, inline vertices + VRAM indices */
        m8.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        m8.draw8x(4, 1, FVF_TLVERTEX, quad, H_VB, 64, { 10, 11, 12 }, 0, 0, 10);
        m8.draw8x(4, 1, FVF_TLVERTEX, quad, 0, 0, { 13, 14, 15 }, H_IB, 32 + 6, 10);
        hr = send_dp2(&enc, m8, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x404040, 2) && near_(px(124, 84), 0x404040, 2),
              "DRAW8 mixed inline / VRAM vertices and indices (0x%06x 0x%06x)", px(104, 84), px(124, 84));
        /* the guest wrote the buffer (VRAM_DIRTY_RANGE) and the draw sees the
         * new bytes: the quad moved right by 300 pixels */
        for (size_t i = 0; i < quad.size(); i++) ((tlv *)(vram + VB_OFF + 64))[i].x += 300.0f;
        { d3dpt_u32x3 *dr = (d3dpt_u32x3 *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_DIRTY_RANGE, sizeof *dr, 0); *dr = { H_VB, 64, (uint32_t)(quad.size() * sizeof(tlv)), 0 }; }
        Dp2Buf w8;
        w8.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        w8.draw8x(4, 2, FVF_TLVERTEX, quad, H_VB, 64, { 10, 11, 12, 13, 14, 15 }, H_IB, 32, 10);
        hr = send_dp2(&enc, w8, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), CLEAR_COLOR & 0xffffff, 2) && near_(px(404, 84), 0x404040, 2),
              "DRAW8 after the guest rewrote the VRAM buffer: the quad moved (0x%06x at the old place, 0x%06x at the new)", px(104, 84), px(404, 84));
        /* hostile: a range beyond the buffer, an unknown buffer, a texture
         * named as a buffer, a flag the host does not know. The first three
         * skip the draw, the last refuses the record */
        Dp2Buf h8;
        h8.draw8x(4, 2, FVF_TLVERTEX, quad, H_VB, VB_SIZE - 32, { 10, 11, 12, 13, 14, 15 }, H_IB, 32, 10);
        h8.draw8x(4, 2, FVF_TLVERTEX, quad, 4242, 0, { 10, 11, 12, 13, 14, 15 }, H_IB, 32, 10);
        h8.draw8x(4, 2, FVF_TLVERTEX, quad, H_TEX, 0, { 10, 11, 12, 13, 14, 15 }, H_IB, IB_SIZE, 10);
        hr = send_dp2(&enc, h8, vtx);
        CHECK(hr == 0, "DRAW8 naming a range beyond its buffer, an unknown buffer, a texture: skipped, not fatal (0x%08x)", hr);
        Dp2Buf f8;
        f8.draw8x(4, 2, FVF_TLVERTEX, quad, H_VB, 64, {}, 0, 0, 0, 0x80);
        hr = send_dp2(&enc, f8, vtx, 0, &err_off);
        CHECK(hr == 0x88760BB8u, "DRAW8 with an unknown flag -> D3DERR_COMMAND_UNPARSED (0x%08x)", hr);
    }

    /* --- the DX8 DDI: vertex and pixel shaders 1.x (protocol v7) --- */
    {
        /* hand-assembled shader models 1.x: a version token, instruction
         * tokens (opcode), parameter tokens (bit 31, register type in bits
         * 28..30, write mask / swizzle in 16..23, the register number) */
        const uint32_t VS11 = 0xFFFE0101u, PS11 = 0xFFFF0101u, END = 0x0000FFFFu, MOV = 1, MUL = 5, TEX = 66;
        auto dst = [](uint32_t type, uint32_t n) { return 0x80000000u | (type << 28) | (0xFu << 16) | n; };
        auto src = [](uint32_t type, uint32_t n) { return 0x80000000u | (type << 28) | (0xE4u << 16) | n; };
        enum { R_TEMP = 0, R_INPUT = 1, R_CONST = 2, R_TEXTURE = 3, R_RASTOUT = 4, R_ATTROUT = 5 };
        /* D3DVSD_* declaration tokens: STREAM(n) = 1 << 29 | n, REG(reg, type) = 2 << 29 | type << 16 | reg,
         * CONST(reg, count) = 4 << 29 | count << 25 | reg (+ count float4s), END = 0xFFFFFFFF */
        auto STREAM = [](uint32_t n) { return (1u << 29) | n; };
        auto REG = [](uint32_t reg, uint32_t type) { return (2u << 29) | (type << 16) | reg; };
        auto VSCONST = [](uint32_t reg, uint32_t count) { return (4u << 29) | (count << 25) | reg; };
        const uint32_t T_FLOAT3 = 2, T_FLOAT4 = 3, T_D3DCOLOR = 4;
        auto F = [](float f) { uint32_t v; memcpy(&v, &f, 4); return v; };
        /* vs 1.1: oPos = v0 (clip space as given), oD0 = v5 * c0 */
        std::vector<uint32_t> vs_code = { VS11, MOV, dst(R_RASTOUT, 0), src(R_INPUT, 0), MUL, dst(R_ATTROUT, 0), src(R_INPUT, 5), src(R_CONST, 0), END };
        std::vector<uint32_t> decl_pc = { STREAM(0), REG(0, T_FLOAT4), REG(5, T_D3DCOLOR), 0xFFFFFFFFu };
        /* ps 1.1: r0 = c0; and r0 = t0 * v0 (the texture times the diffuse) */
        std::vector<uint32_t> ps_const = { PS11, MOV, dst(R_TEMP, 0), src(R_CONST, 0), END };
        std::vector<uint32_t> ps_tex = { PS11, TEX, dst(R_TEXTURE, 0), MUL, dst(R_TEMP, 0), src(R_TEXTURE, 0), src(R_INPUT, 0), END };
        struct svtx { float x, y, z, w; uint32_t color; };                     /* 20 bytes: the declaration above */
        /* a white quad over the top-left quadrant in clip space */
        std::vector<svtx> sq = { { -1, 1, 0.5f, 1, 0xffffffffu }, { 0, 1, 0.5f, 1, 0xffffffffu }, { -1, 0, 0.5f, 1, 0xffffffffu },
                                 { -1, 0, 0.5f, 1, 0xffffffffu }, { 0, 1, 0.5f, 1, 0xffffffffu }, { 0, 0, 0.5f, 1, 0xffffffffu } };
        const uint32_t H_VS = 0x101, H_VS_FF = 0x103, H_VS_CONST = 0x105, H_PS = 0x201, H_PS_TEX = 0x203;
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6), fan(vtx.begin() + 6, vtx.begin() + 12);
        Dp2Buf s;
        s.create_vs(H_VS, decl_pc, vs_code);
        s.vs_const(0, 1, 0, 0, 1);                                             /* c0 = red */
        s.set_vs(H_VS);
        s.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        s.tss(0, 0, 0); s.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); s.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        s.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); s.tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        s.draw8(4, 2, H_VS, sq);
        hr = send_dp2(&enc, s, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0xff0000, 2) && px(500, 400) == (CLEAR_COLOR & 0xffffff),
              "vs 1.1 through its declaration, c0 red: quad 0x%06x, outside 0x%06x", px(100, 100), px(500, 400));
        Dp2Buf s2;
        s2.vs_const(0, 0, 1, 0, 1);                                            /* c0 = green: the constant alone changes the frame */
        s2.draw8(4, 2, H_VS, sq);
        hr = send_dp2(&enc, s2, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0x00ff00, 2), "vertex shader constant c0 green: 0x%06x", px(100, 100));
        /* the DX9 DDI (M16): a declaration (CREATEVERTEXSHADERDECL, D3DVERTEXELEMENT9s) and
         * a vs 1.1 function apart (CREATE / SETVERTEXSHADERFUNC). d3d9.dll's vs 1.1
         * carries dcl instructions, which d3d8.dll's never did; a dcl naming a
         * temporary instead of an input is refused, and the draw falls back.
         * A context of its own: a context is d3d8.dll's or d3d9.dll's, never both */
        {
            const uint32_t H_DECL9 = 0x301, H_VF9 = 0x302, H_VF9_BAD = 0x304, DCL = 31, CTX9 = CTX + 1;
            {
                uint32_t off = d3dpt_enc_ret(&enc, 0);
                d3dpt_ctx_create *c = (d3dpt_ctx_create *)d3dpt_enc_cmd(&enc, D3DPT_OP_CTX_CREATE, sizeof *c, 0);
                *c = { CTX9, off, H_RT, H_Z };
                d3dpt_enc_flush(&enc);
                CHECK(enc.last_status == 0 && d3dpt_enc_result(&enc, off)->hr == 0, "a second context for the DX9 DDI (hr 0x%08x)",
                      d3dpt_enc_result(&enc, off)->hr);
            }
            const D3DVERTEXELEMENT9 el9[2] = { { 0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                                               { 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 } };
            std::vector<uint32_t> vf = { VS11, DCL, 0x80000000u, dst(R_INPUT, 0), DCL, 0x8000000Au, dst(R_INPUT, 1),
                                         MOV, dst(R_RASTOUT, 0), src(R_INPUT, 0), MUL, dst(R_ATTROUT, 0), src(R_INPUT, 1), src(R_CONST, 0), END };
            std::vector<uint32_t> vf_bad = vf;
            vf_bad[3] = dst(R_TEMP, 0);
            auto create_vf = [](Dp2Buf &d, uint32_t h, const std::vector<uint32_t> &code) {
                d.cmd(74, 1); d.u32(h); d.u32((uint32_t)(code.size() * 4));
                for (uint32_t w : code) d.u32(w);
            };
            Dp2Buf s9;
            s9.cmd(71, 1); s9.u32(H_DECL9); s9.u32(2);
            for (const D3DVERTEXELEMENT9 &e : el9) {
                uint8_t raw[8];
                memcpy(raw, &e, 8);
                s9.b.insert(s9.b.end(), raw, raw + 8);
            }
            create_vf(s9, H_VF9, vf);
            create_vf(s9, H_VF9_BAD, vf_bad);
            s9.vs_const(0, 0, 0, 1, 1);                                        /* c0 = blue */
            s9.cmd(76, 1); s9.u32(H_VF9);
            s9.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            s9.tss(0, 0, 0); s9.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); s9.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            s9.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); s9.tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            s9.draw8(4, 2, H_DECL9, sq);
            hr = send_dp2(&enc, s9, vtx, 0, nullptr, CTX9);
            hr |= readback(&enc, H_RT);
            CHECK(hr == 0 && near_(px(100, 100), 0x0000ff, 2) && px(500, 400) == (CLEAR_COLOR & 0xffffff),
                  "DX9 vs 1.1 with dcl tokens over a D3DVERTEXELEMENT9 declaration, c0 blue: quad 0x%06x, outside 0x%06x",
                  px(100, 100), px(500, 400));
            Dp2Buf b9;
            b9.cmd(76, 1); b9.u32(H_VF9_BAD);
            b9.draw8(4, 2, H_DECL9, sq);
            hr = send_dp2(&enc, b9, vtx, 0, nullptr, CTX9);
            CHECK(hr == 0, "DX9 vs 1.1 whose dcl names a temporary: refused, the draw not fatal (0x%08x)", hr);
            d3dpt_handle *dc = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_CTX_DESTROY, sizeof *dc, 0);
            *dc = { CTX9, 0 };
            d3dpt_enc_flush(&enc);
        }
        /* a declaration-only shader: the fixed function on a layout that is
         * no FVF (the colour before the position), identity transforms */
        struct ffvtx { uint32_t color; float x, y, z; };
        std::vector<ffvtx> fq;
        for (const svtx &v : sq) fq.push_back({ 0xff00ffffu, v.x, v.y, v.z });
        static const float ident[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        Dp2Buf s3;
        s3.create_vs(H_VS_FF, { STREAM(0), REG(5, T_D3DCOLOR), REG(0, T_FLOAT3), 0xFFFFFFFFu }, {});
        s3.transform(256, ident); s3.transform(2, ident); s3.transform(3, ident);
        s3.rs(D3DRS_LIGHTING, 0);
        s3.set_vs(H_VS_FF);
        s3.draw8(4, 2, H_VS_FF, fq);
        hr = send_dp2(&enc, s3, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0x00ffff, 2), "declaration-only shader (fixed function, colour first): 0x%06x", px(100, 100));
        /* D3DVSD_CONST in the declaration: loaded when the shader is set */
        Dp2Buf s4;
        s4.create_vs(H_VS_CONST, { STREAM(0), REG(0, T_FLOAT4), REG(5, T_D3DCOLOR), VSCONST(0, 1), F(0), F(0), F(1), F(1), 0xFFFFFFFFu }, vs_code);
        s4.set_vs(H_VS_CONST);
        s4.draw8(4, 2, H_VS_CONST, sq);
        hr = send_dp2(&enc, s4, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0x0000ff, 2), "declaration constant c0 blue: 0x%06x", px(100, 100));
        /* a pixel shader over it: r0 = c0 (yellow); then off again */
        Dp2Buf s5;
        s5.create_ps(H_PS, ps_const);
        s5.ps_const(0, 1, 1, 0, 1);
        s5.set_ps(H_PS);
        s5.draw8(4, 2, H_VS_CONST, sq);
        hr = send_dp2(&enc, s5, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0xffff00, 2), "ps 1.1 r0 = c0 yellow: 0x%06x", px(100, 100));
        Dp2Buf s6;
        s6.set_ps(0);
        s6.draw8(4, 2, H_VS_CONST, sq);
        hr = send_dp2(&enc, s6, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(100, 100), 0x0000ff, 2), "pixel shader off: the vertex colour again 0x%06x", px(100, 100));
        /* a texturing pixel shader on fixed-function (XYZRHW) vertices: the
         * texture is the grey of the re-read case by now */
        Dp2Buf s7;
        s7.create_ps(H_PS_TEX, ps_tex);
        s7.set_ps(H_PS_TEX);
        s7.tss(0, 0, H_TEX);
        s7.set_vs(FVF_TLVERTEX);
        s7.draw8(4, 2, FVF_TLVERTEX, quad, { 10, 11, 12, 13, 14, 15 }, 10);
        s7.set_ps(0); s7.tss(0, 0, 0);
        hr = send_dp2(&enc, s7, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x404040, 2) && near_(px(300, 200), 0x404040, 2), "ps 1.1 tex t0 * v0 on FVF vertices: 0x%06x 0x%06x", px(104, 84), px(300, 200));
        /* hostile: a function with no END and garbage tokens (refused, the
         * draw with it skipped), a declaration reading stream 1 (skipped), a
         * declaration wider than the stride (skipped), constants off the
         * scale (dropped), a CREATEVERTEXSHADER lying about its sizes
         * (COMMAND_UNPARSED); then the FVF path still draws */
        Dp2Buf s8;
        s8.create_vs(0x107, decl_pc, { VS11, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu });
        s8.set_vs(0x107);
        s8.draw8(4, 2, 0x107, sq);
        /* garbage with a proper END: an unknown opcode, an instruction without
         * its operands, a register off the file (DXVK's compiler asserts on
         * the first two and the process would die, so the executor's own
         * validator must refuse them first) */
        s8.create_vs(0x10f, decl_pc, { VS11, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, END });
        s8.create_ps(0x20f, { PS11, 0xDEADBEEFu, 0xDEADBEEFu, END });
        s8.set_vs(0x10f); s8.set_ps(0x20f);
        s8.draw8(4, 2, 0x10f, sq);
        s8.create_vs(0x111, decl_pc, { VS11, MOV, END });
        s8.create_ps(0x211, { PS11, MOV, dst(R_TEMP, 0), src(R_RASTOUT, 0), END });
        s8.set_vs(0x111); s8.set_ps(0x211);
        s8.draw8(4, 2, 0x111, sq);
        s8.create_vs(0x113, decl_pc, { VS11, MOV, dst(R_RASTOUT, 0), src(R_INPUT, 0), MUL, dst(R_CONST, 0), src(R_INPUT, 5), src(R_CONST, 0), END });
        s8.set_vs(0x113);
        s8.draw8(4, 2, 0x113, sq);
        s8.set_ps(0);
        s8.create_vs(0x109, { STREAM(1), REG(0, T_FLOAT4), REG(5, T_D3DCOLOR), 0xFFFFFFFFu }, vs_code);
        s8.set_vs(0x109);
        s8.draw8(4, 2, 0x109, sq);
        s8.create_vs(0x10b, { STREAM(0), REG(0, T_FLOAT4), REG(5, T_D3DCOLOR), REG(7, T_FLOAT4), 0xFFFFFFFFu }, vs_code);
        s8.set_vs(0x10b);
        s8.draw8(4, 2, 0x10b, sq);
        s8.vs_const(300, 1, 1, 1, 1);
        s8.ps_const(250, 1, 1, 1, 1);
        s8.set_vs(0x1ff);                                                     /* never created */
        s8.draw8(4, 2, 0x1ff, sq);
        s8.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        s8.set_vs(FVF_TLVERTEX);
        s8.draw8(6, 4, FVF_TLVERTEX, fan);
        hr = send_dp2(&enc, s8, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && px(100, 100) == (CLEAR_COLOR & 0xffffff) && near_(px(480, 240), 0xffffff, 2),
              "garbage function / stream 1 / wide declaration / unknown handle / wild constants: skipped, not fatal; the fan still draws (0x%06x 0x%06x)", px(100, 100), px(480, 240));
        Dp2Buf s9;
        s9.create_vs(0x10d, decl_pc, vs_code, 4096);                          /* the declaration size lies */
        hr = send_dp2(&enc, s9, vtx, 0, &err_off);
        CHECK(hr == 0x88760BB8u && err_off == 0, "CREATEVERTEXSHADER lying about its declaration size -> D3DERR_COMMAND_UNPARSED at %u (0x%08x)", err_off, hr);

        /* --- more than one vertex stream (v10): the position in stream 0,
         * the colour in another stream at its own stride; the host
         * interleaves them under the declaration --- */
        {
            typedef Dp2Buf::Strm Strm;
            struct pos4 { float x, y, z, w; };
            struct col8 { uint32_t color, pad; };                       /* a stride of 8 where the colour is 4 */
            std::vector<pos4> p4;
            for (const svtx &v : sq) p4.push_back({ v.x, v.y, v.z, v.w });
            auto cols = [](uint32_t c, size_t n) { return std::vector<col8>(n, col8{ c, 0xdeadbeefu }); };
            const uint32_t H_VS2 = 0x115, H_VS02 = 0x117, H_FF2 = 0x119;
            Dp2Buf m1;
            m1.create_vs(H_VS2, { STREAM(0), REG(0, T_FLOAT4), STREAM(1), REG(5, T_D3DCOLOR), 0xFFFFFFFFu }, vs_code);
            m1.vs_const(0, 1, 1, 1, 1);                                 /* c0 white: the colour stream alone decides */
            m1.set_vs(H_VS2);
            m1.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            m1.draw8m(4, 2, H_VS2, p4, {}, 0, { Dp2Buf::strm(1, cols(0xff00ff00u, 6)) });
            hr = send_dp2(&enc, m1, vtx);
            hr |= readback(&enc, H_RT);
            CHECK(hr == 0 && near_(px(100, 100), 0x00ff00, 2) && px(500, 400) == (CLEAR_COLOR & 0xffffff),
                  "two streams, position + colour at stride 8, inline: quad 0x%06x, outside 0x%06x", px(100, 100), px(500, 400));
            /* the colour stream in a VRAM buffer, the draw indexed with a MinIndex of 2 */
            std::vector<col8> mag = cols(0xffff00ffu, 6);
            memcpy(vram + VB_OFF + 0x1000, mag.data(), mag.size() * sizeof(col8));
            Dp2Buf m2;
            m2.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            m2.draw8m(4, 2, H_VS2, p4, { 2, 3, 4, 5, 6, 7 }, 2, { Dp2Buf::strm_vram(1, sizeof(col8), H_VB, 0x1000) });
            hr = send_dp2(&enc, m2, vtx);
            hr |= readback(&enc, H_RT);
            CHECK(hr == 0 && near_(px(100, 100), 0xff00ff, 2), "two streams, the colour in a VRAM buffer, indexed (MinIndex 2): 0x%06x", px(100, 100));
            /* a gap: streams 0 and 2 read, stream 1 not bound at all */
            Dp2Buf m3;
            m3.create_vs(H_VS02, { STREAM(0), REG(0, T_FLOAT4), STREAM(2), REG(5, T_D3DCOLOR), 0xFFFFFFFFu }, vs_code);
            m3.set_vs(H_VS02);
            m3.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            m3.draw8m(4, 2, H_VS02, p4, {}, 0, { Dp2Buf::strm(2, cols(0xff0000ffu, 6)) });
            hr = send_dp2(&enc, m3, vtx);
            hr |= readback(&enc, H_RT);
            CHECK(hr == 0 && near_(px(100, 100), 0x0000ff, 2), "streams 0 and 2 (none bound at 1): 0x%06x", px(100, 100));
            /* the fixed function on a two-stream declaration (no function), FLOAT3 positions */
            struct pos3 { float x, y, z; };
            std::vector<pos3> p3;
            for (const svtx &v : sq) p3.push_back({ v.x, v.y, v.z });
            Dp2Buf m4;
            m4.create_vs(H_FF2, { STREAM(0), REG(0, T_FLOAT3), STREAM(1), REG(5, T_D3DCOLOR), 0xFFFFFFFFu }, {});
            m4.set_vs(H_FF2);
            m4.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            m4.draw8m(4, 2, H_FF2, p3, {}, 0, { Dp2Buf::strm(1, cols(0xffffff00u, 6)) });
            hr = send_dp2(&enc, m4, vtx);
            hr |= readback(&enc, H_RT);
            CHECK(hr == 0 && near_(px(100, 100), 0xffff00, 2), "fixed function on a two-stream declaration: 0x%06x", px(100, 100));
            /* a stale stream the shader does not read goes along and is ignored:
             * a stream-0 shader with a stream 3 bound */
            Dp2Buf m5;
            m5.set_vs(H_VS);
            m5.vs_const(0, 0, 1, 1, 1);                                 /* cyan */
            m5.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            m5.draw8m(4, 2, H_VS, sq, {}, 0, { Dp2Buf::strm(3, cols(0xffff0000u, 6)) });
            hr = send_dp2(&enc, m5, vtx);
            hr |= readback(&enc, H_RT);
            CHECK(hr == 0 && near_(px(100, 100), 0x00ffff, 2), "a stream the declaration does not read: carried, ignored 0x%06x", px(100, 100));
            /* hostile: the declaration reads stream 1 and the draw carries
             * only stream 2 (stream 1 reads as zeros, as Direct3D 9 draws an
             * unbound stream); skipped: stream 1's stride shorter than what it
             * reads (a FLOAT4 colour in 4 bytes), stream 1's VRAM range beyond
             * its buffer. Then the good draw, so the frame proves the stream
             * stayed in sync */
            Dp2Buf m6;
            m6.set_vs(H_VS2);
            m6.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            m6.draw8m(4, 2, H_VS2, p4, {}, 0, { Dp2Buf::strm(2, cols(0xffff0000u, 6)) });
            Strm shortc = Dp2Buf::strm(1, std::vector<uint32_t>(6, 0xffff0000u));
            m6.create_vs(0x11b, { STREAM(0), REG(0, T_FLOAT4), STREAM(1), REG(5, T_FLOAT4), 0xFFFFFFFFu }, vs_code);
            m6.set_vs(0x11b);
            m6.draw8m(4, 2, 0x11b, p4, {}, 0, { shortc });
            m6.set_vs(H_VS2);
            m6.draw8m(4, 2, H_VS2, p4, {}, 0, { Dp2Buf::strm_vram(1, sizeof(col8), H_VB, VB_SIZE - 16) });
            m6.draw8m(4, 2, H_VS2, p4, {}, 0, { Dp2Buf::strm(1, cols(0xff00ff00u, 6)) });
            hr = send_dp2(&enc, m6, vtx);
            hr |= readback(&enc, H_RT);
            CHECK(hr == 0 && near_(px(100, 100), 0x00ff00, 2),
                  "stream missing (zeros) / too short / beyond its buffer (skipped): not fatal; the good draw after them lands 0x%06x", px(100, 100));
            /* hostile, refused (COMMAND_UNPARSED): stream 0 in the list, streams
             * out of order, a count of 16, an unknown stream flag, stream data
             * running past the record */
            struct { const char *what; std::vector<Strm> st; uint32_t count_lie; } bad[] = {
                { "stream 0 in the list", { Dp2Buf::strm(0, cols(0, 6)) }, 0 },
                { "streams out of order", { Dp2Buf::strm(2, cols(0, 6)), Dp2Buf::strm(1, cols(0, 6)) }, 0 },
                { "a count of 16", { Dp2Buf::strm(1, cols(0, 6)) }, 16 },
                { "an unknown stream flag", { Dp2Buf::strm(1, cols(0, 6)) }, 0 },
                { "a count past the record", { Dp2Buf::strm(1, cols(0, 6)) }, 3 },
            };
            bad[3].st[0].flags_lie = 0x8;
            for (auto &c : bad) {
                Dp2Buf mb;
                mb.draw8m(4, 2, H_VS2, p4, {}, 0, c.st, c.count_lie);
                hr = send_dp2(&enc, mb, vtx, 0, &err_off);
                CHECK(hr == 0x88760BB8u && err_off == 0, "DRAW8 with %s -> D3DERR_COMMAND_UNPARSED at %u (0x%08x)", c.what, err_off, hr);
            }
            /* instancing (v16): the quad indexed, three instances added up
             * (blend ONE + ONE over black), each coloured by its element of an
             * instance stream: 0x400000 + 0x004000 + 0x000040. At a divider
             * of 2 instances 0 and 1 take the first element and instance 2
             * the second: 0x400000 * 2 + 0x004000. Then
             * the hostile ones: an instance count on a draw with no indices,
             * an instance stream on a draw that is not instanced */
            {
                std::vector<uint16_t> ix = { 0, 1, 2, 3, 4, 5 };
                std::vector<col8> ic = { { 0xff400000u, 0 }, { 0xff004000u, 0 }, { 0xff000040u, 0 } };
                uint32_t sums[2];
                for (int dv = 1; dv <= 2; dv++) {
                    /* ceil(3 / divider) elements, as the driver sends them */
                    Strm is = Dp2Buf::strm(1, std::vector<col8>(ic.begin(), ic.begin() + (3 + dv - 1) / dv));
                    is.freq = D3DSTREAMSOURCE_INSTANCEDATA | (uint32_t)dv;
                    Dp2Buf mi;
                    mi.set_vs(H_VS2);
                    mi.vs_const(0, 1, 1, 1, 1);
                    mi.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f);
                    mi.rs(D3DRS_ALPHABLENDENABLE, 1); mi.rs(D3DRS_SRCBLEND, D3DBLEND_ONE); mi.rs(D3DRS_DESTBLEND, D3DBLEND_ONE);
                    mi.rs(D3DRS_ZENABLE, 0);
                    mi.draw8m(4, 2, H_VS2, p4, ix, 0, { is }, 0, D3DSTREAMSOURCE_INDEXEDDATA | 3);
                    mi.rs(D3DRS_ALPHABLENDENABLE, 0); mi.rs(D3DRS_ZENABLE, 1);
                    hr = send_dp2(&enc, mi, vtx);
                    hr |= readback(&enc, H_RT);
                    sums[dv - 1] = hr ? 0xdeadbe : px(100, 100);
                }
                CHECK(near_(sums[0], 0x404040, 2) && near_(sums[1], 0x804000, 2),
                      "three instances, an instance colour each: 0x%06x (want 0x404040), at a divider of 2 0x%06x (want 0x804000)", sums[0], sums[1]);
                Strm is = Dp2Buf::strm(1, ic);
                is.freq = D3DSTREAMSOURCE_INSTANCEDATA | 1;
                Dp2Buf h1;
                h1.draw8m(4, 2, H_VS2, p4, {}, 0, { Dp2Buf::strm(1, cols(0, 6)) }, 0, D3DSTREAMSOURCE_INDEXEDDATA | 3);
                hr = send_dp2(&enc, h1, vtx, 0, &err_off);
                CHECK(hr == 0x88760BB8u && err_off == 0, "an instance count on a draw with no indices -> D3DERR_COMMAND_UNPARSED at %u (0x%08x)", err_off, hr);
                Dp2Buf h2;
                h2.draw8m(4, 2, H_VS2, p4, ix, 0, { is });
                hr = send_dp2(&enc, h2, vtx, 0, &err_off);
                CHECK(hr == 0x88760BB8u && err_off == 0, "an instance stream on a draw not instanced -> D3DERR_COMMAND_UNPARSED at %u (0x%08x)", err_off, hr);
            }
            Dp2Buf m7;
            m7.delete_vs(H_VS2); m7.delete_vs(H_VS02); m7.delete_vs(H_FF2); m7.delete_vs(0x11b);
            hr = send_dp2(&enc, m7, vtx);
            CHECK(hr == 0, "multi-stream shaders deleted (0x%08x)", hr);
        }
        Dp2Buf s10;
        s10.delete_vs(H_VS); s10.delete_vs(H_VS_FF); s10.delete_vs(H_VS_CONST); s10.delete_vs(0x109); s10.delete_vs(0x10b);
        s10.delete_ps(H_PS); s10.delete_ps(H_PS_TEX); s10.delete_ps(0x2ff);
        s10.set_vs(FVF_TLVERTEX);
        s10.draw8(6, 4, FVF_TLVERTEX, fan);
        hr = send_dp2(&enc, s10, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(480, 240), 0xffffff, 2), "shaders deleted, the fan still draws (0x%06x)", px(480, 240));
    }

    /* --- palettized textures and colour keying (v8) --- */
    {
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6);
        /* a P8 texture: index = (x / 8 + y / 8) & 3, so the quad's cells cycle
         * through four palette entries; a R5G6B5 checker of blue / white */
        for (int y = 0; y < TEX; y++)
            for (int x = 0; x < TEX; x++) {
                vram[TEXP8_OFF + y * TEXP8_PITCH + x] = (uint8_t)(((x / 8) + (y / 8)) & 3);
                uint16_t c = ((x / 8) + (y / 8)) & 1 ? 0xffff : 0x001f;
                memcpy(vram + TEX16_OFF + y * TEX16_PITCH + x * 2, &c, 2);
            }
        vram_surface(&enc, H_TEXP8, TEXP8_OFF, TEX, TEX, TEXP8_PITCH, D3DFMT_P8, D3DPT_VS_TEXTURE);
        vram_surface(&enc, H_TEX16, TEX16_OFF, TEX, TEX, TEX16_PITCH, D3DFMT_R5G6B5, D3DPT_VS_TEXTURE);
        Dp2Buf p1;
        std::vector<uint32_t> pal(256, 0);
        pal[0] = 0xffff0000u; pal[1] = 0xff00ff00u; pal[2] = 0xff0000ffu; pal[3] = 0xffffff00u;
        p1.update_palette(77, 0, pal);
        p1.set_palette(77, 1, H_TEXP8);
        p1.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        p1.tss(0, 0, H_TEXP8); p1.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); p1.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        p1.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); p1.tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
        p1.tss(0, 16, 1); p1.tss(0, 17, 1); p1.tss(0, 18, 1);
        p1.set_vs(FVF_TLVERTEX);
        p1.draw8(4, 2, FVF_TLVERTEX, quad);
        hr = send_dp2(&enc, p1, vtx);
        hr |= readback(&enc, H_RT);
        /* the quad maps 2 texels per 2.5 px: cell 0 at (104, 84), cell 1 at (124, 84), cell 2 at (144, 84), cell 3 at (164, 84) */
        CHECK(hr == 0 && near_(px(104, 84), 0xff0000, 2) && near_(px(124, 84), 0x00ff00, 2) && near_(px(144, 84), 0x0000ff, 2) && near_(px(164, 84), 0xffff00, 2),
              "P8 texture through its palette: 0x%06x 0x%06x 0x%06x 0x%06x", px(104, 84), px(124, 84), px(144, 84), px(164, 84));
        Dp2Buf p2;
        p2.update_palette(77, 1, { 0xff00ffffu });                          /* entry 1 becomes cyan: the texture is re-expanded */
        p2.draw8(4, 2, FVF_TLVERTEX, quad);
        hr = send_dp2(&enc, p2, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(124, 84), 0x00ffff, 2) && near_(px(104, 84), 0xff0000, 2), "UPDATEPALETTE changes the texels: 0x%06x 0x%06x", px(124, 84), px(104, 84));
        /* colour key: blue (0x001f) keyed on the 16-bit checker; render state
         * 41 on -> the blue cells show the clear colour, off -> blue again */
        { d3dpt_u32x4 *k = (d3dpt_u32x4 *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_COLORKEY, sizeof *k, 0); *k = { H_TEX16, 0x001f, 0x001f, 1 }; }
        Dp2Buf k1;
        k1.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        k1.rs(41, 1);
        k1.tss(0, 0, H_TEX16);
        k1.draw8(4, 2, FVF_TLVERTEX, quad);
        hr = send_dp2(&enc, k1, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && px(104, 84) == (CLEAR_COLOR & 0xffffff) && near_(px(124, 84), 0xffffff, 2),
              "colour-keyed R5G6B5 texture, COLORKEYENABLE on: keyed cell 0x%06x, other 0x%06x", px(104, 84), px(124, 84));
        Dp2Buf k2;
        k2.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        k2.rs(41, 0);
        k2.draw8(4, 2, FVF_TLVERTEX, quad);
        hr = send_dp2(&enc, k2, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x0000ff, 2), "COLORKEYENABLE off: the keyed cell draws again 0x%06x", px(104, 84));
        /* the app's own alpha test survives the key: ALPHATESTENABLE on with
         * ALWAYS, key on -> the keyed cell still draws (the app's test wins),
         * then the key removed and the alpha test off */
        Dp2Buf k3;
        k3.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        k3.rs(15, 1); k3.rs(25, D3DCMP_ALWAYS); k3.rs(41, 1);
        k3.draw8(4, 2, FVF_TLVERTEX, quad);
        k3.rs(15, 0); k3.rs(41, 0);
        hr = send_dp2(&enc, k3, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x0000ff, 2), "the app's own alpha test (ALWAYS) wins over the key: 0x%06x", px(104, 84));
        { d3dpt_u32x4 *k = (d3dpt_u32x4 *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_COLORKEY, sizeof *k, 0); *k = { H_TEX16, 0, 0, 0 }; }
        Dp2Buf k4;
        k4.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        k4.rs(41, 1);
        k4.draw8(4, 2, FVF_TLVERTEX, quad);
        k4.rs(41, 0); k4.tss(0, 0, 0);
        hr = send_dp2(&enc, k4, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x0000ff, 2), "key removed: the cell draws with COLORKEYENABLE on 0x%06x", px(104, 84));
        /* hostile: a palette beyond 256 entries, a SETPALETTE on an unknown surface */
        Dp2Buf p3; p3.update_palette(78, 250, std::vector<uint32_t>(10, 0));
        hr = send_dp2(&enc, p3, vtx, 0, &err_off);
        CHECK(hr == 0x88760BB8u, "UPDATEPALETTE beyond 256 entries -> D3DERR_COMMAND_UNPARSED (0x%08x)", hr);
        Dp2Buf p4; p4.set_palette(77, 1, 999); p4.draw8(6, 4, FVF_TLVERTEX, std::vector<tlv>(vtx.begin() + 6, vtx.begin() + 12));
        hr = send_dp2(&enc, p4, vtx);
        CHECK(hr == 0, "SETPALETTE on an unknown surface: ignored (0x%08x)", hr);
    }

    /* --- the rest of DX8's texture formats (FMTTEST's, without the DXTs):
     * each a 64x64 texture of one texel, drawn with its colour and then
     * with its alpha replicated (D3DTA_ALPHAREPLICATE); whichever of them
     * this host's DXVK lacks (R3G3B2 / A8R3G3B2 always) goes through the
     * A8R8G8B8 expansion, so both paths are asked on every host --- */
    {
        enum { FMT_OFF = 0x340000, FMT_STRIDE = 0x2000, H_FMT = 300 };
        struct { uint32_t fmt; const char *name; uint32_t bytes, texel, rgb, alpha; bool has_rgb; } fc[] = {
            { D3DFMT_L8, "L8", 1, 0x80, 0x808080, 0xffffff, true },
            { D3DFMT_A8L8, "A8L8", 2, 0x40c0, 0xc0c0c0, 0x404040, true },
            { D3DFMT_A4L4, "A4L4", 1, 0x4c, 0xcccccc, 0x444444, true },
            { D3DFMT_A8, "A8", 1, 0x60, 0x000000, 0x606060, false },
            { D3DFMT_X4R4G4B4, "X4R4G4B4", 2, 0x0f84, 0xff8844, 0xffffff, true },
            { D3DFMT_R3G3B2, "R3G3B2", 1, 0xe3, 0xff00ff, 0xffffff, true },
            { D3DFMT_A8R3G3B2, "A8R3G3B2", 2, 0x40e3, 0xff00ff, 0x404040, true },
        };
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6);
        for (uint32_t i = 0; i < sizeof fc / sizeof fc[0]; i++) {
            uint32_t off = FMT_OFF + i * FMT_STRIDE, pitch = TEX * fc[i].bytes, got[2];
            for (int y = 0; y < TEX; y++)
                for (int x = 0; x < TEX; x++) memcpy(vram + off + y * pitch + x * fc[i].bytes, &fc[i].texel, fc[i].bytes);
            vram_surface(&enc, H_FMT + i, off, TEX, TEX, pitch, fc[i].fmt, D3DPT_VS_TEXTURE);
            for (int a = 0; a < 2; a++) {
                Dp2Buf f;
                f.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
                f.tss(0, 0, H_FMT + i); f.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                f.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE | (a ? D3DTA_ALPHAREPLICATE : 0));
                f.draw8(4, 2, FVF_TLVERTEX, quad);
                hr = send_dp2(&enc, f, vtx);
                hr |= readback(&enc, H_RT);
                got[a] = hr ? 0xdeadbe : px(124, 84);
            }
            CHECK((!fc[i].has_rgb || near_(got[0], fc[i].rgb, 2)) && near_(got[1], fc[i].alpha, 2),
                  "%s texture: colour 0x%06x (want 0x%06x), alpha replicated 0x%06x (want 0x%06x)", fc[i].name, got[0], fc[i].rgb, got[1], fc[i].alpha);
        }
        Dp2Buf r; r.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); r.tss(0, 0, 0);
        send_dp2(&enc, r, vtx);
    }

    /* --- an autogen texture (v15, M16): 64x64, its left half red and its
     * right half green in VRAM, level 0 alone; a one-pixel quad point-sampled
     * with point mips reads the 1x1 level the host made, the two halves
     * averaged.
     * The texels rewritten blue, VRAM_DIRTY and GENERATEMIPSUBLEVELS make the
     * small quad blue. Then the hostile ones: the token on a texture that is
     * no autogen one and on an unknown handle is skipped, and an autogen
     * record with two levels or as a cube is refused --- */
    {
        enum { GEN_OFF = 0x368000, H_GEN = 330 };
        auto fill = [&](uint32_t left, uint32_t right) {
            for (int y = 0; y < TEX; y++)
                for (int x = 0; x < TEX; x++) memcpy(vram + GEN_OFF + y * TEX * 4 + x * 4, x < TEX / 2 ? &left : &right, 4);
        };
        fill(0xffff0000u, 0xff00ff00u);
        vram_surface(&enc, H_GEN, GEN_OFF, TEX, TEX, TEX * 4, D3DFMT_A8R8G8B8, D3DPT_VS_TEXTURE | D3DPT_VS_AUTOGEN);
        std::vector<tlv> small = { { 300, 300, 0.5f, 1, ~0u, 0, 0, 0 }, { 301, 300, 0.5f, 1, ~0u, 0, 1, 0 }, { 300, 301, 0.5f, 1, ~0u, 0, 0, 1 },
                                   { 300, 301, 0.5f, 1, ~0u, 0, 0, 1 }, { 301, 300, 0.5f, 1, ~0u, 0, 1, 0 }, { 301, 301, 0.5f, 1, ~0u, 0, 1, 1 } };
        auto draw = [&](Dp2Buf &g) {
            g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            g.tss(0, 0, H_GEN); g.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); g.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            g.tss(0, 16, 1); g.tss(0, 17, 1); g.tss(0, 18, 2);          /* point sampling, point mips */
            g.draw8(4, 2, FVF_TLVERTEX, small);
        };
        Dp2Buf g1;
        draw(g1);
        hr = send_dp2(&enc, g1, vtx);
        hr |= readback(&enc, H_RT);
        uint32_t mixed = hr ? 0xdeadbe : px(300, 300);
        fill(0xff0000ffu, 0xff0000ffu);
        { d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_DIRTY, sizeof *hh, 0); *hh = { H_GEN, 0 }; }
        Dp2Buf g2;
        g2.cmd(89, 1); g2.u32(H_GEN); g2.u32(D3DTEXF_LINEAR);
        draw(g2);
        hr = send_dp2(&enc, g2, vtx);
        hr |= readback(&enc, H_RT);
        uint32_t blue = hr ? 0xdeadbe : px(300, 300);
        CHECK(near_(mixed, 0x7f7f00, 3) && near_(blue, 0x0000ff, 2),
              "autogen texture: one pixel of its 64x64 0x%06x (want 0x7f7f00, the host's 1x1 level), after GENERATEMIPSUBLEVELS 0x%06x (want 0x0000ff)", mixed, blue);
        Dp2Buf g3;
        g3.cmd(89, 2); g3.u32(H_TEX); g3.u32(D3DTEXF_LINEAR); g3.u32(0x7777); g3.u32(D3DTEXF_POINT);
        hr = send_dp2(&enc, g3, vtx);
        CHECK(hr == 0, "GENERATEMIPSUBLEVELS on a plain texture and an unknown handle skipped (0x%08x)", hr);
        d3dpt_vram_surface *two = (d3dpt_vram_surface *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_SURFACE, sizeof *two, sizeof(d3dpt_u32x2));
        *two = { H_GEN + 1, GEN_OFF, TEX, TEX, TEX * 4, D3DFMT_A8R8G8B8, D3DPT_VS_TEXTURE | D3DPT_VS_AUTOGEN, 2 };
        *(d3dpt_u32x2 *)(two + 1) = { GEN_OFF + TEX * TEX * 4, TEX * 2 };
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "autogen texture with two levels refused (status %u)", enc.last_status);
        vram_surface(&enc, H_GEN + 1, GEN_OFF, TEX, TEX, TEX * 4, D3DFMT_A8R8G8B8, D3DPT_VS_AUTOGEN);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "autogen without the texture cap refused (status %u)", enc.last_status);
        Dp2Buf r; r.tss(0, 0, 0); r.tss(0, 18, 1);
        send_dp2(&enc, r, vtx);
    }

    /* --- several render targets (v17): a second 640x480 target at index 1
     * (the runtime's SETRENDERTARGET2), a ps 2.0 writing c0 to oC0 and c1
     * to oC1: red in the first target, green in the second, each read
     * back. Then target 1 unbound, and the hostile ones: index 0 and 4
     * refused, a texture that is no target unbound with a line --- */
    {
        enum { H_MRT = 340, H_PS_MRT = 0x241 };
        const uint32_t ps_mrt[] = { 0xFFFF0200u, 0x02000001u, 0x800F0800u, 0xA0E40000u,    /* mov oC0, c0 */
                                    0x02000001u, 0x800F0801u, 0xA0E40001u, 0x0000FFFFu };  /* mov oC1, c1 */
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6);
        memset(vram + RT2_OFF, 0, (size_t)RT_PITCH * H);
        vram_surface(&enc, H_MRT, RT2_OFF, W, H, RT_PITCH, D3DFMT_X8R8G8B8, D3DPT_VS_RENDER_TARGET);
        Dp2Buf m;
        m.create_ps(H_PS_MRT, std::vector<uint32_t>(ps_mrt, ps_mrt + 8));
        m.set_ps(H_PS_MRT);
        m.ps_const(0, 1, 0, 0, 1);
        m.ps_const(1, 0, 1, 0, 1);
        m.cmd(85, 1); m.u32(1); m.u32(H_MRT);
        m.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        m.draw8(4, 2, FVF_TLVERTEX, quad);
        m.cmd(85, 1); m.u32(1); m.u32(0);
        m.set_ps(0);
        hr = send_dp2(&enc, m, vtx);
        hr |= readback(&enc, H_RT);
        hr |= readback(&enc, H_MRT);
        uint32_t c0 = hr ? 0xdeadbe : px(124, 84), c1;
        memcpy(&c1, vram + RT2_OFF + 84 * RT_PITCH + 124 * 4, 4);
        c1 &= 0xffffff;
        uint32_t cleared;
        memcpy(&cleared, vram + RT2_OFF + 400 * RT_PITCH + 600 * 4, 4);
        CHECK(near_(c0, 0xff0000, 2) && near_(c1, 0x00ff00, 2) && (cleared & 0xffffff) == (CLEAR_COLOR & 0xffffff),
              "two render targets: oC0 0x%06x (want 0xff0000), oC1 0x%06x (want 0x00ff00), the second cleared 0x%06x", c0, c1, cleared & 0xffffff);
        for (uint32_t bad : { 0u, 4u }) {
            Dp2Buf mb;
            mb.cmd(85, 1); mb.u32(bad); mb.u32(H_MRT);
            hr = send_dp2(&enc, mb, vtx, 0, &err_off);
            CHECK(hr == 0x88760BB8u && err_off == 0, "SETRENDERTARGET2 index %u -> D3DERR_COMMAND_UNPARSED at %u (0x%08x)", bad, err_off, hr);
        }
        Dp2Buf mt;
        mt.cmd(85, 1); mt.u32(1); mt.u32(H_TEX);
        mt.clear(D3DCLEAR_TARGET, CLEAR_COLOR, 1.0f);
        mt.cmd(85, 1); mt.u32(1); mt.u32(0);
        hr = send_dp2(&enc, mt, vtx);
        CHECK(hr == 0, "a texture as target 1: unbound, not fatal (0x%08x)", hr);
        { d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_RELEASE, sizeof *hh, 0); *hh = { H_MRT, 0 }; }
        Dp2Buf md; md.delete_ps(H_PS_MRT);
        send_dp2(&enc, md, vtx);
    }

    /* --- StretchRect between two depth buffers (v18): on a 64x64 target, a
     * second D16 buffer cleared to 0 is blitted over the bound one (cleared
     * to 1), so a white quad at z 0.5 fails the depth test and the target
     * stays black; the same quad after the bound buffer is cleared to 1
     * again lands. Then a BLT that names no depth buffer is dropped --- */
    {
        enum { DRT_OFF = 0x370000, DZA_OFF = 0x374000, DZB_OFF = 0x376000, H_DRT = 350, H_DZA = 351, H_DZB = 352 };
        vram_surface(&enc, H_DRT, DRT_OFF, 64, 64, 256, D3DFMT_X8R8G8B8, D3DPT_VS_RENDER_TARGET);
        vram_surface(&enc, H_DZA, DZA_OFF, 64, 64, 128, D3DFMT_D16, D3DPT_VS_ZBUFFER);
        vram_surface(&enc, H_DZB, DZB_OFF, 64, 64, 128, D3DFMT_D16, D3DPT_VS_ZBUFFER);
        std::vector<tlv> dq = { { 0, 0, 0.5f, 1, ~0u, 0, 0, 0 }, { 64, 0, 0.5f, 1, ~0u, 0, 0, 0 }, { 0, 64, 0.5f, 1, ~0u, 0, 0, 0 },
                                { 0, 64, 0.5f, 1, ~0u, 0, 0, 0 }, { 64, 0, 0.5f, 1, ~0u, 0, 0, 0 }, { 64, 64, 0.5f, 1, ~0u, 0, 0, 0 } };
        auto blt = [](Dp2Buf &g, uint32_t src, uint32_t dst) {
            g.cmd(81, 1); g.u32(src); g.u32(0); g.u32(0); g.u32(64); g.u32(64); g.u32(0);
            g.u32(dst); g.u32(0); g.u32(0); g.u32(64); g.u32(64); g.u32(0); g.u32(0);
        };
        auto at = [&]() { uint32_t v; memcpy(&v, vram + DRT_OFF + 32 * 256 + 32 * 4, 4); return v & 0xffffff; };
        uint32_t got[2];
        for (int k = 0; k < 2; k++) {
            Dp2Buf g;
            g.cmd(41, 1); g.u32(H_DRT); g.u32(H_DZB);
            g.viewport(0, 0, 64, 64);
            g.clear(D3DCLEAR_ZBUFFER, 0, 0.0f);
            g.cmd(41, 1); g.u32(H_DRT); g.u32(H_DZA);
            g.viewport(0, 0, 64, 64);
            g.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f);
            if (k == 0) blt(g, H_DZB, H_DZA);
            g.rs(D3DRS_ZENABLE, 1); g.rs(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
            g.tss(0, 0, 0); g.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); g.tss(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            g.draw8(4, 2, FVF_TLVERTEX, dq);
            g.cmd(41, 1); g.u32(H_RT); g.u32(H_Z);
            g.viewport(0, 0, W, H);
            hr = send_dp2(&enc, g, vtx);
            hr |= readback(&enc, H_DRT);
            got[k] = hr ? 0xdeadbe : at();
        }
        CHECK(got[0] == 0 && got[1] == 0xffffff,
              "depth StretchRect: a quad behind the copied depth 0x%06x (want 0), without the copy 0x%06x (want 0xffffff)", got[0], got[1]);
        Dp2Buf bb;
        blt(bb, H_RT, H_DZA);
        hr = send_dp2(&enc, bb, vtx);
        CHECK(hr == 0, "a BLT from a colour target to a depth buffer dropped, not fatal (0x%08x)", hr);
        for (uint32_t h : { (uint32_t)H_DRT, (uint32_t)H_DZA, (uint32_t)H_DZB }) {
            d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_RELEASE, sizeof *hh, 0);
            *hh = { h, 0 };
        }
        Dp2Buf r; r.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        send_dp2(&enc, r, vtx);
    }

    /* --- a render-target texture's level under its own handle (v19): a
     * 64x64 target texture of two levels, level 1 linked as handle 361;
     * a SETRENDERTARGET to 361 and a clear to green land in level 1 (read
     * back at its offset), and a one-pixel quad sampling the texture with
     * point mips reads that green. Hostile: level 0, a level past the
     * texture, a texture that is no target --- */
    {
        enum { MT_OFF = 0x378000, H_MT = 360, H_ML = 361 };
        const uint32_t l1_off = MT_OFF + 64 * 64 * 4;
        memset(vram + MT_OFF, 0, 64 * 64 * 4 + 32 * 32 * 4);
        {
            d3dpt_vram_surface *t = (d3dpt_vram_surface *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_SURFACE, sizeof *t, sizeof(d3dpt_u32x2));
            *t = { H_MT, MT_OFF, 64, 64, 256, D3DFMT_X8R8G8B8, D3DPT_VS_TEXTURE | D3DPT_VS_RENDER_TARGET, 2 };
            *(d3dpt_u32x2 *)(t + 1) = { l1_off, 128 };
            d3dpt_u32x4 *ml = (d3dpt_u32x4 *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_MIP_LEVEL, sizeof *ml, 0);
            *ml = { H_ML, H_MT, 1, 0 };
        }
        Dp2Buf g;
        g.cmd(41, 1); g.u32(H_ML); g.u32(0);
        g.viewport(0, 0, 32, 32);
        g.clear(D3DCLEAR_TARGET, 0xff00ff00u, 1.0f);
        g.cmd(41, 1); g.u32(H_RT); g.u32(H_Z);
        g.viewport(0, 0, W, H);
        hr = send_dp2(&enc, g, vtx);
        hr |= readback(&enc, H_ML);
        uint32_t lv1;
        memcpy(&lv1, vram + l1_off + 16 * 128 + 16 * 4, 4);
        lv1 = hr ? 0xdeadbe : lv1 & 0xffffff;
        std::vector<tlv> one = { { 300, 300, 0.5f, 1, ~0u, 0, 0, 0 }, { 301, 300, 0.5f, 1, ~0u, 0, 1, 0 }, { 300, 301, 0.5f, 1, ~0u, 0, 0, 1 },
                                 { 300, 301, 0.5f, 1, ~0u, 0, 0, 1 }, { 301, 300, 0.5f, 1, ~0u, 0, 1, 0 }, { 301, 301, 0.5f, 1, ~0u, 0, 1, 1 } };
        Dp2Buf q;
        q.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        q.rs(D3DRS_ZENABLE, 0);
        q.tss(0, 0, H_MT); q.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); q.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        q.tss(0, 16, 1); q.tss(0, 17, 1); q.tss(0, 18, 2);
        q.draw8(4, 2, FVF_TLVERTEX, one);                 /* 64 texels a pixel: the last level, 1 */
        q.tss(0, 0, 0); q.tss(0, 18, 1); q.rs(D3DRS_ZENABLE, 1);
        hr = send_dp2(&enc, q, vtx);
        hr |= readback(&enc, H_RT);
        uint32_t sampled = hr ? 0xdeadbe : px(300, 300);
        CHECK(lv1 == 0x00ff00 && near_(sampled, 0x00ff00, 2),
              "a target texture's level 1 by its own handle: cleared 0x%06x (want 0x00ff00), sampled 0x%06x (want 0x00ff00)", lv1, sampled);
        struct { uint32_t lh, th, lvl; const char *what; } bad[] = {
            { 362, H_MT, 0, "level 0" }, { 362, H_MT, 2, "a level past the texture" }, { 362, H_TEX, 1, "a texture that is no target" } };
        for (auto &c : bad) {
            d3dpt_u32x4 *ml = (d3dpt_u32x4 *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_MIP_LEVEL, sizeof *ml, 0);
            *ml = { c.lh, c.th, c.lvl, 0 };
            d3dpt_enc_flush(&enc);
            CHECK(enc.last_status == D3DPT_ERR_BAD_ARG || enc.last_status == D3DPT_ERR_BAD_HANDLE, "MIP_LEVEL with %s refused (status %u)", c.what, enc.last_status);
        }
        for (uint32_t h : { (uint32_t)H_ML, (uint32_t)H_MT }) {
            d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_RELEASE, sizeof *hh, 0);
            *hh = { h, 0 };
        }
        Dp2Buf r; r.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        send_dp2(&enc, r, vtx);
    }

    /* --- the luminance bump maps (BUMPENVMAPLUMINANCE): a uniform bump map
     * (du = dv = 0, L = one half) at stage 0 over a uniform green
     * environment map at stage 1, a zero bump matrix and a luminance scale
     * of 1: BUMPENVMAP draws the green as it is, BUMPENVMAPLUMINANCE at
     * half, for X8L8V8U8 and L6V5U5 (both converted by DXVK at upload).
     * DXVK's ubershader never applied the luminance until our
     * patches/dxvk/07 (full green both times) --- */
    {
        enum { BUMP_OFF = 0x350000, ENV_OFF = 0x354000, BUMP16_OFF = 0x358000, H_BUMP = 320, H_ENV = 321, H_BUMP16 = 322 };
        struct { uint32_t fmt, handle, off, bytes, texel; const char *name; } lb[] = {
            { D3DFMT_X8L8V8U8, H_BUMP, BUMP_OFF, 4, 0x00800000u, "X8L8V8U8" },      /* L 0x80 in bits 16..23 */
            { D3DFMT_L6V5U5, H_BUMP16, BUMP16_OFF, 2, 0x8000u, "L6V5U5" },           /* L 32/63 in bits 10..15 */
        };
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6);
        for (int y = 0; y < TEX; y++)
            for (int x = 0; x < TEX; x++) {
                uint32_t e = 0xff00ff00u;
                memcpy(vram + ENV_OFF + y * TEX * 4 + x * 4, &e, 4);
                for (auto &m : lb) memcpy(vram + m.off + y * TEX * m.bytes + x * m.bytes, &m.texel, m.bytes);
            }
        for (auto &m : lb) vram_surface(&enc, m.handle, m.off, TEX, TEX, TEX * m.bytes, m.fmt, D3DPT_VS_TEXTURE);
        vram_surface(&enc, H_ENV, ENV_OFF, TEX, TEX, TEX * 4, D3DFMT_A8R8G8B8, D3DPT_VS_TEXTURE);
        float one = 1.0f, zero = 0.0f;
        uint32_t one_bits, zero_bits;
        memcpy(&one_bits, &one, 4);
        memcpy(&zero_bits, &zero, 4);
        for (auto &m : lb) {
        uint32_t got[2];
        for (int l = 0; l < 2; l++) {
            Dp2Buf b;
            b.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            b.tss(0, 0, m.handle);
            b.tss(0, D3DTSS_COLOROP, l ? D3DTOP_BUMPENVMAPLUMINANCE : D3DTOP_BUMPENVMAP);
            b.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE); b.tss(0, D3DTSS_COLORARG2, D3DTA_CURRENT);
            b.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); b.tss(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
            b.tss(0, D3DTSS_BUMPENVMAT00, zero_bits); b.tss(0, D3DTSS_BUMPENVMAT01, zero_bits);
            b.tss(0, D3DTSS_BUMPENVMAT10, zero_bits); b.tss(0, D3DTSS_BUMPENVMAT11, zero_bits);
            b.tss(0, D3DTSS_BUMPENVLSCALE, one_bits); b.tss(0, D3DTSS_BUMPENVLOFFSET, zero_bits);
            b.tss(1, 0, H_ENV);
            b.tss(1, D3DTSS_COLOROP, D3DTOP_SELECTARG1); b.tss(1, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            b.tss(1, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); b.tss(1, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            b.tss(1, D3DTSS_TEXCOORDINDEX, 0);
            b.tss(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
            b.draw8(4, 2, FVF_TLVERTEX, quad);
            hr = send_dp2(&enc, b, vtx);
            hr |= readback(&enc, H_RT);
            got[l] = hr ? 0xdeadbe : px(124, 84);
        }
        CHECK(near_(got[0], 0x00ff00, 3) && near_(got[1], 0x008000, 3),
              "%s bump map: BUMPENVMAP 0x%06x (want 0x00ff00), BUMPENVMAPLUMINANCE at L 1/2 0x%06x (want 0x008000)", m.name, got[0], got[1]);
        }
        Dp2Buf r;
        r.tss(1, 0, 0); r.tss(1, D3DTSS_COLOROP, D3DTOP_DISABLE); r.tss(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        r.tss(0, 0, 0); r.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); r.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        send_dp2(&enc, r, vtx);
    }

    /* --- a multisampled render target (v13): a 64x64 target and a D16 depth
     * buffer with 4 samples (D3DPT_VS_SAMPLES), a white triangle with a
     * slanted edge on black, read back into the target's own VRAM through
     * the host's resolve. MULTISAMPLEANTIALIAS on, pixels along the edge are
     * between black and white; off, none are --- */
    {
        enum { MS_OFF = 0x360000, MSZ_OFF = 0x364000, MS_E = 64, H_MS = 330, H_MSZ = 331 };
        const uint32_t ms4 = 4u << D3DPT_VS_SAMPLES_SHIFT;
        vram_surface(&enc, H_MS, MS_OFF, MS_E, MS_E, MS_E * 4, D3DFMT_X8R8G8B8, D3DPT_VS_RENDER_TARGET | ms4);
        vram_surface(&enc, H_MSZ, MSZ_OFF, MS_E, MS_E, MS_E * 2, D3DFMT_D16, D3DPT_VS_ZBUFFER | ms4);
        std::vector<tlv> tri = { V(4, 4, 0.5f, 0xffffffff, 0, 0), V(60, 4, 0.5f, 0xffffffff, 0, 0), V(4, 56, 0.5f, 0xffffffff, 0, 0) };
        auto blended = [&](int *rows) {
            int n = 0;
            *rows = 0;
            for (int y = 8; y < 52; y += 4) {
                int k = 0;
                for (int x = 0; x < MS_E; x++) {
                    uint32_t r = vram[MS_OFF + y * MS_E * 4 + x * 4 + 2];
                    if (r > 0x20 && r < 0xe0) k++;
                }
                if (k) (*rows)++;
                n += k;
            }
            return n;
        };
        int got[2], rows[2];
        for (int aa = 1; aa >= 0; aa--) {
            Dp2Buf m;
            m.cmd(41, 1); m.u32(H_MS); m.u32(H_MSZ);
            m.viewport(0, 0, MS_E, MS_E);
            m.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xff000000u, 1.0f);
            m.rs(D3DRS_ZENABLE, 0); m.rs(D3DRS_MULTISAMPLEANTIALIAS, aa);
            m.tss(0, 0, 0); m.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2); m.tss(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
            m.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2); m.tss(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
            m.draw8(4, 1, FVF_TLVERTEX, tri);
            hr = send_dp2(&enc, m, vtx);
            hr |= readback(&enc, H_MS);
            got[aa] = hr ? -1 : blended(&rows[aa]);
        }
        uint32_t inside, outside;
        memcpy(&inside, vram + MS_OFF + 10 * MS_E * 4 + 10 * 4, 4);
        memcpy(&outside, vram + MS_OFF + 56 * MS_E * 4 + 56 * 4, 4);
        CHECK(got[1] > 0 && rows[1] >= 10 && got[0] == 0 && (inside & 0xffffff) == 0xffffff && (outside & 0xffffff) == 0,
              "4 samples: %d blended edge pixels on %d of 11 rows with MULTISAMPLEANTIALIAS, %d without (inside 0x%06x, outside 0x%06x)",
              got[1], rows[1], got[0], inside & 0xffffff, outside & 0xffffff);
        Dp2Buf r;
        r.cmd(41, 1); r.u32(H_RT); r.u32(H_Z);
        r.viewport(0, 0, W, H);
        r.rs(D3DRS_MULTISAMPLEANTIALIAS, 1); r.rs(D3DRS_ZENABLE, 1);
        send_dp2(&enc, r, vtx);
    }

    /* --- cube textures (v11): a 16-texel cube of two levels in VRAM, every
     * face and level its own colour, drawn with 3D texture coordinates at
     * each face's direction and minified onto a small quad (level 1); a
     * face's VRAM rewritten and marked dirty under the face's own handle; a
     * render-target cube drawn into face by face and then sampled; hostile
     * records --- */
    {
        enum { CUBE_OFF = 0x5a0000, RTC_OFF = 0x5b0000, H_CUBE = 20, H_RTC = 30, EDGE = 16, RTE = 32, FSTRIDE = 0x800 };
        static const uint32_t C0[6] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffff00u, 0xffff00ffu, 0xff00ffffu };
        static const uint32_t C1[6] = { 0xff800000u, 0xff008000u, 0xff000080u, 0xff808000u, 0xff800080u, 0xff008080u };
        static const float DIR[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
        auto lvl_off = [](uint32_t edge, uint32_t l) { uint32_t o = 0; for (uint32_t k = 0; k < l; k++) o += (edge >> k) * (edge >> k) * 4; return o; };
        auto fill = [](uint32_t off, uint32_t n, uint32_t c) { for (uint32_t i = 0; i < n; i++) memcpy(vram + off + 4 * i, &c, 4); };
        /* a cube's record: face f at off0 + f * fstride, its levels one after the other (32-bit texels) */
        auto cube_rec = [&](uint32_t handle, uint32_t off0, uint32_t edge, uint32_t levels, uint32_t fmt, uint32_t caps, uint32_t fstride,
                            uint32_t height = 0, uint32_t short_by = 0) {
            uint32_t nlv = 6 * levels - 1 - short_by;
            d3dpt_vram_surface *s = (d3dpt_vram_surface *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_SURFACE, sizeof *s, nlv * sizeof(d3dpt_u32x2));
            *s = { handle, off0, edge, height ? height : edge, edge * 4, fmt, caps | D3DPT_VS_CUBE, levels };
            d3dpt_u32x2 *lv = (d3dpt_u32x2 *)(s + 1);
            for (uint32_t i = 1; i <= nlv; i++) lv[i - 1] = { off0 + (i / levels) * fstride + lvl_off(edge, i % levels), (edge >> (i % levels)) * 4 };
        };
        auto face_rec = [&](uint32_t face_h, uint32_t cube_h, uint32_t f) {
            d3dpt_u32x4 *r = (d3dpt_u32x4 *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_CUBE_FACE, sizeof *r, 0);
            *r = { face_h, cube_h, f, 0 };
        };
        for (uint32_t f = 0; f < 6; f++) {
            fill(CUBE_OFF + f * FSTRIDE, EDGE * EDGE, C0[f]);
            fill(CUBE_OFF + f * FSTRIDE + lvl_off(EDGE, 1), (EDGE / 2) * (EDGE / 2), C1[f]);
        }
        cube_rec(H_CUBE, CUBE_OFF, EDGE, 2, D3DFMT_A8R8G8B8, D3DPT_VS_TEXTURE, FSTRIDE);
        for (uint32_t f = 1; f < 6; f++) face_rec(H_CUBE + f, H_CUBE, f);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == 0, "cube texture and its five faces registered (status %u)", enc.last_status);
        struct cv { float x, y, z, rhw; uint32_t diffuse; float u, v, w; };
        const uint32_t FVF_CUBE = 0x4 | 0x40 | 0x100 | (1u << 16);   /* XYZRHW | DIFFUSE | TEX1 | TEXCOORDSIZE3(0) */
        auto quad = [](float x0, float y0, float sz, const float *d) {
            static const float xs[6] = { 0, 1, 0, 0, 1, 1 }, ys[6] = { 0, 0, 1, 1, 0, 1 };
            std::vector<cv> q;
            for (int i = 0; i < 6; i++) q.push_back({ x0 + xs[i] * sz, y0 + ys[i] * sz, 0.5f, 1.0f, 0xffffffffu, d[0], d[1], d[2] });
            return q;
        };
        /* a quad at each face's direction, 40 pixels each along y = 400..440 */
        auto faces = [&](Dp2Buf &e, uint32_t tex) {
            e.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            e.set_vs(FVF_CUBE);
            e.tss(0, 0, tex); e.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); e.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            e.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); e.tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            e.tss(0, 16, 1); e.tss(0, 17, 1); e.tss(0, 18, 2);          /* point sampling, point mips */
            for (int f = 0; f < 6; f++) e.draw8(4, 2, FVF_CUBE, quad(40.0f + 90.0f * f, 400.0f, 40.0f, DIR[f]));
        };
        auto at = [](int f) { return px(60 + 90 * f, 420); };
        auto all_faces = [&](const uint32_t *want) { bool ok = true; for (int f = 0; f < 6; f++) ok = ok && near_(at(f), want[f] & 0xffffff, 2); return ok; };
        Dp2Buf k1;
        faces(k1, H_CUBE);
        /* +Z spread over 4 x 4 pixels: 4 texels a pixel, level 2 asked, level 1 the last */
        std::vector<cv> mq = { { 300, 300, 0.5f, 1, ~0u, -1, 1, 1 }, { 304, 300, 0.5f, 1, ~0u, 1, 1, 1 }, { 300, 304, 0.5f, 1, ~0u, -1, -1, 1 },
                               { 300, 304, 0.5f, 1, ~0u, -1, -1, 1 }, { 304, 300, 0.5f, 1, ~0u, 1, 1, 1 }, { 304, 304, 0.5f, 1, ~0u, 1, -1, 1 } };
        k1.draw8(4, 2, FVF_CUBE, mq);
        hr = send_dp2(&enc, k1, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && all_faces(C0), "cube texture, a quad at each face's direction: 0x%06x 0x%06x 0x%06x 0x%06x 0x%06x 0x%06x",
              at(0), at(1), at(2), at(3), at(4), at(5));
        CHECK(near_(px(301, 301), C1[4] & 0xffffff, 2), "cube level 1 where +Z is minified: 0x%06x", px(301, 301));
        /* face +Y rewritten in VRAM, VRAM_DIRTY under the face's handle: the whole cube is read again */
        fill(CUBE_OFF + 2 * FSTRIDE, EDGE * EDGE, 0xffffffffu);
        { d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_DIRTY, sizeof *hh, 0); *hh = { H_CUBE + 2, 0 }; }
        Dp2Buf k2;
        faces(k2, H_CUBE);
        hr = send_dp2(&enc, k2, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(at(2), 0xffffff, 2) && near_(at(1), C0[1] & 0xffffff, 2),
              "a face's VRAM rewritten, VRAM_DIRTY under the face's own handle: the cube re-read (0x%06x, its neighbour 0x%06x)", at(2), at(1));
        /* a render-target cube: faces -Y and +X cleared through SETRENDERTARGET by face handle, then sampled */
        cube_rec(H_RTC, RTC_OFF, RTE, 1, D3DFMT_X8R8G8B8, D3DPT_VS_TEXTURE | D3DPT_VS_RENDER_TARGET, RTE * RTE * 4);
        for (uint32_t f = 1; f < 6; f++) face_rec(H_RTC + f, H_RTC, f);
        Dp2Buf k3;
        k3.cmd(41, 1); k3.u32(H_RTC + 3); k3.u32(0);
        k3.clear(D3DCLEAR_TARGET, 0xffff00ffu, 1.0f);
        k3.cmd(41, 1); k3.u32(H_RTC); k3.u32(0);
        k3.clear(D3DCLEAR_TARGET, 0xff00ff00u, 1.0f);
        k3.cmd(41, 1); k3.u32(H_RT); k3.u32(H_Z);
        faces(k3, H_RTC);
        hr = send_dp2(&enc, k3, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(at(3), 0xff00ff, 2) && near_(at(0), 0x00ff00, 2),
              "render-target cube: faces -Y and +X drawn into by their own handles, then sampled: 0x%06x 0x%06x", at(3), at(0));
        hr = readback(&enc, H_RTC + 3);
        uint32_t f3;
        memcpy(&f3, vram + RTC_OFF + 3 * RTE * RTE * 4 + 5 * RTE * 4 + 5 * 4, 4);
        CHECK(hr == 0 && (f3 & 0xffffff) == 0xff00ff, "a render-target face read back into its own VRAM (0x%08x, 0x%06x)", hr, f3 & 0xffffff);
        Dp2Buf k4;
        k4.tss(0, 0, 0);
        hr = send_dp2(&enc, k4, vtx);
        /* hostile: not square, short of its face levels, faces past VRAM; a
         * face 6, a face of an unknown cube, a face of a texture that is no cube */
        cube_rec(40, CUBE_OFF, EDGE, 2, D3DFMT_A8R8G8B8, D3DPT_VS_TEXTURE, FSTRIDE, EDGE * 2);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a cube that is not square refused (status %u)", enc.last_status);
        cube_rec(40, CUBE_OFF, EDGE, 2, D3DFMT_A8R8G8B8, D3DPT_VS_TEXTURE, FSTRIDE, 0, 3);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a cube record short of its face levels refused (status %u)", enc.last_status);
        cube_rec(40, VRAM_SIZE - FSTRIDE, EDGE, 2, D3DFMT_A8R8G8B8, D3DPT_VS_TEXTURE, FSTRIDE);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a cube whose faces run past VRAM refused (status %u)", enc.last_status);
        face_rec(41, H_CUBE, 6);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "cube face 6 refused (status %u)", enc.last_status);
        face_rec(41, 999, 1);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_HANDLE, "a face of an unknown cube refused (status %u)", enc.last_status);
        face_rec(41, H_TEX16, 1);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_HANDLE, "a face of a texture that is no cube refused (status %u)", enc.last_status);
    }

    /* --- volume textures (v12): a 16 x 16 x 4 volume of two levels in VRAM,
     * every slice its own colour, drawn at each slice's w and minified onto a
     * small quad (level 1); a slice's VRAM rewritten and marked dirty;
     * hostile records --- */
    {
        enum { VOL_OFF = 0x5c0000, H_VOL = 50, VE = 16, VD = 4, VSLICE = VE * VE * 4 };
        static const uint32_t V0[VD] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffff00u };
        static const uint32_t V1[VD / 2] = { 0xff800080u, 0xff008080u };
        auto fill = [](uint32_t off, uint32_t n, uint32_t c) { for (uint32_t i = 0; i < n; i++) memcpy(vram + off + 4 * i, &c, 4); };
        /* a volume's record: level 0's slices VSLICE apart, level 1 (8 x 8 x 2) at lv1 (right after level 0 unless
         * given); `pair` = false leaves the {depth, slice pitch} out */
        auto vol_rec = [&](uint32_t handle, uint32_t off0, uint32_t depth, uint32_t slice, uint32_t caps = D3DPT_VS_TEXTURE,
                           bool pair = true, uint32_t lv1 = 0) {
            d3dpt_vram_surface *s = (d3dpt_vram_surface *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_SURFACE, sizeof *s, (pair ? 2 : 1) * sizeof(d3dpt_u32x2));
            *s = { handle, off0, VE, VE, VE * 4, D3DFMT_A8R8G8B8, caps | D3DPT_VS_VOLUME, 2 };
            d3dpt_u32x2 *lv = (d3dpt_u32x2 *)(s + 1);
            lv[0] = { lv1 ? lv1 : off0 + VSLICE * VD, (VE / 2) * 4 };
            if (pair) lv[1] = { depth, slice };
        };
        for (uint32_t z = 0; z < VD; z++) fill(VOL_OFF + z * VSLICE, VE * VE, V0[z]);
        for (uint32_t z = 0; z < VD / 2; z++) fill(VOL_OFF + VSLICE * VD + z * (VE / 2) * (VE / 2) * 4, (VE / 2) * (VE / 2), V1[z]);
        vol_rec(H_VOL, VOL_OFF, VD, VSLICE);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == 0, "volume texture registered (status %u)", enc.last_status);
        struct vv { float x, y, z, rhw; uint32_t diffuse; float u, v, w; };
        const uint32_t FVF_VOL = 0x4 | 0x40 | 0x100 | (1u << 16);    /* XYZRHW | DIFFUSE | TEX1 | TEXCOORDSIZE3(0) */
        auto quad = [](float x0, float y0, float sz, float w) {
            static const float xs[6] = { 0, 1, 0, 0, 1, 1 }, ys[6] = { 0, 0, 1, 1, 0, 1 };
            std::vector<vv> q;
            for (int i = 0; i < 6; i++) q.push_back({ x0 + xs[i] * sz, y0 + ys[i] * sz, 0.5f, 1.0f, 0xffffffffu, xs[i], ys[i], w });
            return q;
        };
        /* a quad at each slice's w, 40 pixels each along y = 400..440 */
        auto slices = [&](Dp2Buf &e) {
            e.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
            e.set_vs(FVF_VOL);
            e.tss(0, 0, H_VOL); e.tss(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); e.tss(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            e.tss(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); e.tss(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
            e.tss(0, 16, 1); e.tss(0, 17, 1); e.tss(0, 18, 2);          /* point sampling, point mips */
            for (int z = 0; z < VD; z++) e.draw8(4, 2, FVF_VOL, quad(40.0f + 90.0f * z, 400.0f, 40.0f, (z + 0.5f) / VD));
        };
        auto at = [](int z) { return px(60 + 90 * z, 420); };
        Dp2Buf v1;
        slices(v1);
        /* 16 texels on 4 pixels: level 2 asked, level 1 the last; a w of 0.125 is its first slice */
        v1.draw8(4, 2, FVF_VOL, quad(300.0f, 300.0f, 4.0f, 0.125f));
        hr = send_dp2(&enc, v1, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(at(0), V0[0] & 0xffffff, 2) && near_(at(1), V0[1] & 0xffffff, 2) && near_(at(2), V0[2] & 0xffffff, 2) &&
              near_(at(3), V0[3] & 0xffffff, 2), "volume texture, a quad at each slice: 0x%06x 0x%06x 0x%06x 0x%06x", at(0), at(1), at(2), at(3));
        CHECK(near_(px(301, 301), V1[0] & 0xffffff, 2), "volume level 1 where it is minified: 0x%06x", px(301, 301));
        /* slice 2 rewritten in VRAM, VRAM_DIRTY: the volume is read again */
        fill(VOL_OFF + 2 * VSLICE, VE * VE, 0xffffffffu);
        { d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_DIRTY, sizeof *hh, 0); *hh = { H_VOL, 0 }; }
        Dp2Buf v2;
        slices(v2);
        hr = send_dp2(&enc, v2, vtx);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(at(2), 0xffffff, 2) && near_(at(1), V0[1] & 0xffffff, 2),
              "a slice's VRAM rewritten, VRAM_DIRTY: the volume re-read (0x%06x, its neighbour 0x%06x)", at(2), at(1));
        Dp2Buf v3;
        v3.tss(0, 0, 0);
        hr = send_dp2(&enc, v3, vtx);
        /* hostile: depth 0, deeper than the limit, a slice pitch shorter than a slice, slices past VRAM, a volume
         * that is a cube too, a render-target volume, a record without its {depth, slice pitch} */
        vol_rec(51, VOL_OFF, 0, VSLICE);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a volume of depth 0 refused (status %u)", enc.last_status);
        vol_rec(51, VOL_OFF, D3DPT_VOLUME_MAX_DEPTH + 1, VSLICE);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a volume deeper than %u refused (status %u)", D3DPT_VOLUME_MAX_DEPTH, enc.last_status);
        vol_rec(51, VOL_OFF, VD, VE * 4);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a slice pitch shorter than a slice refused (status %u)", enc.last_status);
        vol_rec(51, VRAM_SIZE - 2 * VSLICE, 16, VSLICE, D3DPT_VS_TEXTURE, true, VOL_OFF + VSLICE * VD);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a volume whose slices run past VRAM refused (status %u)", enc.last_status);
        vol_rec(51, VOL_OFF, VD, VSLICE, D3DPT_VS_TEXTURE | D3DPT_VS_CUBE);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a volume that is a cube too refused (status %u)", enc.last_status);
        vol_rec(51, VOL_OFF, VD, VSLICE, D3DPT_VS_TEXTURE | D3DPT_VS_RENDER_TARGET);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a render-target volume refused (status %u)", enc.last_status);
        vol_rec(51, VOL_OFF, VD, VSLICE, D3DPT_VS_TEXTURE, false);
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "a volume record without its {depth, slice pitch} refused (status %u)", enc.last_status);
    }

    /* --- the DirectX 3 execute-buffer path (doc 15 "Execute buffers"): the
     * legacy 8-byte INDEXEDTRIANGLELIST (v1, v2, v3, wFlags) and the DX5
     * texture render states a DX3 title still sends, TEXTUREHANDLE (1) and
     * TEXTUREMAPBLEND (21) first of all --- */
    {
        std::vector<tlv> quad(vtx.begin(), vtx.begin() + 6);
        for (tlv &v : quad) v.diffuse = 0xffff0000u;                     /* red vertices: MODULATE with white texels keeps them red */
        Dp2Buf e1;
        e1.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        e1.tss(0, 16, 1); e1.tss(0, 17, 1); e1.tss(0, 18, 1);
        e1.rs(1, H_TEX16); e1.rs(21, 2 /* MODULATE */); e1.rs(17, 1); e1.rs(18, 1);      /* the blue / white checker, no key now */
        e1.cmd(3, 2); e1.u16(0); e1.u16(1); e1.u16(2); e1.u16(0x1f); e1.u16(3); e1.u16(4); e1.u16(5); e1.u16(0x1f);
        hr = send_dp2(&enc, e1, quad);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x000000, 2) && near_(px(124, 84), 0xff0000, 2),
              "DX3 TRIANGLEs with TEXTUREHANDLE + MODULATE: blue cell x red = 0x%06x, white cell x red = 0x%06x", px(104, 84), px(124, 84));
        Dp2Buf e2;
        e2.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        e2.rs(21, 1 /* DECAL */);
        e2.cmd(3, 2); e2.u16(0); e2.u16(1); e2.u16(2); e2.u16(0x1f); e2.u16(3); e2.u16(4); e2.u16(5); e2.u16(0x1f);
        hr = send_dp2(&enc, e2, quad);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x0000ff, 2) && near_(px(124, 84), 0xffffff, 2),
              "TEXTUREMAPBLEND DECAL: the texels alone 0x%06x 0x%06x", px(104, 84), px(124, 84));
        Dp2Buf e3;
        e3.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        e3.rs(1, 0);                                                    /* no texture: the diffuse colour, whatever the blend */
        e3.cmd(3, 2); e3.u16(0); e3.u16(1); e3.u16(2); e3.u16(0x1f); e3.u16(3); e3.u16(4); e3.u16(5); e3.u16(0x1f);
        hr = send_dp2(&enc, e3, quad);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0xff0000, 2) && near_(px(124, 84), 0xff0000, 2),
              "TEXTUREHANDLE 0: the vertex colour 0x%06x 0x%06x", px(104, 84), px(124, 84));
        /* a DirectX 6 title: the blend chosen while no texture is bound, the texture then bound as a
         * stage state per draw (GTA 2's menu text): the blend must follow the texture */
        Dp2Buf e5;
        e5.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        e5.rs(21, 2 /* MODULATE */); e5.tss(0, 0, H_TEX16);
        e5.cmd(3, 2); e5.u16(0); e5.u16(1); e5.u16(2); e5.u16(0x1f); e5.u16(3); e5.u16(4); e5.u16(5); e5.u16(0x1f);
        hr = send_dp2(&enc, e5, quad);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x000000, 2) && near_(px(124, 84), 0xff0000, 2),
              "TEXTUREMAPBLEND before the texture, bound as a stage state: blue cell x red = 0x%06x, white cell x red = 0x%06x", px(104, 84), px(124, 84));
        /* Crimson Skies' menu: the runtime's context dump has set the ops, the
         * title then picks MODULATE with no texture bound, sets its own COLORARG2 / ALPHAARG2
         * (its arguments, not its ops) and binds a texture per draw: the blend must still
         * follow the texture; an argument does not end it (the logo drew as a white silhouette) */
        Dp2Buf e6;
        e6.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        e6.tss(0, 1, D3DTOP_MODULATE); e6.tss(0, 4, D3DTOP_SELECTARG1); e6.tss(0, 0, 0);
        e6.rs(21, 2 /* MODULATE */); e6.tss(0, 3, D3DTA_DIFFUSE); e6.tss(0, 6, D3DTA_DIFFUSE); e6.tss(0, 0, H_TEX16);
        e6.cmd(3, 2); e6.u16(0); e6.u16(1); e6.u16(2); e6.u16(0x1f); e6.u16(3); e6.u16(4); e6.u16(5); e6.u16(0x1f);
        hr = send_dp2(&enc, e6, quad);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0x000000, 2) && near_(px(124, 84), 0xff0000, 2),
              "TEXTUREMAPBLEND, then the app's ARGs, then the texture: blue cell x red = 0x%06x, white cell x red = 0x%06x", px(104, 84), px(124, 84));
        /* and the app's own op does end it: SELECTARG2 set by the app stays over the next bind */
        Dp2Buf e7;
        e7.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        e7.tss(0, 1, D3DTOP_SELECTARG2); e7.tss(0, 0, 0); e7.tss(0, 0, H_TEX16);
        e7.cmd(3, 2); e7.u16(0); e7.u16(1); e7.u16(2); e7.u16(0x1f); e7.u16(3); e7.u16(4); e7.u16(5); e7.u16(0x1f);
        hr = send_dp2(&enc, e7, quad);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(104, 84), 0xff0000, 2) && near_(px(124, 84), 0xff0000, 2),
              "the app's own COLOROP over a later bind: 0x%06x 0x%06x", px(104, 84), px(124, 84));
        Dp2Buf e4; e4.rs(21, 2); e4.tss(0, 1, D3DTOP_MODULATE); e4.tss(0, 4, D3DTOP_SELECTARG1);   /* back to the scene's stage states */
        hr = send_dp2(&enc, e4, quad);
        CHECK(hr == 0, "stage states restored (0x%08x)", hr);

        /* --- untracked writes (doc 15 "Untracked writes"): the guest writes
         * the target's VRAM with no VRAM_DIRTY (GDI through GetDC bypasses
         * DdLock / DdUnlock). Before the frame's first draw the executor
         * finds the change against its shadow of the target and uploads it,
         * so the draws land on top; after the draws it keeps the guest's
         * pixels over the host frame at the readback --- */
        hr = readback(&enc, H_RT);                                      /* host and VRAM agree from here */
        auto poke = [](int x0, int y0, int w, int h, uint32_t c) {
            for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) memcpy(vram + RT_OFF + y * RT_PITCH + x * 4, &c, 4);
        };
        poke(200, 200, 40, 40, 0xff00ff00u);                            /* inside the quad: the draw covers it */
        poke(500, 20, 40, 40, 0xff00ff00u);                             /* outside: read back as it is */
        Dp2Buf u1;
        u1.rs(1, 0);
        u1.cmd(3, 2); u1.u16(0); u1.u16(1); u1.u16(2); u1.u16(0x1f); u1.u16(3); u1.u16(4); u1.u16(5); u1.u16(0x1f);
        hr = send_dp2(&enc, u1, quad);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(220, 220), 0xff0000, 2) && near_(px(520, 40), 0x00ff00, 2),
              "untracked write before the draws: uploaded, the quad on top 0x%06x, the rest kept 0x%06x", px(220, 220), px(520, 40));
        hr = send_dp2(&enc, u1, quad);                                  /* the quad again, then writes after the draws */
        poke(200, 200, 40, 40, 0xff00ffffu);
        poke(500, 20, 40, 40, 0xff00ffffu);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(220, 220), 0x00ffff, 2) && near_(px(120, 100), 0xff0000, 2),
              "untracked write after the draws: kept over the host frame 0x%06x, the quad around it 0x%06x", px(220, 220), px(120, 100));
        hr = send_dp2(&enc, u1, quad);                                  /* the next frame: drawn over where the quad is, persisting elsewhere */
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(220, 220), 0xff0000, 2) && near_(px(520, 40), 0x00ffff, 2),
              "kept pixels in the next frame: drawn over 0x%06x, persisting 0x%06x", px(220, 220), px(520, 40));

        /* --- ZBIAS (47) -> DEPTHBIAS: a coplanar quad loses the Z test
         * (LESS) without it and wins with it --- */
        std::vector<tlv> green(quad);
        for (tlv &v : green) v.diffuse = 0xff00ff00u;
        Dp2Buf z1;
        z1.clear(D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, CLEAR_COLOR, 1.0f);
        z1.rs(7, 1); z1.rs(14, 1); z1.rs(23, D3DCMP_LESS);
        z1.cmd(3, 2); z1.u16(0); z1.u16(1); z1.u16(2); z1.u16(0x1f); z1.u16(3); z1.u16(4); z1.u16(5); z1.u16(0x1f);
        hr = send_dp2(&enc, z1, quad);
        Dp2Buf z2;
        z2.cmd(3, 2); z2.u16(0); z2.u16(1); z2.u16(2); z2.u16(0x1f); z2.u16(3); z2.u16(4); z2.u16(5); z2.u16(0x1f);
        hr |= send_dp2(&enc, z2, green);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(220, 220), 0xff0000, 2), "coplanar quad under ZFUNC LESS: the first stays 0x%06x", px(220, 220));
        Dp2Buf z3;
        z3.rs(47, 8);
        z3.cmd(3, 2); z3.u16(0); z3.u16(1); z3.u16(2); z3.u16(0x1f); z3.u16(3); z3.u16(4); z3.u16(5); z3.u16(0x1f);
        z3.rs(47, 0); z3.rs(23, D3DCMP_LESSEQUAL);
        hr = send_dp2(&enc, z3, green);
        hr |= readback(&enc, H_RT);
        CHECK(hr == 0 && near_(px(220, 220), 0x00ff00, 2), "with ZBIAS 8 it wins: 0x%06x", px(220, 220));
    }

    /* --- hostile records --- */
    vram_surface(&enc, 9, VRAM_SIZE - 4096, 64, 64, 256, D3DFMT_X8R8G8B8, D3DPT_VS_TEXTURE);
    d3dpt_enc_flush(&enc);
    CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "surface beyond VRAM refused (status %u)", enc.last_status);
    vram_surface(&enc, 9, VRAM_SIZE - 4096, 8192, 1, 8192, 0, D3DPT_VS_BUFFER);           /* v9: a buffer beyond VRAM */
    d3dpt_enc_flush(&enc);
    CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "buffer beyond VRAM refused (status %u)", enc.last_status);
    vram_surface(&enc, 9, VB_OFF, 4096, 1, 4096, 0, D3DPT_VS_BUFFER | D3DPT_VS_TEXTURE);  /* a buffer that is also a texture */
    d3dpt_enc_flush(&enc);
    CHECK(enc.last_status == D3DPT_ERR_BAD_ARG, "buffer with texture caps refused (status %u)", enc.last_status);
    hr = send_dp2(&enc, d, vtx, (uint32_t)d.b.size() + 4096);
    CHECK(hr == (0xffff0000u | D3DPT_ERR_BAD_ARG), "DP2 record lying about its command bytes refused (0x%08x)", hr);
    Dp2Buf bad; bad.rs(D3DRS_ZENABLE, 1); bad.cmd(18, 3);           /* TRIANGLELIST without its start vertex */
    hr = send_dp2(&enc, bad, vtx, 0, &err_off);
    CHECK(hr == 0x88760BB8u && err_off == 12, "truncated token stream -> D3DERR_COMMAND_UNPARSED at %u (0x%08x)", err_off, hr);
    /* a light index / transform id off the scale: DXVK would grow its light
     * array to the index (std::bad_alloc, uncatchable across its own
     * unwinder: the process aborts) or index past its transform array */
    Dp2Buf wild; wild.cmd(34, 2); wild.u32(0xfffffff0u); wild.u32(0); wild.u32(0xfffffff0u); wild.u32(2); for (int i = 0; i < 26; i++) wild.u32(0);
    wild.cmd(36, 1); wild.u32(0x7fffffffu); for (int i = 0; i < 16; i++) wild.f32(i % 5 == 0 ? 1.0f : 0.0f);
    wild.trilist(0, 2);
    hr = send_dp2(&enc, wild, vtx);
    CHECK(hr == 0, "wild light index / transform id dropped, the draw still runs (0x%08x)", hr);
    Dp2Buf oob; oob.trilist(100, 4);                                 /* vertices beyond the buffer: skipped, not fatal */
    hr = send_dp2(&enc, oob, vtx);
    CHECK(hr == 0, "out-of-range vertices skipped (0x%08x)", hr);
    { d3dpt_handle *hh = (d3dpt_handle *)d3dpt_enc_cmd(&enc, D3DPT_OP_VRAM_RELEASE, sizeof *hh, 0); *hh = { H_TEX, 0 }; }
    Dp2Buf gone; gone.tss(0, 0, H_TEX); gone.trilist(0, 2);
    hr = send_dp2(&enc, gone, vtx);
    CHECK(hr == 0, "released texture bound: drawn untextured, not fatal (0x%08x)", hr);

    /* --- a runtime that swaps two flip buffers' memory (a DirectX 6 title's, doc 15): the
     * target registered again at another offset. The host's shadow of the old memory must not
     * count against the new: the readback into it is the whole frame, so the two frames of the
     * same scene are identical --- */
    vram_surface(&enc, H_RT, RT2_OFF, W, H, RT_PITCH, D3DFMT_X8R8G8B8, D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY);
    hr = send_dp2(&enc, d, vtx);
    hr |= readback(&enc, H_RT);
    vram_surface(&enc, H_RT, RT_OFF, W, H, RT_PITCH, D3DFMT_X8R8G8B8, D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY);
    hr |= send_dp2(&enc, d, vtx);
    hr |= readback(&enc, H_RT);
    CHECK(hr == 0 && memcmp(vram + RT_OFF, vram + RT2_OFF, (size_t)RT_PITCH * H) == 0,
          "a target moved to another offset reads back whole (0x%08x, frames %s)", hr,
          memcmp(vram + RT_OFF, vram + RT2_OFF, (size_t)RT_PITCH * H) ? "differ" : "equal");
    /* --- the context again under its open handle: a guest that never destroyed it (the display
     * driver once lost its table with the PDEV, GTA 2) gets a fresh one, not BAD_HANDLE --- */
    {
        uint32_t off = d3dpt_enc_ret(&enc, 0);
        d3dpt_ctx_create *c = (d3dpt_ctx_create *)d3dpt_enc_cmd(&enc, D3DPT_OP_CTX_CREATE, sizeof *c, 0);
        *c = { CTX, off, H_RT, H_Z };
        d3dpt_enc_flush(&enc);
        CHECK(enc.last_status == 0 && d3dpt_enc_result(&enc, off)->hr == 0, "context re-created under its open handle (status %u hr 0x%08x)", enc.last_status, d3dpt_enc_result(&enc, off)->hr);
        hr = send_dp2(&enc, d, vtx);
        CHECK(hr == 0, "the scene draws on the re-created context (0x%08x)", hr);
    }

    p_attach(X, 0);
    p_destroy(X);
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
