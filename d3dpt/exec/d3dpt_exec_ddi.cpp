/*
 * d3dpt_exec_ddi.cpp: the display driver's records of the paravirtual
 * Direct3D executor (doc 15, M7c): what the XP display driver
 * (guest-tools/src/d3dptvid/nt/d3dptdisp.c) sends through the d3dpt-vga
 * adapter's command window when dxg.sys drives its Direct3D DDI.
 *
 * The model: every DirectDraw surface stays in guest VRAM (dxg's heap),
 * the host mirrors the ones Direct3D touches. A VRAM_SURFACE record
 * registers a surface by the runtime's handle with its VRAM offset,
 * size, pitch and D3DFORMAT; textures become DXVK textures whose texels
 * are read straight from the VRAM pointer (re-read after VRAM_DIRTY),
 * render targets and Z buffers become host surfaces. A CTX_CREATE binds
 * a render target + Z pair as a Direct3D context; a DP2 record carries
 * one D3dDrawPrimitives2 call (the DX7 token stream plus the vertex
 * buffer) and is interpreted on IDirect3DDevice9; READBACK copies the
 * host render target back into the surface's VRAM so the guest's flips,
 * HEL blits and Lock reads see the frame.
 *
 * First cut = the DX7 non-T&L HAL: the runtime transforms and lights, the
 * vertices arrive as XYZRHW, so the tokens needed are RENDERSTATE,
 * TEXTURESTAGESTATE, VIEWPORTINFO, ZRANGE, SETRENDERTARGET, CLEAR and the
 * primitive tokens. The T&L tokens (SETTRANSFORM / SETLIGHT / SETMATERIAL)
 * are mapped 1:1 onto d3d9 already so claiming T&L is a caps change.
 * The DX8 DDI adds the driver's self-contained D3DPT_DP2_DRAW8 draws and,
 * with protocol v7, the shader tokens: CREATEVERTEXSHADER becomes a d3d9
 * vertex declaration (+ a vertex shader with dcl instructions in front),
 * CREATEPIXELSHADER a d3d9 pixel shader, the constants go straight
 * through; a DRAW8 under a shader carries the handle in its fvf field.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "d3dpt_exec_int.h"
#include <algorithm>
#include <bitset>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#ifndef _WIN32
#include <unistd.h>
#endif

/* `access(p, F_OK)` without unistd.h, which Windows does not have; the
 * trace dumps are the only thing that asks. */
static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

namespace d3dpt {

namespace {

/* DP2 tokens (D3DHAL_DP2OPERATION of the DDK's d3dhal.h) */
enum {
    DP2_POINTS = 1, DP2_INDEXEDLINELIST = 2, DP2_INDEXEDTRIANGLELIST = 3, DP2_RENDERSTATE = 8,
    DP2_LINELIST = 15, DP2_LINESTRIP = 16, DP2_INDEXEDLINESTRIP = 17, DP2_TRIANGLELIST = 18,
    DP2_TRIANGLESTRIP = 19, DP2_INDEXEDTRIANGLESTRIP = 20, DP2_TRIANGLEFAN = 21,
    DP2_INDEXEDTRIANGLEFAN = 22, DP2_TRIANGLEFAN_IMM = 23, DP2_LINELIST_IMM = 24,
    DP2_TEXTURESTAGESTATE = 25, DP2_INDEXEDTRIANGLELIST2 = 26, DP2_INDEXEDLINELIST2 = 27,
    DP2_VIEWPORTINFO = 28, DP2_WINFO = 29, DP2_SETPALETTE = 30, DP2_UPDATEPALETTE = 31,
    DP2_ZRANGE = 32, DP2_SETMATERIAL = 33, DP2_SETLIGHT = 34, DP2_CREATELIGHT = 35,
    DP2_SETTRANSFORM = 36, DP2_TEXBLT = 38, DP2_STATESET = 39, DP2_SETPRIORITY = 40,
    DP2_SETRENDERTARGET = 41, DP2_CLEAR = 42, DP2_SETTEXLOD = 43, DP2_SETCLIPPLANE = 44,
    /* DX8 (the display driver rewrites the draws into D3DPT_DP2_DRAW8; the
     * rest is parsed for its size and dropped, or mapped when d3d9 has it) */
    DP2_CREATEVERTEXSHADER = 45, DP2_DELETEVERTEXSHADER = 46, DP2_SETVERTEXSHADER = 47,
    DP2_SETVERTEXSHADERCONST = 48, DP2_SETSTREAMSOURCE = 49, DP2_SETSTREAMSOURCEUM = 50, DP2_SETINDICES = 51,
    DP2_DRAWPRIMITIVE = 52, DP2_DRAWINDEXEDPRIMITIVE = 53, DP2_CREATEPIXELSHADER = 54, DP2_DELETEPIXELSHADER = 55,
    DP2_SETPIXELSHADER = 56, DP2_SETPIXELSHADERCONST = 57, DP2_CLIPPEDTRIANGLEFAN = 58, DP2_DRAWPRIMITIVE2 = 59,
    DP2_DRAWINDEXEDPRIMITIVE2 = 60, DP2_DRAWRECTPATCH = 61, DP2_DRAWTRIPATCH = 62, DP2_VOLUMEBLT = 63,
    DP2_BUFFERBLT = 64, DP2_MULTIPLYTRANSFORM = 65, DP2_ADDDIRTYRECT = 66, DP2_ADDDIRTYBOX = 67,
    /* DX9 (protocol v14, M16): declarations and shader code apart, integer
     * and boolean constants, the scissor. The driver consumes
     * SETVERTEXSHADERDECL (a draw carries it), SETSTREAMSOURCE2 and the
     * target tokens, and drops the ones it does not walk yet */
    DP2_CREATEVERTEXSHADERDECL = 71, DP2_DELETEVERTEXSHADERDECL = 72, DP2_SETVERTEXSHADERDECL = 73,
    DP2_CREATEVERTEXSHADERFUNC = 74, DP2_DELETEVERTEXSHADERFUNC = 75, DP2_SETVERTEXSHADERFUNC = 76,
    DP2_SETVERTEXSHADERCONSTI = 77, DP2_SETSCISSORRECT = 79, DP2_SETVERTEXSHADERCONSTB = 83,
    DP2_BLT = 81, DP2_SETRENDERTARGET2 = 85, DP2_GENERATEMIPSUBLEVELS = 89, DP2_SETPIXELSHADERCONSTI = 93, DP2_SETPIXELSHADERCONSTB = 94,
};
#define D3DERR_COMMAND_UNPARSED_ 0x88760BB8u

struct VramSurf {
    d3dpt_vram_surface d{};
    std::vector<d3dpt_u32x2> levels;    /* mip levels 1..n-1: offset, pitch */
    IDirect3DTexture9 *tex = nullptr;
    IDirect3DSurface9 *rt = nullptr;    /* render target or depth stencil */
    bool dirty = true;                  /* VRAM newer than the host object */
    bool rendered = false;              /* host render target newer than VRAM */
    bool mips_stale = false;            /* v15: an autogen target drawn into since its levels were made */
    /* a render target's VRAM as of the last time the host and VRAM agreed
     * (after an upload or a readback): what differs from it later was
     * written by the guest without a VRAM_DIRTY (GDI on the surface's DC,
     * since GetDC bypasses DdLock / DdUnlock, or a title's own writes through
     * a cached pointer) and is uploaded before the frame's first draw or
     * kept over the host's pixels at the readback (doc 15 "Untracked
     * writes") */
    std::vector<uint8_t> shadow;
    bool checked = false;               /* the untracked-write check ran since the last readback */
    /* v8: a P8 texture's palette (SETPALETTE: the runtime's palette handle,
     * whether its entries carry alpha) and a source colour key
     * (VRAM_COLORKEY): both make the host texture an A8R8G8B8 expansion of
     * the VRAM texels (host_format) */
    uint32_t palette = 0;
    bool pal_alpha = false;
    bool ckey = false;
    uint32_t ckey_lo = 0, ckey_hi = 0;
    D3DFORMAT host_fmt = D3DFMT_UNKNOWN;    /* the format tex was created in */
    /* v11: a cube texture (D3DPT_VS_CUBE; its levels vector is face-major,
     * 6 * levels - 1 entries) and a cube face's level 0 (VRAM_CUBE_FACE):
     * its cube and face 1..5. On a render-target cube, rt is that face's
     * surface of the cube's host object */
    IDirect3DCubeTexture9 *cube = nullptr;
    uint32_t cube_root = 0, face = 0;
    uint32_t mip_root = 0, mip_level = 0;      /* v19: a render-target texture's level under its own handle */
    std::vector<uint32_t> mip_h;               /* v19: a render-target texture: its levels' handles */
    uint32_t face_h[D3DPT_CUBE_FACES] = {};    /* a cube: its faces' handles (0: none) */
    bool faces_dirty = false;                   /* a render-target cube: a face entry is dirty */
    /* v12: a volume texture (D3DPT_VS_VOLUME): level 0's depth and slice pitch */
    IDirect3DVolumeTexture9 *vol = nullptr;
    uint32_t depth = 0, slice = 0;
    /* v13: a multisampled render target / depth buffer (D3DPT_VS_SAMPLES):
     * the sample count its host object was created with (0 when this host
     * has none such, which draws it without), and the plain target a
     * readback resolves it into */
    uint32_t ms = 0;
    IDirect3DSurface9 *resolve = nullptr;
    void release() {
        if (tex) tex->Release();
        if (rt) rt->Release();
        if (cube) cube->Release();
        if (vol) vol->Release();
        if (resolve) resolve->Release();
        tex = nullptr; rt = nullptr; cube = nullptr; vol = nullptr; resolve = nullptr;
        host_fmt = D3DFMT_UNKNOWN;
        ms = 0;
    }
};

struct Palette { uint32_t argb[256]; };

/* a DX8 vertex shader (CREATEVERTEXSHADER, protocol v7): the D3DVSD_*
 * declaration as a d3d9 vertex declaration, the function (if any) as a
 * d3d9 vertex shader with the dcl instructions d3d9 wants in front, the
 * declaration's D3DVSD_CONST runs (loaded when the shader is set). A
 * declaration without a function is the fixed function on that layout. */
struct VShader8 {
    IDirect3DVertexDeclaration9 *decl = nullptr;
    IDirect3DVertexShader9 *vs = nullptr;
    struct ConstRun { uint32_t reg; std::vector<float> v; };
    std::vector<ConstRun> consts;
    uint32_t stream_bytes[D3DPT_DRAW8_MAX_STREAMS] = {};  /* what the declaration reads of each stream's vertex */
    uint32_t streams = 0;               /* bitmask of the streams it reads */
    bool pos_t = false;                 /* a POSITIONT element: pre-transformed vertices (Dp2::apply_clip) */
    std::vector<D3DVERTEXELEMENT9> el;  /* the d3d9 elements, END included */
    /* v10: the declaration with every stream it reads interleaved into
     * stream 0, one per set of strides seen (a few at most) */
    struct Ilv { uint32_t stride[D3DPT_DRAW8_MAX_STREAMS]; IDirect3DVertexDeclaration9 *decl; };
    std::vector<Ilv> ilv;
    void release() {
        if (decl) decl->Release();
        if (vs) vs->Release();
        for (Ilv &i : ilv) i.decl->Release();
        decl = nullptr; vs = nullptr;
        ilv.clear();
    }
};

struct Ctx {
    uint32_t rt = 0, z = 0;
    uint32_t mrt[4] = {};               /* v17: targets 1..3 (SETRENDERTARGET2), 0 = none; mrt[0] unused */
    D3DVIEWPORT9 vp = { 0, 0, 0, 0, 0.0f, 1.0f };
    /* the DX8 shaders by the runtime's handle (per device = per context);
     * a DX9 declaration (CREATEVERTEXSHADERDECL) is one of these with no
     * function, in the same handle space */
    std::unordered_map<uint32_t, VShader8> vshaders;
    std::unordered_map<uint32_t, IDirect3DPixelShader9 *> pshaders;
    /* DX9 vertex shader code (CREATEVERTEXSHADERFUNC) and the current one
     * (SETVERTEXSHADERFUNC; 0 = the fixed function). A context that has
     * set one is a DX9 context: apply_vs puts the function over whatever
     * declaration or FVF a draw carries */
    std::unordered_map<uint32_t, IDirect3DVertexShader9 *> vfuncs;
    uint32_t vfunc = 0;
    bool dx9 = false;
    bool np2cond = false;               /* v20: D3DPT_CTX_NP2_CONDITIONAL (Dp2::apply_addr) */
    /* One device serves every context. While another context has it, this
     * one's state is kept here: the device's (a state block of everything,
     * made when the device switched away, so it knows every light created
     * by then) and Ddi's shadows of it (Ddi::save_shadow). Two programs
     * drawing at once (a windowed game under Windows 7's DWM) otherwise
     * draw with each other's render states and textures. */
    IDirect3DStateBlock9 *sb = nullptr;
    struct Shadow {
        std::bitset<1024> lights_on;
        uint32_t ckey_rs = 0, stage_tex[21] = {}, addr[21][3] = {};
        bool ckey_forced = false, ckey_alpha_ovr = false, legacy_cop = false, legacy_aop = false;
        bool clip_tl = false;
        uint32_t clip_host = ~0u;
    } shadow;
    void release_shaders() {
        for (auto &kv : vshaders) kv.second.release();
        for (auto &kv : pshaders) if (kv.second) kv.second->Release();
        for (auto &kv : vfuncs) if (kv.second) kv.second->Release();
        vshaders.clear(); pshaders.clear(); vfuncs.clear();
        vfunc = 0;
        if (sb) sb->Release();
        sb = nullptr;
    }
};

} // namespace

struct Ddi {
    std::unordered_map<uint32_t, VramSurf> surfs;
    std::unordered_map<uint32_t, Ctx> ctxs;
    uint32_t bound_rt = 0, bound_z = 0;     /* what the device's targets are set to */
    uint32_t bound_mrt[4] = {};             /* v17: and its targets 1..3 */
    IDirect3DSurface9 *stage = nullptr;     /* system-memory staging for readback / upload */
    IDirect3DSurface9 *stage_def = nullptr; /* default-pool hop for uploads into render targets */
    uint32_t stage_w = 0, stage_h = 0;
    D3DFORMAT stage_fmt = D3DFMT_UNKNOWN;
    std::vector<uint16_t> idx;
    std::vector<uint8_t> ilv;               /* a multi-stream DRAW8's vertices, interleaved (v10) */
    std::vector<uint8_t> zeros;             /* a stream the declaration reads and the draw did not carry */
    /* one device serves every context, and a new context must not find the
     * last one's state: the device's own at its first context, and the
     * lights enabled since (a state block of the fresh device knows no
     * light created later), M16 finding 15 */
    IDirect3DStateBlock9 *fresh = nullptr;
    std::bitset<1024> lights_on;
    uint32_t cur_ctx = 0;                   /* the context whose state the device holds (use_ctx) */
    std::vector<uint32_t> warned;           /* one log line per unsupported state / token */
    /* DX8 state sets (STATESET tokens) as d3d9 state blocks, by the runtime's handle */
    std::unordered_map<uint32_t, IDirect3DStateBlock9 *> sblocks;
    bool recording = false;
    /* v8: the runtime's palettes (UPDATEPALETTE) by handle, and colour
     * keying: render state 41, the surface bound at stage 0, whether the
     * alpha test is forced on for its key (the app's own alpha test state
     * is restored when it is not) */
    std::unordered_map<uint32_t, Palette> palettes;
    /* the surface handle bound at each sampler: the 16 pixel samplers
     * (DX9's ps 2.0 has 16, the fixed function 8), then the displacement
     * map sampler and the four vertex samplers (D3DDMAPSAMPLER 256 and
     * D3DVERTEXTEXTURESAMPLER0..3 257..260, sampler_slot) */
    uint32_t ckey_rs = 0, stage_tex[21] = {};
    /* each sampler's ADDRESSU / V / W as the app set them (v20, apply_addr) */
    uint32_t addr[21][3];
    void reset_addr() { for (auto &a : addr) a[0] = a[1] = a[2] = D3DTADDRESS_WRAP; }
    Ddi() { reset_addr(); }
    bool ckey_forced = false, ckey_alpha_ovr = false;
    /* TEXTUREMAPBLEND / TEXTUREHANDLE in effect for stage 0's colour op and
     * for its alpha op: each half follows the bound texture until the app
     * sets that op itself. Its ARGs are not its ops: Crimson Skies sets
     * COLORARG2 / ALPHAARG2 after the blend and leaves both ops to it */
    bool legacy_cop = false, legacy_aop = false;
    uint32_t pal_lines = 0;                     /* palette / colour-key events logged (the first few) */
    uint32_t dp2_calls = 0, draws = 0, readbacks = 0;
    uint32_t untracked = 0;                     /* target pixels the guest wrote without VRAM_DIRTY (uploaded or kept) */
    uint32_t untracked_lines = 0;               /* the first such events logged */
    /* D3DPT_DP2_TRACE=<flag file>: while the file exists, every token of
     * the next frame (DP2 records up to the next READBACK) is logged with
     * its arguments; the file is removed when the frame ends */
    const char *trace_flag = env("D3DPT_DP2_TRACE");
    bool trace = false, trace_armed = false;
    uint32_t trace_draws = 0;               /* draw-<n>.ppm snapshots of the traced frame */
    /* the render / stage states seen so far (a snapshot at the start of a
     * traced frame: most are set once at scene start) */
    uint32_t rs_val[256] = {}, tss_val[8][33] = {};
    uint8_t rs_set[256] = {}, tss_set[8][33] = {};
    /* the vertex format bound is pre-transformed, and the D3DRS_CLIPPING
     * the host has (~0 = not set since the device was fresh; Dp2::apply_clip) */
    bool clip_tl = false;
    uint32_t clip_host = ~0u;
    /* D3DPT_DDI_NOFOG=1: FOGENABLE forced off (an experiment switch) */
    bool nofog = env("D3DPT_DDI_NOFOG") && atoi(env("D3DPT_DDI_NOFOG")) != 0;
    /* D3DPT_DDI_REREAD=1: every texture is re-read from VRAM at every bind
     * (the experiment that tells a stale host copy from never-written VRAM) */
    bool reread_all = env("D3DPT_DDI_REREAD") && atoi(env("D3DPT_DDI_REREAD")) != 0;
    /* a rate line every 5 s of host time while frames are read back (the
     * frame rate of the guest's Direct3D, one readback per presented frame) */
    std::chrono::steady_clock::time_point stat_t0{};
    uint32_t stat_dp2 = 0, stat_draws = 0, stat_rb = 0, stat_untracked = 0, stat_bw = 0, stat_bb = 0;
    uint32_t buffer_writes = 0, buffer_bytes = 0;   /* v9: VRAM_DIRTY_RANGE records (the guest's vertex / index buffer writes) */
    /* host time in readbacks, and the part of it spent waiting for the GPU's frame (GetRenderTargetData + the lock) */
    uint64_t rb_ns = 0, rb_wait_ns = 0, stat_rb_ns = 0, stat_rb_wait_ns = 0;

    /* the shadows of the device's state that belong to its current context */
    void save_shadow(Ctx::Shadow &s) const {
        s.lights_on = lights_on;
        s.ckey_rs = ckey_rs;
        memcpy(s.stage_tex, stage_tex, sizeof s.stage_tex);
        memcpy(s.addr, addr, sizeof s.addr);
        s.ckey_forced = ckey_forced; s.ckey_alpha_ovr = ckey_alpha_ovr;
        s.legacy_cop = legacy_cop; s.legacy_aop = legacy_aop;
        s.clip_tl = clip_tl; s.clip_host = clip_host;
    }
    void load_shadow(const Ctx::Shadow &s) {
        lights_on = s.lights_on;
        ckey_rs = s.ckey_rs;
        memcpy(stage_tex, s.stage_tex, sizeof stage_tex);
        memcpy(addr, s.addr, sizeof addr);
        ckey_forced = s.ckey_forced; ckey_alpha_ovr = s.ckey_alpha_ovr;
        legacy_cop = s.legacy_cop; legacy_aop = s.legacy_aop;
        clip_tl = s.clip_tl; clip_host = s.clip_host;
    }
    bool warn_once(uint32_t key) {
        for (uint32_t k : warned) if (k == key) return false;
        warned.push_back(key);
        return true;
    }
    void drop_stage() {
        if (stage) stage->Release();
        if (stage_def) stage_def->Release();
        stage = stage_def = nullptr;
        stage_w = stage_h = 0;
        if (flush_q) flush_q->Release();
        flush_q = nullptr;
    }

    /* The GPU works on a frame while the guest is still building it. The
     * executor never presents; its frame ends in a readback, and until then
     * DXVK submits only once three command chunks have piled up (EndScene's
     * hint at the end of every batch). The rest waited for the readback,
     * which then waited for the GPU to draw it: 1 ms a frame, 6 % of the
     * vCPU in Max Payne 2. An EVENT query's GetData(FLUSH) submits whenever
     * the GPU is short of work, so every flush_draws draws the recorded
     * commands go now. D3DPT_DDI_FLUSH_DRAWS=n sets it, 0 turns it off. */
    uint32_t flush_draws = 16, hint_draws = 0;
    /* D3DPT_DDI_FLUSH_AB=n: flush_draws and n by turns, one rate line
     * each, so an A/B is read within one run (a TCG run's own speed moves
     * by 5 % between launches, doc 22 §5.0); n = 0 is the hint off */
    bool flush_ab = false;
    uint32_t flush_alt = 0;
    IDirect3DQuery9 *flush_q = nullptr;
    void flush_hint(IDirect3DDevice9 *dev) {
        if (!flush_draws || draws - hint_draws < flush_draws) return;
        hint_draws = draws;
        if (!flush_q && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &flush_q))) { flush_q = nullptr; flush_draws = 0; return; }
        flush_q->Issue(D3DISSUE_END);
        flush_q->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    }
};

namespace {

static Ddi &ddi(Exec &x) {
    if (!x.ddi) {
        x.ddi = new Ddi;
        if (const char *e = env("D3DPT_DDI_FLUSH_DRAWS")) x.ddi->flush_draws = (uint32_t)atoi(e);
        if (const char *e = env("D3DPT_DDI_FLUSH_AB")) { x.ddi->flush_ab = true; x.ddi->flush_alt = (uint32_t)atoi(e); }
    }
    return *x.ddi;
}

/* The device's state leaves with its context (Ctx::sb): kept in a state
 * block of everything made now, with Ddi's shadows. */
static void save_ctx(Exec &x, Ddi &d) {
    if (!d.cur_ctx || !x.dev) return;
    auto it = d.ctxs.find(d.cur_ctx);
    d.cur_ctx = 0;
    if (it == d.ctxs.end()) return;
    Ctx &c = it->second;
    if (c.sb) c.sb->Release();
    c.sb = nullptr;
    if (FAILED(x.dev->CreateStateBlock(D3DSBT_ALL, &c.sb))) {
        c.sb = nullptr;
        if (d.warn_once(0xf1000)) x.log("ddi: context %u: no state block for its state, the next context draws with it", it->first);
    }
    d.save_shadow(c.shadow);
}

/* The device takes context h's state, when another context's is there. */
static void use_ctx(Exec &x, Ddi &d, uint32_t h, Ctx &c) {
    if (d.cur_ctx == h || !x.dev) return;
    save_ctx(x, d);
    if (c.sb) {
        /* a light this context never created is off: the state block knows only its own */
        for (uint32_t i = 0; i < d.lights_on.size(); i++) if (d.lights_on[i] && !c.shadow.lights_on[i]) x.dev->LightEnable(i, FALSE);
        c.sb->Apply();
        d.load_shadow(c.shadow);
    }
    d.cur_ctx = h;
}

/* --------------------------------------------------------------- formats */

static bool fmt_dxt(uint32_t f) {
    return f == D3DFMT_DXT1 || f == D3DFMT_DXT2 || f == D3DFMT_DXT3 || f == D3DFMT_DXT4 || f == D3DFMT_DXT5;
}
/* bytes of one row of w pixels (blocks for DXT); 0 = unknown format */
static uint32_t fmt_row_bytes(uint32_t f, uint32_t w) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10: case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4:
    case D3DFMT_X8L8V8U8: case D3DFMT_Q8W8V8U8:
    case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_V16U16: case D3DFMT_A2W10V10U10:
    case D3DFMT_G16R16F: case D3DFMT_R32F:
        return w * 4;
    case D3DFMT_A16B16G16R16: case D3DFMT_Q16W16V16U16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F:
        return w * 8;
    case D3DFMT_A32B32G32R32F:
        return w * 16;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1:
    case D3DFMT_L16: case D3DFMT_V8U8: case D3DFMT_A8R3G3B2: case D3DFMT_L6V5U5: case D3DFMT_R16F:
        return w * 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_A4L4: case D3DFMT_P8: case D3DFMT_R3G3B2:
        return w;
    case D3DFMT_DXT1:
        return ((w + 3) / 4) * 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        return ((w + 3) / 4) * 16;
    default:
        return 0;
    }
}
static uint32_t fmt_rows(uint32_t f, uint32_t h) { return fmt_dxt(f) ? (h + 3) / 4 : h; }

/* one 4x4 DXT block as sixteen A8R8G8B8 texels, row by row (the trace's
 * texture dump; the host itself samples DXT natively) */
static void dxt_block(uint32_t f, const uint8_t *b, uint32_t out[16]) {
    const uint8_t *cb = (f == D3DFMT_DXT1) ? b : b + 8;
    uint16_t c0 = (uint16_t)(cb[0] | cb[1] << 8), c1 = (uint16_t)(cb[2] | cb[3] << 8);
    uint32_t pal[4];
    auto rgb = [](uint16_t c) {
        uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
        return ((r << 3 | r >> 2) << 16) | ((g << 2 | g >> 4) << 8) | (bl << 3 | bl >> 2);
    };
    auto mix = [](uint32_t a, uint32_t b, uint32_t wa, uint32_t wb, uint32_t d) {
        uint32_t o = 0;
        for (int s = 0; s < 24; s += 8) o |= ((((a >> s) & 255) * wa + ((b >> s) & 255) * wb) / d) << s;
        return o;
    };
    pal[0] = rgb(c0) | 0xff000000u;
    pal[1] = rgb(c1) | 0xff000000u;
    if (f != D3DFMT_DXT1 || c0 > c1) {
        pal[2] = mix(pal[0], pal[1], 2, 1, 3) | 0xff000000u;
        pal[3] = mix(pal[0], pal[1], 1, 2, 3) | 0xff000000u;
    } else {
        pal[2] = mix(pal[0], pal[1], 1, 1, 2) | 0xff000000u;
        pal[3] = 0;                                     /* DXT1's transparent black */
    }
    uint32_t idx = (uint32_t)cb[4] | cb[5] << 8 | cb[6] << 16 | (uint32_t)cb[7] << 24;
    for (int i = 0; i < 16; i++) out[i] = pal[(idx >> (2 * i)) & 3];
    if (f == D3DFMT_DXT2 || f == D3DFMT_DXT3) {         /* explicit 4-bit alpha */
        for (int i = 0; i < 16; i++) {
            uint32_t a = (b[i / 2] >> (4 * (i & 1))) & 15;
            out[i] = (out[i] & 0xffffff) | (a * 17) << 24;
        }
    } else if (f == D3DFMT_DXT4 || f == D3DFMT_DXT5) {  /* two endpoints, 3-bit indices */
        uint32_t a[8] = { b[0], b[1] };
        if (a[0] > a[1]) for (int k = 1; k < 7; k++) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
        else { for (int k = 1; k < 5; k++) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5; a[6] = 0; a[7] = 255; }
        uint64_t ai = 0;
        for (int k = 0; k < 6; k++) ai |= (uint64_t)b[2 + k] << (8 * k);
        for (int i = 0; i < 16; i++) out[i] = (out[i] & 0xffffff) | a[(ai >> (3 * i)) & 7] << 24;
    }
}

/* formats texel_argb can expand that this host's DXVK may not take as they
 * are: R3G3B2 / A8R3G3B2 never (no Vulkan format), A4L4 only where the
 * device has the optional Vulkan format. Asked of the adapter once, when
 * the device is created (ensure_device); a refused one is expanded to
 * A8R8G8B8 at upload, like P8 */
static const D3DFORMAT expandable_fmts[] = { D3DFMT_L8, D3DFMT_A8L8, D3DFMT_A4L4, D3DFMT_A8, D3DFMT_R3G3B2, D3DFMT_A8R3G3B2 };
static bool host_lacks[256];
/* the system Direct3D 9 backend (Windows below the Vulkan 1.3 floor): a
 * real 2020s driver still lists D3DFMT_L6V5U5 (the 1999 Matrox bump
 * format) and no longer draws its luminance. Measured here on NVIDIA's
 * own d3d9 (RTX 3090): CheckDeviceFormat says yes, BUMPENVMAP
 * over it is right and BUMPENVMAPLUMINANCE comes out black, while
 * X8L8V8U8 (the same three channels at more precision) is right both
 * ways. So on that backend it goes up as X8L8V8U8, converted exactly,
 * rather than trusted. DXVK draws it and keeps it (patches/dxvk/07). */
static bool bump16_expand;

/* L6V5U5 -> X8L8V8U8: U bits 0..4 and V bits 5..9 are signed, L bits
 * 10..15 unsigned; the destination has U, V, L, X one byte each. */
static uint32_t l6v5u5_to_x8l8v8u8(uint32_t raw) {
    int u = (int)(raw & 0x1f), v = (int)((raw >> 5) & 0x1f), l = (int)((raw >> 10) & 0x3f);
    if (u & 0x10) u -= 32;
    if (v & 0x10) v -= 32;
    return ((uint32_t)(uint8_t)(int8_t)(u * 8)) | ((uint32_t)(uint8_t)(int8_t)(v * 8) << 8) |
           ((uint32_t)(uint8_t)((l << 2) | (l >> 4)) << 16);
}

/* the format the host texture is created in: P8 and colour-keyed textures
 * are expanded to A8R8G8B8 at upload (DXVK has no P8; the key becomes alpha
 * 0), and so is a format the host lacks. X4R4G4B4 always is: DXVK creates it
 * as VK_FORMAT_A4R4G4B4 with no swizzle, so the X nibble samples as alpha and
 * a texture written with it 0 draws transparent (FMTTEST) */
static D3DFORMAT host_format(const VramSurf &s) {
    if (s.d.format == D3DFMT_P8 || s.d.format == D3DFMT_X4R4G4B4) return D3DFMT_A8R8G8B8;
    if (bump16_expand && s.d.format == D3DFMT_L6V5U5) return D3DFMT_X8L8V8U8;
    if (s.d.format < 256 && host_lacks[s.d.format]) return D3DFMT_A8R8G8B8;
    if (s.ckey && !fmt_dxt(s.d.format) && fmt_row_bytes(s.d.format, 1) && s.d.format != D3DFMT_A8R8G8B8) return D3DFMT_A8R8G8B8;
    return (D3DFORMAT)s.d.format;
}
static bool needs_expand(const VramSurf &s) { return host_format(s) != (D3DFORMAT)s.d.format || s.ckey; }
/* a format with an alpha channel of its own (the DX3 MODULATE blend reads the alpha from such a texture, else from the diffuse) */
static bool fmt_has_alpha(uint32_t f) {
    switch (f) {
    case D3DFMT_A8R8G8B8: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_A8: case D3DFMT_A8R3G3B2:
    case D3DFMT_A2B10G10R10: case D3DFMT_A8B8G8R8: case D3DFMT_A2R10G10B10: case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_A4L4:
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        return true;
    default:
        return false;
    }
}

/* one texel of the VRAM formats the expansion handles, as A8R8G8B8 (raw = its VRAM value) */
static uint32_t texel_argb(uint32_t f, uint32_t raw, const Palette *pal, bool pal_alpha) {
    switch (f) {
    case D3DFMT_A8R8G8B8: return raw;
    case D3DFMT_X8R8G8B8: return raw | 0xff000000u;
    case D3DFMT_R5G6B5: return 0xff000000u | ((raw & 0xf800) << 8) | ((raw & 0xe000) << 3) | ((raw & 0x07e0) << 5) | ((raw & 0x0600) >> 1) | ((raw & 0x1f) << 3) | ((raw & 0x1c) >> 2);
    case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: {
        uint32_t c = ((raw & 0x7c00) << 9) | ((raw & 0x7000) << 4) | ((raw & 0x03e0) << 6) | ((raw & 0x0380) << 1) | ((raw & 0x1f) << 3) | ((raw & 0x1c) >> 2);
        return c | (f == D3DFMT_A1R5G5B5 && !(raw & 0x8000) ? 0u : 0xff000000u);
    }
    case D3DFMT_X4R4G4B4: case D3DFMT_A4R4G4B4: {
        uint32_t c = ((raw & 0xf00) << 12) | ((raw & 0xf00) << 8) | ((raw & 0x0f0) << 8) | ((raw & 0x0f0) << 4) | ((raw & 0x00f) << 4) | (raw & 0x00f);
        return c | (f == D3DFMT_A4R4G4B4 ? ((raw & 0xf000) << 16) | ((raw & 0xf000) << 12) : 0xff000000u);
    }
    case D3DFMT_P8: {
        uint32_t c = pal ? pal->argb[raw & 0xff] : (0xff000000u | (raw & 0xff) * 0x010101u);
        return pal && !pal_alpha ? c | 0xff000000u : c;
    }
    case D3DFMT_R3G3B2: case D3DFMT_A8R3G3B2: {
        uint32_t r = (raw >> 5) & 7, g = (raw >> 2) & 7, b = raw & 3;
        uint32_t c = (((r << 5) | (r << 2) | (r >> 1)) << 16) | (((g << 5) | (g << 2) | (g >> 1)) << 8) | (b * 0x55);
        return c | (f == D3DFMT_A8R3G3B2 ? (raw & 0xff00) << 16 : 0xff000000u);
    }
    case D3DFMT_L8: return 0xff000000u | (raw & 0xff) * 0x010101u;
    case D3DFMT_A8L8: return ((raw & 0xff00) << 16) | (raw & 0xff) * 0x010101u;
    case D3DFMT_A4L4: return ((raw & 0xf0) * 0x11u << 20) | (raw & 0x0f) * 0x111111u;
    case D3DFMT_A8: return (raw & 0xff) << 24;      /* no colour: D3D9 reads A8's as black */
    default: return 0xff000000u;
    }
}

/* -------------------------------------------------------------- surfaces */

static VramSurf *surf(Exec &x, uint32_t h) {
    if (!x.ddi || !h) return nullptr;
    auto it = x.ddi->surfs.find(h);
    return it == x.ddi->surfs.end() ? nullptr : &it->second;
}

static bool ensure_device(Exec &x, uint32_t w, uint32_t h) {
    if (x.dev) return true;
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof pp);
    pp.BackBufferWidth = w; pp.BackBufferHeight = h;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Windowed = TRUE;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *dev = nullptr;
    HRESULT hr = exec_create_device(x, 0, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, pp, &dev);
    x.log("ddi: device for %ux%u render targets -> 0x%08x", w, h, (unsigned)hr);
    if (FAILED(hr) || !dev) return false;
    x.dev = dev;
    bump16_expand = x.native;
    /* the texture formats this adapter lacks, expanded at upload (host_format) */
    char lacks[128] = "";
    for (D3DFORMAT f : expandable_fmts) {
        host_lacks[f] = FAILED(x.d3d->CheckDeviceFormat(0, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE, f));
        if (host_lacks[f]) snprintf(lacks + strlen(lacks), sizeof lacks - strlen(lacks), " %u", (unsigned)f);
    }
    if (lacks[0]) x.log("ddi: no host texture format%s: expanded to A8R8G8B8 at upload", lacks);
    if (x.ops.active) x.ops.active(x.ops.ud, 1);
    return true;
}

/* v13: the sample count a render target / depth buffer asks for
 * (D3DPT_VS_SAMPLES), when this host has it for the surface's format; 0 =
 * none, and then the target is drawn without (said once per count) */
static uint32_t target_samples(Exec &x, const VramSurf &s) {
    uint32_t n = (s.d.caps & D3DPT_VS_SAMPLES_MASK) >> D3DPT_VS_SAMPLES_SHIFT;
    if (n < 2 || (s.d.caps & D3DPT_VS_TEXTURE)) return 0;
    if (FAILED(x.d3d->CheckDeviceMultiSampleType(0, D3DDEVTYPE_HAL, (D3DFORMAT)s.d.format, TRUE, (D3DMULTISAMPLE_TYPE)n, nullptr))) {
        if (x.ddi && x.ddi->warn_once(0x80000 | n)) x.log("ddi: no %ux multisampling for format %u on this host: drawn without", n, s.d.format);
        return 0;
    }
    return n;
}

/* the host object behind a surface, created on first use */
static bool ensure_object(Exec &x, VramSurf &s) {
    if (s.tex || s.rt || s.cube || s.vol) return true;
    if (s.d.caps & D3DPT_VS_BUFFER) return false;       /* a vertex / index buffer: read from VRAM at each draw, no host object */
    if (s.mip_root) {
        /* a level of a render-target texture (v19): its surface of the
         * texture's host object */
        VramSurf *r = surf(x, s.mip_root);
        if (!r || !ensure_object(x, *r) || !r->tex || !(r->d.caps & D3DPT_VS_RENDER_TARGET)) return false;
        return SUCCEEDED(r->tex->GetSurfaceLevel(s.mip_level, &s.rt)) && s.rt;
    }
    if (s.cube_root) {
        /* a cube face (v11): its surface of a render-target cube's host
         * object; a plain cube's face has no object of its own */
        VramSurf *r = surf(x, s.cube_root);
        if (!r || !ensure_object(x, *r) || !r->cube || !(r->d.caps & D3DPT_VS_RENDER_TARGET)) return false;
        return SUCCEEDED(r->cube->GetCubeMapSurface((D3DCUBEMAP_FACES)s.face, 0, &s.rt)) && s.rt;
    }
    if (!ensure_device(x, s.d.width, s.d.height)) return false;
    HRESULT hr;
    if (s.d.caps & D3DPT_VS_VOLUME) {
        /* v12: a managed volume texture, uploaded slice by slice */
        s.host_fmt = host_format(s);
        hr = x.dev->CreateVolumeTexture(s.d.width, s.d.height, s.depth, s.d.levels, 0, s.host_fmt, D3DPOOL_MANAGED, &s.vol, nullptr);
        if (FAILED(hr)) x.log("ddi: volume texture %ux%ux%u fmt %u levels %u: 0x%08x", s.d.width, s.d.height, s.depth, s.d.format, s.d.levels, (unsigned)hr);
        return SUCCEEDED(hr);
    }
    if (s.d.caps & D3DPT_VS_CUBE) {
        /* v11: a render-target cube is a default-pool one of one level,
         * face 0's surface its own target; a plain one is managed */
        if (s.d.caps & D3DPT_VS_RENDER_TARGET) {
            hr = x.dev->CreateCubeTexture(s.d.width, 1, D3DUSAGE_RENDERTARGET, (D3DFORMAT)s.d.format, D3DPOOL_DEFAULT, &s.cube, nullptr);
            if (SUCCEEDED(hr)) hr = s.cube->GetCubeMapSurface(D3DCUBEMAP_FACE_POSITIVE_X, 0, &s.rt);
        } else {
            s.host_fmt = host_format(s);
            hr = x.dev->CreateCubeTexture(s.d.width, s.d.levels, 0, s.host_fmt, D3DPOOL_MANAGED, &s.cube, nullptr);
        }
        if (FAILED(hr)) x.log("ddi: cube texture %u fmt %u (host %u) levels %u%s: 0x%08x", s.d.width, s.d.format, s.host_fmt, s.d.levels,
                              (s.d.caps & D3DPT_VS_RENDER_TARGET) ? " (render target)" : "", (unsigned)hr);
        return SUCCEEDED(hr);
    }
    s.ms = target_samples(x, s);
    if (s.d.caps & D3DPT_VS_ZBUFFER) {
        static const uint32_t fallback[] = { D3DFMT_D24S8, D3DFMT_D24X8, D3DFMT_D16 };
        hr = x.dev->CreateDepthStencilSurface(s.d.width, s.d.height, (D3DFORMAT)s.d.format, (D3DMULTISAMPLE_TYPE)s.ms, 0, FALSE, &s.rt, nullptr);
        for (uint32_t i = 0; FAILED(hr) && i < 3; i++)
            hr = x.dev->CreateDepthStencilSurface(s.d.width, s.d.height, (D3DFORMAT)fallback[i], (D3DMULTISAMPLE_TYPE)s.ms, 0, FALSE, &s.rt, nullptr);
        if (FAILED(hr)) x.log("ddi: depth surface %ux%u fmt %u: 0x%08x", s.d.width, s.d.height, s.d.format, (unsigned)hr);
    } else if ((s.d.caps & D3DPT_VS_TEXTURE) && (s.d.caps & D3DPT_VS_RENDER_TARGET)) {
        /* render-to-texture: a default-pool render-target texture, level 0
         * is the target; an autogen one's levels are DXVK's (v15) */
        /* every level the guest's texture has (a sublevel no draw reached
         * reads black, as a new texture's memory does on the cards of the
         * era, Wine's test_generate_mipmap) */
        bool gen = (s.d.caps & D3DPT_VS_AUTOGEN) != 0;
        hr = x.dev->CreateTexture(s.d.width, s.d.height, gen ? 0 : s.d.levels, D3DUSAGE_RENDERTARGET | (gen ? D3DUSAGE_AUTOGENMIPMAP : 0),
                                  (D3DFORMAT)s.d.format, D3DPOOL_DEFAULT, &s.tex, nullptr);
        if (SUCCEEDED(hr)) hr = s.tex->GetSurfaceLevel(0, &s.rt);
        if (FAILED(hr)) x.log("ddi: render-target texture %ux%u fmt %u: 0x%08x", s.d.width, s.d.height, s.d.format, (unsigned)hr);
    } else if (s.d.caps & (D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY)) {
        hr = x.dev->CreateRenderTarget(s.d.width, s.d.height, (D3DFORMAT)s.d.format, (D3DMULTISAMPLE_TYPE)s.ms, 0, FALSE, &s.rt, nullptr);
        if (FAILED(hr)) x.log("ddi: render target %ux%u fmt %u (%u samples): 0x%08x", s.d.width, s.d.height, s.d.format, s.ms, (unsigned)hr);
        else if (s.ms) x.log("ddi: render target %u: %ux%u fmt %u, %u samples", s.d.handle, s.d.width, s.d.height, s.d.format, s.ms);
    } else {
        s.host_fmt = host_format(s);
        /* an autogen texture (v15): DXVK makes the levels below the one the
         * guest has, again after each upload */
        bool gen = (s.d.caps & D3DPT_VS_AUTOGEN) != 0;
        hr = x.dev->CreateTexture(s.d.width, s.d.height, gen ? 0 : s.d.levels, gen ? D3DUSAGE_AUTOGENMIPMAP : 0, s.host_fmt,
                                  D3DPOOL_MANAGED, &s.tex, nullptr);
        if (FAILED(hr)) x.log("ddi: texture %ux%u fmt %u (host %u) levels %u: 0x%08x", s.d.width, s.d.height, s.d.format, s.host_fmt, s.d.levels, (unsigned)hr);
    }
    return SUCCEEDED(hr);
}

/* the host texture of a surface whose expansion changed (a colour key set or
 * cleared): recreated in the new format on the next use */
static void refresh_object(VramSurf &s) {
    if ((s.tex || s.cube || s.vol) && !s.rt && s.host_fmt != host_format(s)) s.release();
    s.dirty = true;
}

static void copy_rows(void *dst, uint32_t dpitch, const void *src, uint32_t spitch, uint32_t row, uint32_t rows) {
    for (uint32_t y = 0; y < rows; y++)
        memcpy((uint8_t *)dst + (size_t)y * dpitch, (const uint8_t *)src + (size_t)y * spitch, row);
}

/* a system-memory + default-pool staging pair of the surface's size and format */
static bool ensure_stage(Exec &x, Ddi &d, uint32_t w, uint32_t h, D3DFORMAT fmt, bool with_default) {
    if (d.stage && (d.stage_w != w || d.stage_h != h || d.stage_fmt != fmt)) d.drop_stage();
    if (!d.stage) {
        if (FAILED(x.dev->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_SYSTEMMEM, &d.stage, nullptr))) return false;
        d.stage_w = w; d.stage_h = h; d.stage_fmt = fmt;
    }
    if (with_default && !d.stage_def &&
        FAILED(x.dev->CreateOffscreenPlainSurface(w, h, fmt, D3DPOOL_DEFAULT, &d.stage_def, nullptr))) return false;
    return true;
}

/* VRAM -> host texture (every level; a cube's every face, v11); a P8 or
 * colour-keyed texture is expanded texel by texel: the palette's colour,
 * alpha 0 for a keyed value */
static void upload_texture(Exec &x, Ddi &d, VramSurf &s) {
    D3DLOCKED_RECT lr;
    bool bump16 = s.host_fmt == D3DFMT_X8L8V8U8 && s.d.format == D3DFMT_L6V5U5;
    bool expand = needs_expand(s) && (s.host_fmt == D3DFMT_A8R8G8B8 || bump16);
    const Palette *pal = nullptr;
    if (s.d.format == D3DFMT_P8) {
        auto it = d.palettes.find(s.palette);
        if (it != d.palettes.end()) pal = &it->second;
    }
    if (s.vol) {
        /* v12: every level's slices (level 0's at the record's slice pitch,
         * the others one after the other at their own pitch * rows) */
        for (uint32_t l = 0; l < s.d.levels; l++) {
            uint32_t w = s.d.width >> l, h = s.d.height >> l, dep = s.depth >> l;
            if (!w) w = 1;
            if (!h) h = 1;
            if (!dep) dep = 1;
            uint32_t off = l ? s.levels[l - 1].a : s.d.offset, pitch = l ? s.levels[l - 1].b : s.d.pitch;
            uint32_t row = fmt_row_bytes(s.d.format, w), rows = fmt_rows(s.d.format, h);
            size_t slice = l ? (size_t)pitch * rows : s.slice;
            D3DLOCKED_BOX lb;
            if (FAILED(s.vol->LockBox(l, &lb, nullptr, 0))) continue;
            for (uint32_t z = 0; z < dep; z++)
                copy_rows((uint8_t *)lb.pBits + (size_t)z * lb.SlicePitch, lb.RowPitch, x.vram + off + z * slice, pitch,
                          row < (uint32_t)lb.RowPitch ? row : (uint32_t)lb.RowPitch, rows);
            s.vol->UnlockBox(l);
        }
        s.dirty = false;
        return;
    }
    uint32_t bpp = fmt_row_bytes(s.d.format, 1), faces = s.cube ? D3DPT_CUBE_FACES : 1;
    for (uint32_t i = 0; i < faces * s.d.levels; i++) {
        uint32_t f = i / s.d.levels, l = i % s.d.levels;    /* the level table is face-major */
        uint32_t w = s.d.width >> l, h = s.d.height >> l;
        if (!w) w = 1;
        if (!h) h = 1;
        uint32_t off = i ? s.levels[i - 1].a : s.d.offset, pitch = i ? s.levels[i - 1].b : s.d.pitch;
        uint32_t row = fmt_row_bytes(s.d.format, w), rows = fmt_rows(s.d.format, h);
        if (FAILED(s.cube ? s.cube->LockRect((D3DCUBEMAP_FACES)f, l, &lr, nullptr, 0) : s.tex->LockRect(l, &lr, nullptr, 0))) continue;
        if (!expand || (bpp != 1 && bpp != 2 && bpp != 4)) {
            copy_rows(lr.pBits, lr.Pitch, x.vram + off, pitch, row < (uint32_t)lr.Pitch ? row : (uint32_t)lr.Pitch, rows);
        } else {
            for (uint32_t yy = 0; yy < rows; yy++) {
                const uint8_t *src = x.vram + off + (size_t)yy * pitch;
                uint32_t *dst = (uint32_t *)((uint8_t *)lr.pBits + (size_t)yy * lr.Pitch);
                for (uint32_t xx = 0; xx < w; xx++) {
                    uint32_t raw;
                    if (bpp == 1) raw = src[xx];
                    else if (bpp == 2) { uint16_t v; memcpy(&v, src + xx * 2, 2); raw = v; }
                    else memcpy(&raw, src + xx * 4, 4);
                    uint32_t c = bump16 ? l6v5u5_to_x8l8v8u8(raw) : texel_argb(s.d.format, raw, pal, s.pal_alpha);
                    if (!bump16 && s.ckey && raw >= s.ckey_lo && raw <= s.ckey_hi) c &= 0x00ffffffu;
                    dst[xx] = c;
                }
            }
        }
        if (s.cube) s.cube->UnlockRect((D3DCUBEMAP_FACES)f, l);
        else s.tex->UnlockRect(l);
    }
    /* v15: an autogen texture's levels from the level 0 just written, as
     * Direct3D 9 makes them on its own (DXVK does not after an upload).
     * Windows' own d3d9 makes a managed texture's levels from its copy in
     * video memory, stale until the manager uploads the lock: PreLoad
     * first, or the levels are the old level 0's (D3DPT_D3D9=system) */
    if (s.d.caps & D3DPT_VS_AUTOGEN) {
        s.tex->PreLoad();
        s.tex->GenerateMipSubLevels();
    }
    s.dirty = false;
}

/* the target's VRAM rows into its shadow (host and VRAM agree from here) */
static void shadow_take(Exec &x, VramSurf &s) {
    uint32_t row = fmt_row_bytes(s.d.format, s.d.width);
    s.shadow.resize((size_t)row * s.d.height);
    copy_rows(s.shadow.data(), row, x.vram + s.d.offset, s.d.pitch, row, s.d.height);
}

/* the target's VRAM differs from its shadow: the guest wrote it without a
 * VRAM_DIRTY (counted in pixels; 0 when there is no shadow yet) */
static uint32_t shadow_diff(Exec &x, VramSurf &s) {
    uint32_t row = fmt_row_bytes(s.d.format, s.d.width), bpp = fmt_row_bytes(s.d.format, 1), n = 0;
    if (s.shadow.size() != (size_t)row * s.d.height || !bpp) return 0;
    for (uint32_t yy = 0; yy < s.d.height; yy++) {
        const uint8_t *v = x.vram + s.d.offset + (size_t)yy * s.d.pitch, *sh = s.shadow.data() + (size_t)yy * row;
        if (memcmp(v, sh, row) == 0) continue;
        for (uint32_t xx = 0; xx < row; xx += bpp) if (memcmp(v + xx, sh + xx, bpp) != 0) n++;
    }
    return n;
}

/* VRAM -> host render target (the guest drew into the target with GDI / the HEL) */
static void upload_target(Exec &x, Ddi &d, VramSurf &s) {
    /* the copies below are not made inside a scene: the frame the guest
     * is building closes here and opens again at its next draw */
    x.scene_end();
    D3DLOCKED_RECT lr;
    if (s.ms) {
        /* v13: nothing to upload. Direct3D 8 locks no multisampled surface,
         * so the guest cannot have written one (and d3d9 cannot stretch into it) */
        s.dirty = false;
        shadow_take(x, s);
        return;
    }
    if (!ensure_stage(x, d, s.d.width, s.d.height, (D3DFORMAT)s.d.format, true)) return;
    if (FAILED(d.stage->LockRect(&lr, nullptr, 0))) return;
    uint32_t row = fmt_row_bytes(s.d.format, s.d.width);
    copy_rows(lr.pBits, lr.Pitch, x.vram + s.d.offset, s.d.pitch, row, s.d.height);
    d.stage->UnlockRect();
    if (SUCCEEDED(x.dev->UpdateSurface(d.stage, nullptr, d.stage_def, nullptr)))
        x.dev->StretchRect(d.stage_def, nullptr, s.rt, nullptr, D3DTEXF_NONE);
    s.dirty = false;
    shadow_take(x, s);
}

/* the surface a readback reads: the target itself, or (v13) the plain
 * target a multisampled one is resolved into first, since d3d9 reads no
 * multisampled surface back */
static IDirect3DSurface9 *resolved(Exec &x, VramSurf &s) {
    x.scene_end();
    if (!s.ms) return s.rt;
    if (!s.resolve && FAILED(x.dev->CreateRenderTarget(s.d.width, s.d.height, (D3DFORMAT)s.d.format, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                       &s.resolve, nullptr))) return nullptr;
    HRESULT hr = x.dev->StretchRect(s.rt, nullptr, s.resolve, nullptr, D3DTEXF_NONE);
    if (FAILED(hr)) { x.log("ddi: resolve of target %u: StretchRect 0x%08x", s.d.handle, (unsigned)hr); return nullptr; }
    return s.resolve;
}

/* host render target -> VRAM. Pixels the guest changed since the shadow
 * was taken (untracked writes: GDI through GetDC, drawn after the scene
 * as a rule, like a title's text and panels) stay over the host's. */
static HRESULT readback(Exec &x, Ddi &d, VramSurf &s) {
    x.scene_end();
    if (!s.rt || (s.d.caps & D3DPT_VS_ZBUFFER)) return D3DERR_INVALIDCALL;
    if (!ensure_stage(x, d, s.d.width, s.d.height, (D3DFORMAT)s.d.format, false)) return E_FAIL;
    auto t0 = std::chrono::steady_clock::now();
    IDirect3DSurface9 *src = resolved(x, s);
    if (!src) return E_FAIL;
    HRESULT hr = x.dev->GetRenderTargetData(src, d.stage);
    if (FAILED(hr)) { x.log("ddi: readback: GetRenderTargetData 0x%08x", (unsigned)hr); return hr; }
    D3DLOCKED_RECT lr;
    if (FAILED(d.stage->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return E_FAIL;
    auto t1 = std::chrono::steady_clock::now();
    uint32_t row = fmt_row_bytes(s.d.format, s.d.width), bpp = fmt_row_bytes(s.d.format, 1), kept = 0;
    if (d.trace || d.readbacks < 5) {
        uint32_t px0 = *(const uint32_t *)lr.pBits;
        x.log("ddi: readback: %u (%ux%u fmt %u pitch %u) -> vram 0x%x (px0 0x%08x)",
              s.d.handle, s.d.width, s.d.height, s.d.format, s.d.pitch, s.d.offset, px0);
    }
    bool have = s.shadow.size() == (size_t)row * s.d.height && bpp;
    if (!have) s.shadow.resize((size_t)row * s.d.height);
    for (uint32_t yy = 0; yy < s.d.height; yy++) {
        uint8_t *v = x.vram + s.d.offset + (size_t)yy * s.d.pitch, *sh = s.shadow.data() + (size_t)yy * row;
        const uint8_t *h = (const uint8_t *)lr.pBits + (size_t)yy * lr.Pitch;
        if (have && memcmp(v, sh, row) != 0) {
            for (uint32_t xx = 0; xx < row; xx += bpp) {
                if (memcmp(v + xx, sh + xx, bpp) != 0) kept++;      /* the guest's pixel stays */
                else memcpy(v + xx, h + xx, bpp);
            }
        } else memcpy(v, h, row);
        memcpy(sh, v, row);
    }
    d.stage->UnlockRect();
    if (kept) {
        d.untracked += kept;
        if (d.untracked_lines < 8) { d.untracked_lines++; x.log("ddi: target %u: %u pixels written by the guest since the last frame without VRAM_DIRTY, kept over the host frame", s.d.handle, kept); }
        if (d.trace) x.log("ddi: trace: readback of %u keeps %u untracked guest pixels", s.d.handle, kept);
    }
    s.rendered = false;
    s.dirty = kept != 0;                /* the host frame lacks the kept pixels: refreshed from VRAM before the next draw */
    s.checked = false;
    d.readbacks++;
    auto now = std::chrono::steady_clock::now();
    d.rb_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - t0).count();
    d.rb_wait_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    if (d.stat_t0 == std::chrono::steady_clock::time_point{}) d.stat_t0 = now;
    double dt = std::chrono::duration<double>(now - d.stat_t0).count();
    if (dt >= 5.0) {
        char extra[128] = "";
        size_t n = 0;
        if (d.untracked != d.stat_untracked) n += (size_t)snprintf(extra + n, sizeof extra - n, ", %u untracked guest pixels", d.untracked - d.stat_untracked);
        if (d.buffer_writes != d.stat_bw) snprintf(extra + n, sizeof extra - n, ", %u buffer writes of %u KiB", d.buffer_writes - d.stat_bw, (d.buffer_bytes - d.stat_bb) >> 10);
        char ab[32] = "";
        if (d.flush_ab) snprintf(ab, sizeof ab, " [flush %u]", d.flush_draws);
        x.log("ddi: %.1f frames/s (%u readbacks, %u dp2 calls, %u draws%s in %.1f s; readbacks %.0f ms, %.0f of them waiting for the frame)%s",
              (d.readbacks - d.stat_rb) / dt, d.readbacks - d.stat_rb, d.dp2_calls - d.stat_dp2, d.draws - d.stat_draws, extra, dt,
              (d.rb_ns - d.stat_rb_ns) / 1e6, (d.rb_wait_ns - d.stat_rb_wait_ns) / 1e6,
              ab);
        if (d.flush_ab) std::swap(d.flush_draws, d.flush_alt);
        d.stat_rb_ns = d.rb_ns; d.stat_rb_wait_ns = d.rb_wait_ns;
        d.stat_t0 = now;
        d.stat_rb = d.readbacks; d.stat_dp2 = d.dp2_calls; d.stat_draws = d.draws; d.stat_untracked = d.untracked;
        d.stat_bw = d.buffer_writes; d.stat_bb = d.buffer_bytes;
    }
    if (x.ops.vram_dirty) x.ops.vram_dirty(x.ops.ud, s.d.offset, s.d.pitch * s.d.height);
    return S_OK;
}

/* set the context's targets on the device (and re-apply its viewport, which
 * SetRenderTarget resets); uploads a target the guest wrote since */
static bool bind_ctx(Exec &x, Ddi &d, Ctx &c, Batch &b, bool for_draw) {
    VramSurf *rt = surf(x, c.rt);
    if (!rt || !ensure_object(x, *rt) || !rt->rt) {
        if (d.warn_once(0x80000)) x.log("ddi: render target %u %s: the batch fails", c.rt, !rt ? "unknown" : "has no host object");
        b.err = D3DPT_ERR_BAD_HANDLE;
        return false;
    }
    VramSurf *z = c.z ? surf(x, c.z) : nullptr;
    if (c.z && (!z || !ensure_object(x, *z))) {
        if (d.warn_once(0x80001)) x.log("ddi: depth surface %u %s: the batch fails", c.z, !z ? "unknown" : "has no host object");
        b.err = D3DPT_ERR_BAD_HANDLE;
        return false;
    }
    if (d.bound_rt != c.rt || d.bound_z != c.z) {
        x.dev->SetRenderTarget(0, rt->rt);
        x.dev->SetDepthStencilSurface(z ? z->rt : nullptr);
        d.bound_rt = c.rt; d.bound_z = c.z;
        if (c.vp.Width && c.vp.Height) x.dev->SetViewport(&c.vp);
    }
    /* v17: targets 1..3, each where it still names a target (a context
     * switch leaves the other context's bound until this one says) */
    for (uint32_t i = 1; i < 4; i++) {
        VramSurf *m = c.mrt[i] ? surf(x, c.mrt[i]) : nullptr;
        if (m && (!ensure_object(x, *m) || !m->rt)) m = nullptr;
        uint32_t h = m ? c.mrt[i] : 0;
        if (d.bound_mrt[i] != h) {
            x.dev->SetRenderTarget(i, m ? m->rt : nullptr);
            d.bound_mrt[i] = h;
        }
        if (m && for_draw) {
            if (m->dirty) upload_target(x, d, *m);
            m->rendered = true;
            m->checked = true;
            if (m->d.caps & D3DPT_VS_AUTOGEN) m->mips_stale = true;
        }
    }
    if (for_draw) {
        /* once per frame, before its first draw: VRAM the guest changed
         * without telling (GDI on the surface's DC) goes into the target
         * like a tracked write would */
        if (!rt->dirty && !rt->checked) {
            uint32_t n = shadow_diff(x, *rt);
            if (n) {
                rt->dirty = true;
                d.untracked += n;
                if (d.untracked_lines < 8) { d.untracked_lines++; x.log("ddi: target %u: %u pixels written by the guest without VRAM_DIRTY, uploaded before the frame's draws", rt->d.handle, n); }
            }
        }
        rt->checked = true;
        if (rt->dirty) upload_target(x, d, *rt);
        rt->rendered = true;
        if (rt->d.caps & D3DPT_VS_AUTOGEN) rt->mips_stale = true;
    }
    return true;
}

/* ------------------------------------------------------------- DP2 state */

/* DX7 render states that exist in d3d9 under the same number pass through;
 * the DX5/6-era ones (texture handle, stipple, ROP, colour key, ...) have
 * no d3d9 equivalent and are dropped */
static bool rs_passthrough(uint32_t s) {
    static const uint8_t dropped[] = { 1, 2, 3, 4, 5, 6, 10, 11, 12, 13, 17, 18, 21, 30, 31, 32, 33, 39, 40, 41,
                                       43, 44, 45, 46, 47, 49, 50, 51, 138, 144,
                                       153, 164, 172, 173 };    /* d3d8: SOFTWAREVERTEXPROCESSING, PATCHSEGMENTS, POSITION/NORMALORDER */
    if (s >= 210 || (s >= 64 && s <= 95)) return false;
    for (uint8_t v : dropped) if (v == s) return false;
    return true;
}

static uint32_t stride_of_fvf(uint32_t fvf) {
    uint32_t n = 0, pos = fvf & 0xe;
    switch (pos) {
    case 0x2: n = 12; break;                    /* XYZ */
    case 0x4: n = 16; break;                    /* XYZRHW */
    case 0x6: n = 16; break;                    /* XYZB1 */
    case 0x8: n = 20; break;
    case 0xa: n = 24; break;
    case 0xc: n = 28; break;
    case 0xe: n = 32; break;
    default: return 0;
    }
    if (fvf & 0x10) n += 12;                     /* NORMAL */
    if (fvf & 0x20) n += 4;                      /* PSIZE */
    if (fvf & 0x40) n += 4;                      /* DIFFUSE */
    if (fvf & 0x80) n += 4;                      /* SPECULAR */
    uint32_t tex = (fvf >> 8) & 0xf;
    for (uint32_t i = 0; i < tex; i++) {
        switch ((fvf >> (16 + 2 * i)) & 3) {
        case 0: n += 8; break;
        case 1: n += 12; break;
        case 2: n += 16; break;
        case 3: n += 4; break;
        }
    }
    return n;
}

/* --- DX8 vertex shader declarations (D3DVSD_* tokens) -> d3d9 elements.
 * The register number of a D3DVSD_REG is the shader's input register and,
 * by the DX8 convention the fixed function relies on, names the usage
 * (0 position, 3 normal, 5 diffuse, 7.. texcoords); the d3d9 declaration
 * and the dcl instructions prepended to the function use the same table,
 * so a shader reading v5 finds the element declared as COLOR0. Data types
 * 0..7 are numbered as D3DDECLTYPE_*. --- */
static const struct { uint8_t usage, index; } vsde_usage[17] = {
    { D3DDECLUSAGE_POSITION, 0 }, { D3DDECLUSAGE_BLENDWEIGHT, 0 }, { D3DDECLUSAGE_BLENDINDICES, 0 }, { D3DDECLUSAGE_NORMAL, 0 },
    { D3DDECLUSAGE_PSIZE, 0 }, { D3DDECLUSAGE_COLOR, 0 }, { D3DDECLUSAGE_COLOR, 1 }, { D3DDECLUSAGE_TEXCOORD, 0 },
    { D3DDECLUSAGE_TEXCOORD, 1 }, { D3DDECLUSAGE_TEXCOORD, 2 }, { D3DDECLUSAGE_TEXCOORD, 3 }, { D3DDECLUSAGE_TEXCOORD, 4 },
    { D3DDECLUSAGE_TEXCOORD, 5 }, { D3DDECLUSAGE_TEXCOORD, 6 }, { D3DDECLUSAGE_TEXCOORD, 7 }, { D3DDECLUSAGE_POSITION, 1 }, { D3DDECLUSAGE_NORMAL, 1 },
};
static const uint8_t vsdt_size[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };

/* ntokens DWORDs of declaration; false = malformed (no END, a bad register
 * or type, too many elements). regs: bitmask of the input registers fed */
static bool vsd_convert(const uint32_t *d, uint32_t ntokens, std::vector<D3DVERTEXELEMENT9> &el, uint32_t &regs, VShader8 &s) {
    uint32_t i = 0, stream = 0, offset = 0;
    regs = 0; s.streams = 0;
    memset(s.stream_bytes, 0, sizeof s.stream_bytes);
    while (i < ntokens) {
        uint32_t t = d[i], type = (t >> 29) & 7;
        if (t == 0xFFFFFFFFu) {                                             /* D3DVSD_END */
            D3DVERTEXELEMENT9 end = D3DDECL_END();
            el.push_back(end);
            return true;
        }
        switch (type) {
        case 0: i++; break;                                                 /* NOP */
        case 1: stream = t & 0xF; offset = 0; i++; break;                  /* STREAM (the tessellator bit ignored) */
        case 2:
            if (t & (1u << 28)) { offset += 4 * ((t >> 16) & 0xF); i++; break; }   /* SKIP */
            {
                uint32_t reg = t & 0x1F, dt = (t >> 16) & 0xF;
                if (reg > 16 || dt > 7 || el.size() >= 64) return false;
                D3DVERTEXELEMENT9 e = { (WORD)stream, (WORD)offset, (BYTE)dt, D3DDECLMETHOD_DEFAULT, vsde_usage[reg].usage, vsde_usage[reg].index };
                el.push_back(e);
                regs |= 1u << reg;
                s.streams |= 1u << stream;
                offset += vsdt_size[dt];
                if (offset > s.stream_bytes[stream]) s.stream_bytes[stream] = offset;
                i++;
            }
            break;
        case 3: i++; break;                                                 /* TESSELLATOR: patches are not supported */
        case 4: {                                                           /* CONST: count float4s at register */
            uint32_t cnt = (t >> 25) & 0xF, reg = t & 0x7F;
            if (i + 1 + cnt * 4 > ntokens || reg + cnt > 256) return false;
            VShader8::ConstRun run; run.reg = reg;
            for (uint32_t j = 0; j < cnt * 4; j++) { float f; memcpy(&f, &d[i + 1 + j], 4); run.v.push_back(f); }
            if (cnt) s.consts.push_back(run);
            i += 1 + cnt * 4;
            break;
        }
        case 5: i += 1 + ((t >> 24) & 0x1F); break;                         /* EXT: skipped with its DWORDs */
        default: return false;
        }
    }
    return false;                                                           /* no END */
}

/* --- shader model 1.x bytecode validation. DXVK's compiler asserts on an
 * unknown opcode or a missing operand (an abort, uncatchable: QEMU dies),
 * so only a vs_1_x / ps_1_x stream of known instructions with the right
 * operand counts and registers in range reaches CreateVertexShader /
 * CreatePixelShader. The DX8 runtime validates every shader before its
 * CREATE token, so a real guest never trips this. Tokens: bit 31 set =
 * a parameter (register type in bits 28..30, number in 0..10), clear =
 * an instruction (opcode in bits 0..15); DEF carries four raw floats,
 * COMMENT its length. --- */
struct Sm1Op { uint16_t op; uint8_t vs, ps, min, max; };     /* stage allowed; operands including the destination */
static const Sm1Op sm1_ops[] = {
    { 0, 1, 1, 0, 0 },      /* NOP */
    { 1, 1, 1, 2, 2 },      /* MOV */
    { 2, 1, 1, 3, 3 },      /* ADD */
    { 3, 1, 1, 3, 3 },      /* SUB */
    { 4, 1, 1, 4, 4 },      /* MAD */
    { 5, 1, 1, 3, 3 },      /* MUL */
    { 6, 1, 0, 2, 2 },      /* RCP */
    { 7, 1, 0, 2, 2 },      /* RSQ */
    { 8, 1, 1, 3, 3 },      /* DP3 */
    { 9, 1, 1, 3, 3 },      /* DP4 */
    { 10, 1, 0, 3, 3 },     /* MIN */
    { 11, 1, 0, 3, 3 },     /* MAX */
    { 12, 1, 0, 3, 3 },     /* SLT */
    { 13, 1, 0, 3, 3 },     /* SGE */
    { 14, 1, 0, 2, 2 },     /* EXP */
    { 15, 1, 0, 2, 2 },     /* LOG */
    { 16, 1, 0, 2, 2 },     /* LIT */
    { 17, 1, 0, 3, 3 },     /* DST */
    { 18, 0, 1, 4, 4 },     /* LRP */
    { 19, 1, 0, 2, 2 },     /* FRC */
    { 20, 1, 0, 3, 3 },     /* M4x4 */
    { 21, 1, 0, 3, 3 },     /* M4x3 */
    { 22, 1, 0, 3, 3 },     /* M3x4 */
    { 23, 1, 0, 3, 3 },     /* M3x3 */
    { 24, 1, 0, 3, 3 },     /* M3x2 */
    { 64, 0, 1, 1, 2 },     /* TEXCOORD (ps 1.4 texcrd: + source) */
    { 65, 0, 1, 1, 1 },     /* TEXKILL */
    { 66, 0, 1, 1, 2 },     /* TEX (ps 1.4 texld: + source) */
    { 67, 0, 1, 2, 2 },     /* TEXBEM */
    { 68, 0, 1, 2, 2 },     /* TEXBEML */
    { 69, 0, 1, 2, 2 },     /* TEXREG2AR */
    { 70, 0, 1, 2, 2 },     /* TEXREG2GB */
    { 71, 0, 1, 2, 2 },     /* TEXM3x2PAD */
    { 72, 0, 1, 2, 2 },     /* TEXM3x2TEX */
    { 73, 0, 1, 2, 2 },     /* TEXM3x3PAD */
    { 74, 0, 1, 2, 2 },     /* TEXM3x3TEX */
    { 76, 0, 1, 3, 3 },     /* TEXM3x3SPEC */
    { 77, 0, 1, 2, 2 },     /* TEXM3x3VSPEC */
    { 78, 1, 0, 2, 2 },     /* EXPP */
    { 79, 1, 0, 2, 2 },     /* LOGP */
    { 80, 0, 1, 4, 4 },     /* CND */
    { 81, 1, 1, 5, 5 },     /* DEF: destination + four floats */
    { 82, 0, 1, 2, 2 },     /* TEXREG2RGB */
    { 83, 0, 1, 2, 2 },     /* TEXDP3TEX */
    { 84, 0, 1, 2, 2 },     /* TEXM3x2DEPTH */
    { 85, 0, 1, 2, 2 },     /* TEXDP3 */
    { 86, 0, 1, 2, 2 },     /* TEXM3x3 */
    { 87, 0, 1, 1, 1 },     /* TEXDEPTH */
    { 88, 0, 1, 4, 4 },     /* CMP */
    { 89, 0, 1, 3, 3 },     /* BEM */
};

/* a parameter token's register within the stage's file sizes */
static bool sm1_reg_ok(uint32_t t, bool vs, uint32_t minor) {
    uint32_t type = (t >> 28) & 7, num = t & 0x7ff;
    if (t & 0x1800) return false;                                   /* SM2 register type bits */
    if (vs) {
        switch (type) {
        case 0: return num < 12;                                    /* r */
        case 1: return num < 16;                                    /* v */
        case 2: return num < 256;                                   /* c (the runtime caps it at the driver's count) */
        case 3: return num < 1;                                     /* a0 */
        case 4: return num < 3;                                     /* oPos, oFog, oPts */
        case 5: return num < 2;                                     /* oD0, oD1 */
        case 6: return num < 8;                                     /* oT0..7 */
        default: return false;
        }
    }
    switch (type) {
    case 0: return num < (minor >= 4 ? 6u : 2u);                    /* r */
    case 1: return num < 2;                                         /* v */
    case 2: return num < 8;                                         /* c */
    case 3: return num < (minor >= 4 ? 6u : 4u);                    /* t */
    default: return false;
    }
}

static bool sm1_valid(const uint32_t *t, size_t n, bool vs) {
    if (n < 2 || (t[0] >> 16) != (vs ? 0xfffeu : 0xffffu) || ((t[0] >> 8) & 0xff) != 1 || t[n - 1] != 0x0000ffffu) return false;
    uint32_t minor = t[0] & 0xff;
    if (vs ? minor > 1 : minor > 4) return false;
    size_t i = 1, end = n - 1;
    while (i < end) {
        uint32_t ins = t[i];
        if (ins & 0x80000000u) return false;                        /* a parameter where an instruction should be */
        uint32_t op = ins & 0xffff;
        if (op == 0xfffe) {                                         /* COMMENT: its length in DWORDs */
            i += 1 + ((ins >> 16) & 0x7fff);
            if (i > end) return false;
            continue;
        }
        if (op == 0xfffd) {                                         /* PHASE: ps 1.4 */
            if (vs || minor != 4) return false;
            i++;
            continue;
        }
        if (op == 31) {                                             /* DCL: vs 1.1 as d3d9.dll passes it (M16) */
            /* a usage token, then the input register it names; d3d8.dll's
             * vs 1.1 has none (its declaration is a separate token stream) */
            if (!vs || i + 3 > end || !(t[i + 1] & 0x80000000u) || !(t[i + 2] & 0x80000000u) ||
                ((t[i + 2] >> 28) & 7) != 1 || !sm1_reg_ok(t[i + 2], vs, minor)) return false;
            i += 3;
            continue;
        }
        const Sm1Op *o = nullptr;
        for (const Sm1Op &e : sm1_ops) if (e.op == op) { o = &e; break; }
        if (!o || !(vs ? o->vs : o->ps)) return false;
        i++;
        if (op == 81) {                                             /* DEF: a constant register, then raw floats */
            if (i + 5 > end || !(t[i] & 0x80000000u) || ((t[i] >> 28) & 7) != 2 || !sm1_reg_ok(t[i], vs, minor)) return false;
            i += 5;
            continue;
        }
        uint32_t got = 0;
        while (i < end && (t[i] & 0x80000000u)) {
            if (!sm1_reg_ok(t[i], vs, minor)) return false;
            got++; i++;
        }
        if (got < o->min || got > o->max) return false;
        if (got && ((t[i - got] >> 28) & 7) == (vs ? 2u : 2u) && op != 0) return false;   /* a constant as the destination */
    }
    return i == end;
}

struct Dp2 {
    Exec &x; Ddi &d; Ctx &c; Batch &b;
    const uint8_t *cmd, *cmd_end, *vtx;
    uint32_t stride, nverts, fvf_;
    uint32_t cur_vs = ~0u;      /* the SETVERTEXSHADER value last applied (FVF or shader handle); ~0 = unknown */
    uint32_t cur_vfunc = ~0u;   /* the DX9 function applied with it (Ctx::vfunc); ~0 = unknown */
    uint32_t pos = 0;           /* offset of the current token, for dwErrorOffset */
    HRESULT hr = S_OK;

    uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }
    uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
    /* the last tokens parsed, printed with the first failure of a kind: a
     * mis-sized token shows up as garbage several tokens later */
    struct Tok { uint32_t pos, op, count; } hist[24];
    uint32_t nhist = 0;
    void log_hist() {
        char line[400]; size_t n = 0;
        for (uint32_t i = nhist > 24 ? nhist - 24 : 0; i < nhist && n < sizeof line - 24; i++) {
            const Tok &t = hist[i % 24];
            n += (size_t)snprintf(line + n, sizeof line - n, " %u:%ux%u", t.pos, t.op, t.count);
        }
        x.log("ddi: dp2:   tokens before it (offset:op x count):%s", line);
    }
    D3DPT_PRINTF(2, 3) void tr(const char *fmt, ...) {
        if (!d.trace) return;
        char buf[400]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
        x.log("ddi: trace: %6u %s", pos, buf);
    }
    /* every level of a traced texture, as tex-<handle>-l<n>.ppm (RGB) +
     * tex-<handle>-l<n>-a.pgm (alpha) next to the flag file, once per handle;
     * a cube's faces after the first as tex-<handle>-f<face>-l<n>; every
     * format texel_argb knows (P8 as a grey ramp) */
    void dump_texture(const VramSurf &s, uint32_t handle) {
        char path[512]; const char *slash = strrchr(d.trace_flag, '/');
        int dirlen = slash ? (int)(slash - d.trace_flag) : 1;
        const char *dir = slash ? d.trace_flag : ".";
        snprintf(path, sizeof path, "%.*s/tex-%u-l0.ppm", dirlen, dir, handle);
        if (file_exists(path)) return;   /* one dump per handle */
        uint32_t bpp = fmt_row_bytes(s.d.format, 1);
        uint32_t faces = (s.d.caps & D3DPT_VS_CUBE) ? D3DPT_CUBE_FACES : 1;
        for (uint32_t e = 0; e < faces * s.d.levels; e++) {
            uint32_t f = e / s.d.levels, l = e % s.d.levels;
            uint32_t w = s.d.width >> l, h = s.d.height >> l;
            if (!w) w = 1;
            if (!h) h = 1;
            if (e && e > s.levels.size()) break;
            uint32_t off = e ? s.levels[e - 1].a : s.d.offset, pitch = e ? s.levels[e - 1].b : s.d.pitch;
            if (f) snprintf(path, sizeof path, "%.*s/tex-%u-f%u-l%u.ppm", dirlen, dir, handle, f, l);
            else snprintf(path, sizeof path, "%.*s/tex-%u-l%u.ppm", dirlen, dir, handle, l);
            FILE *rgb = fopen(path, "wb");
            if (f) snprintf(path, sizeof path, "%.*s/tex-%u-f%u-l%u-a.pgm", dirlen, dir, handle, f, l);
            else snprintf(path, sizeof path, "%.*s/tex-%u-l%u-a.pgm", dirlen, dir, handle, l);
            FILE *al = fopen(path, "wb");
            if (!rgb || !al) { if (rgb) fclose(rgb); if (al) fclose(al); return; }
            fprintf(rgb, "P6\n%u %u\n255\n", w, h);
            fprintf(al, "P5\n%u %u\n255\n", w, h);
            if (fmt_dxt(s.d.format)) {          /* decoded block by block: what the guest wrote, not the host copy */
                std::vector<uint32_t> px((size_t)w * h);
                for (uint32_t by = 0; by < (h + 3) / 4; by++)
                    for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
                        uint32_t blk[16];
                        dxt_block(s.d.format, x.vram + off + (size_t)by * pitch + bx * bpp, blk);   /* bpp: one block's bytes */
                        for (uint32_t i = 0; i < 16; i++) {
                            uint32_t xx = bx * 4 + (i & 3), yy = by * 4 + i / 4;
                            if (xx < w && yy < h) px[(size_t)yy * w + xx] = blk[i];
                        }
                    }
                for (uint32_t c : px) {
                    uint8_t o[4] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 24) };
                    fwrite(o, 1, 3, rgb); fwrite(o + 3, 1, 1, al);
                }
                fclose(rgb); fclose(al);
                continue;
            }
            for (uint32_t yy = 0; yy < h; yy++) {
                const uint8_t *row = x.vram + off + (size_t)yy * pitch;
                for (uint32_t xx = 0; xx < w; xx++) {
                    uint32_t raw = 0;
                    if (bpp == 4) memcpy(&raw, row + xx * 4, 4);
                    else if (bpp == 2) { uint16_t v; memcpy(&v, row + xx * 2, 2); raw = v; }
                    else if (bpp == 1) raw = row[xx];
                    uint32_t c = texel_argb(s.d.format, raw, nullptr, false);
                    uint8_t px[4] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c, (uint8_t)(c >> 24) };
                    fwrite(px, 1, 3, rgb); fwrite(px + 3, 1, 1, al);
                }
            }
            fclose(rgb); fclose(al);
        }
    }
    /* trace: the first three vertices of a draw (position, colours, two uv sets) */
    void trv(const uint8_t *v, uint32_t n, uint32_t st = 0, uint32_t fvf = 0) {
        if (!st) st = stride;
        if (!fvf) fvf = fvf_;
        for (uint32_t i = 0; i < n && i < 3; i++, v += st) {
            /* position (3 or 4 floats), blend weights, normal, then the colours and uv sets */
            uint32_t pos = fvf & 0xe, npos = pos == 0x4 ? 4 : pos == 0x2 ? 3 : 3 + (pos - 4) / 2;
            float f[4] = { 0, 0, 0, 0 }; memcpy(f, v, npos > 4 ? 16 : npos * 4);
            const uint8_t *q = v + npos * 4;
            float nrm[3] = { 0, 0, 0 };
            if (fvf & 0x10) { memcpy(nrm, q, 12); q += 12; }
            if (fvf & 0x20) q += 4;
            uint32_t col = 0, spec = 0;
            if (fvf & 0x40) { memcpy(&col, q, 4); q += 4; }
            if (fvf & 0x80) { memcpy(&spec, q, 4); q += 4; }
            float uv[4] = { 0, 0, 0, 0 }; uint32_t nt = (fvf >> 8) & 0xf;
            if (nt > 0) memcpy(uv, q, 8);
            if (nt > 1) memcpy(uv + 2, q + 8, 8);
            if (pos == 0x4)
                tr("    v%u %.1f,%.1f z %.4f rhw %.5f diffuse %08x specular %08x uv0 %.3f,%.3f uv1 %.3f,%.3f", i, f[0], f[1], f[2], f[3], col, spec, uv[0], uv[1], uv[2], uv[3]);
            else
                tr("    v%u %.3f,%.3f,%.3f n %.2f,%.2f,%.2f diffuse %08x specular %08x uv0 %.3f,%.3f uv1 %.3f,%.3f", i, f[0], f[1], f[2], nrm[0], nrm[1], nrm[2], col, spec, uv[0], uv[1], uv[2], uv[3]);
        }
    }
    /* SETVERTEXSHADER: an FVF (the fixed function on it), or a shader
     * handle (bit 0): its declaration, its function or none (the fixed
     * function on the declaration), its declaration constants */
    void apply_vs(uint32_t h) {
        if (h == cur_vs && (!c.dx9 || c.vfunc == cur_vfunc)) return;
        auto it = c.vshaders.find(h);
        if (it != c.vshaders.end()) {
            VShader8 &s = it->second;
            x.dev->SetVertexDeclaration(s.decl);
            x.dev->SetVertexShader(s.vs);
            for (const auto &r : s.consts) x.dev->SetVertexShaderConstantF(r.reg, r.v.data(), (UINT)(r.v.size() / 4));
            apply_clip(s.pos_t);
        } else if (!(h & 1)) {
            x.dev->SetVertexShader(nullptr);
            x.dev->SetFVF(h);
            apply_clip((h & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
        } else {
            if (d.warn_once(0xc0000)) x.log("ddi: dp2: vertex shader handle 0x%x unknown (draws with it are skipped)", h);
            return;
        }
        cur_vs = h;
        if (c.dx9) {
            /* DX9: the declaration (or FVF) above, the current function over it */
            auto f = c.vfuncs.find(c.vfunc);
            x.dev->SetVertexShader(f != c.vfuncs.end() ? f->second : nullptr);
            cur_vfunc = c.vfunc;
        }
    }
    /* CREATEVERTEXSHADERDECL (DX9): the elements as they are (END added if
     * the runtime's count leaves it out), each checked against d3d9's
     * ranges before DXVK sees it; what each stream's vertex must cover, for
     * the draw's checks and the interleaving of several streams */
    void create_vdecl(uint32_t handle, const uint8_t *e, uint32_t n) {
        static const uint8_t type_bytes[17] = { 4, 8, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8 };
        delete_vshader(handle);
        VShader8 s;
        std::vector<D3DVERTEXELEMENT9> el;
        for (uint32_t i = 0; i < n; i++) {
            D3DVERTEXELEMENT9 v;
            memcpy(&v, e + 8 * i, sizeof v);
            if (v.Stream == 0xff) break;                /* D3DDECL_END */
            if (v.Stream >= D3DPT_DRAW8_MAX_STREAMS || v.Type > 16 || v.Method > 6 || v.Usage > 13 || v.UsageIndex > 15 || v.Offset > 1024) {
                if (d.warn_once(0xc1010)) x.log("ddi: dp2: vertex declaration 0x%x: element %u out of range (stream %u offset %u type %u method %u usage %u.%u), refused",
                                                handle, i, v.Stream, v.Offset, v.Type, v.Method, v.Usage, v.UsageIndex);
                return;
            }
            el.push_back(v);
            if (v.Usage == D3DDECLUSAGE_POSITIONT) s.pos_t = true;
            uint32_t end = v.Offset + type_bytes[v.Type];
            if (end > s.stream_bytes[v.Stream]) s.stream_bytes[v.Stream] = end;
            s.streams |= 1u << v.Stream;
        }
        el.push_back(D3DDECL_END());
        HRESULT hr = x.dev->CreateVertexDeclaration(el.data(), &s.decl);
        if (FAILED(hr) || !s.decl) {
            if (d.warn_once(0xc1011)) x.log("ddi: dp2: vertex declaration 0x%x: CreateVertexDeclaration 0x%08x (%zu elements)", handle, (unsigned)hr, el.size() - 1);
            return;
        }
        tr("vertex declaration 0x%x created: %zu elements, streams 0x%x, %u bytes on stream 0", handle, el.size() - 1, s.streams, s.stream_bytes[0]);
        s.el = el;
        c.vshaders[handle] = s;
    }
    /* CREATEVERTEXSHADERFUNC (DX9): vs 1.1 to 3.0 as they are. 1.x is
     * checked as ever; 2.0 and 3.0 get the version and END checks only
     * (no SM2/3 validator for v1, user decision) */
    void create_vfunc(uint32_t handle, const uint8_t *code, uint32_t codebytes) {
        delete_vfunc(handle);
        uint32_t ver = codebytes >= 4 ? u32(code) : 0;
        if (!handle || codebytes < 8 || codebytes % 4 || codebytes > (256u << 10) || ver >> 16 != 0xfffe ||
            (ver & 0xffff) > 0x0300 || u32(code + codebytes - 4) != 0x0000ffffu) {
            if (d.warn_once(0xc1012)) x.log("ddi: dp2: vertex shader function 0x%x refused: version 0x%08x, %u bytes", handle, ver, codebytes);
            return;
        }
        std::vector<uint32_t> g(codebytes / 4);
        memcpy(g.data(), code, codebytes);
        if ((ver & 0xff00) < 0x0200 && !sm1_valid(g.data(), g.size(), true)) {
            if (d.warn_once(0xc1013)) x.log("ddi: dp2: vertex shader function 0x%x is not valid vs 1.x (version 0x%08x), refused", handle, ver);
            return;
        }
        IDirect3DVertexShader9 *vs = nullptr;
        HRESULT hr = x.dev->CreateVertexShader((const DWORD *)g.data(), &vs);
        if (FAILED(hr) || !vs) {
            if (d.warn_once(0xc1014)) x.log("ddi: dp2: vertex shader function 0x%x: CreateVertexShader 0x%08x (version 0x%08x, %u bytes)", handle, (unsigned)hr, ver, codebytes);
            return;
        }
        tr("vertex shader function 0x%x created: version 0x%08x, %u bytes", handle, ver, codebytes);
        c.vfuncs[handle] = vs;
    }
    void delete_vfunc(uint32_t h) {
        auto it = c.vfuncs.find(h);
        if (it == c.vfuncs.end()) return;
        if (h == cur_vfunc) { x.dev->SetVertexShader(nullptr); cur_vs = ~0u; cur_vfunc = ~0u; }
        if (it->second) it->second->Release();
        c.vfuncs.erase(it);
    }
    /* CREATEVERTEXSHADER: handle, the declaration tokens, the function (may be empty) */
    void create_vshader(uint32_t handle, const uint8_t *decl, uint32_t declbytes, const uint8_t *code, uint32_t codebytes) {
        auto old = c.vshaders.find(handle);
        if (old != c.vshaders.end()) { old->second.release(); c.vshaders.erase(old); }
        if (handle == cur_vs) cur_vs = ~0u;
        if (!handle || declbytes < 4 || declbytes % 4 || declbytes > (64u << 10) || codebytes % 4 || codebytes > (256u << 10) ||
            (codebytes && (codebytes < 8 || u32(code) >> 16 != 0xfffe || u32(code + codebytes - 4) != 0x0000ffffu))) {
            if (d.warn_once(0xc1000)) x.log("ddi: dp2: vertex shader 0x%x refused: declaration %u bytes, function %u bytes", handle, declbytes, codebytes);
            return;
        }
        std::vector<uint32_t> dt(declbytes / 4);
        memcpy(dt.data(), decl, declbytes);
        VShader8 s;
        std::vector<D3DVERTEXELEMENT9> el;
        uint32_t regs = 0;
        if (!vsd_convert(dt.data(), (uint32_t)dt.size(), el, regs, s)) {
            if (d.warn_once(0xc1001)) x.log("ddi: dp2: vertex shader 0x%x: malformed declaration (%u tokens)", handle, declbytes / 4);
            return;
        }
        HRESULT hr = x.dev->CreateVertexDeclaration(el.data(), &s.decl);
        if (FAILED(hr) || !s.decl) {
            if (d.warn_once(0xc1002)) x.log("ddi: dp2: vertex shader 0x%x: CreateVertexDeclaration 0x%08x (%zu elements)", handle, (unsigned)hr, el.size() - 1);
            return;
        }
        if (codebytes) {
            std::vector<uint32_t> g(codebytes / 4);
            memcpy(g.data(), code, codebytes);
            if (!sm1_valid(g.data(), g.size(), true)) {
                if (d.warn_once(0xc1008)) x.log("ddi: dp2: vertex shader 0x%x: the function is not valid vs 1.x (version 0x%08x, %u bytes), refused", handle, u32(code), codebytes);
                s.release();
                return;
            }
            /* a dcl per input register the declaration feeds, in front of the
             * function (d3d9 wants them; DX8 shaders have none) */
            std::vector<uint32_t> f;
            f.push_back(u32(code));
            for (uint32_t r = 0; r <= 16; r++) if (regs & (1u << r)) {
                f.push_back(0x0000001Fu);
                f.push_back(0x80000000u | vsde_usage[r].usage | ((uint32_t)vsde_usage[r].index << 16));
                f.push_back(0x900F0000u | r);
            }
            size_t body = f.size();
            for (uint32_t i = 4; i < codebytes; i += 4) f.push_back(u32(code + i));
            /* DX8's scalar instructions (rcp, rsq, exp, log, expp, logp:
             * opcodes 6, 7, 14, 15, 78, 79) read the source's w when it has
             * no swizzle; d3d9 wants a replicate swizzle and otherwise reads
             * x. The source gets .wwww, as DXVK's own d3d8 does (Wine's
             * test_scalar_instructions: logp of (0.25, 0.5, 1, 2) drew black) */
            for (size_t i = body; i + 2 < f.size();) {
                uint32_t op = f[i] & 0xffff;
                if (op == 0xfffe) { i += 1 + ((f[i] >> 16) & 0x7fff); continue; }
                if (op == 81) { i += 6; continue; }                 /* DEF: a register, then four raw floats */
                if (!(f[i] & 0x80000000u) && (op == 6 || op == 7 || op == 14 || op == 15 || op == 78 || op == 79) &&
                    (f[i + 2] & 0x00ff0000u) == 0x00e40000u)
                    f[i + 2] |= 0x00ff0000u;
                i++;
                while (i < f.size() && (f[i] & 0x80000000u)) i++;
            }
            hr = x.dev->CreateVertexShader((const DWORD *)f.data(), &s.vs);
            if (FAILED(hr) || !s.vs) {
                if (d.warn_once(0xc1003)) x.log("ddi: dp2: vertex shader 0x%x: CreateVertexShader 0x%08x (version 0x%08x, %u bytes)", handle, (unsigned)hr, u32(code), codebytes);
                s.release();
                return;
            }
        }
        tr("vertex shader 0x%x created: %zu elements, %u bytes on stream 0, streams 0x%x, %s function, %zu constant runs",
           handle, el.size() - 1, s.stream_bytes[0], s.streams, codebytes ? "a" : "no", s.consts.size());
        s.el = el;
        c.vshaders[handle] = s;
    }
    void create_pshader(uint32_t handle, const uint8_t *code, uint32_t codebytes) {
        auto old = c.pshaders.find(handle);
        if (old != c.pshaders.end()) { if (old->second) old->second->Release(); c.pshaders.erase(old); }
        if (!handle || codebytes < 8 || codebytes % 4 || codebytes > (256u << 10) || u32(code) >> 16 != 0xffff ||
            (u32(code) & 0xffff) > 0x0300 || u32(code + codebytes - 4) != 0x0000ffffu) {
            if (d.warn_once(0xc1004)) x.log("ddi: dp2: pixel shader 0x%x refused: %u bytes", handle, codebytes);
            return;
        }
        std::vector<uint32_t> f(codebytes / 4);
        memcpy(f.data(), code, codebytes);
        /* ps 2.0 / 3.0 (the DX9 face, M16): the checks above only */
        if ((u32(code) & 0xff00) < 0x0200 && !sm1_valid(f.data(), f.size(), false)) {
            if (d.warn_once(0xc1009)) x.log("ddi: dp2: pixel shader 0x%x is not valid ps 1.x (version 0x%08x, %u bytes), refused", handle, u32(code), codebytes);
            return;
        }
        IDirect3DPixelShader9 *ps = nullptr;
        HRESULT hr = x.dev->CreatePixelShader((const DWORD *)f.data(), &ps);
        if (FAILED(hr) || !ps) {
            if (d.warn_once(0xc1005)) x.log("ddi: dp2: pixel shader 0x%x: CreatePixelShader 0x%08x (version 0x%08x, %u bytes)", handle, (unsigned)hr, u32(code), codebytes);
            return;
        }
        tr("pixel shader 0x%x created: version 0x%08x, %u bytes", handle, u32(code), codebytes);
        c.pshaders[handle] = ps;
    }
    void set_pshader(uint32_t h) {
        IDirect3DPixelShader9 *ps = nullptr;
        if (h) {
            auto it = c.pshaders.find(h);
            if (it != c.pshaders.end()) ps = it->second;
            else if (d.warn_once(0xc1006)) x.log("ddi: dp2: pixel shader handle 0x%x unknown (fixed function instead)", h);
        }
        tr("pixel shader 0x%x%s", h, h && !ps ? " (unknown)" : "");
        x.dev->SetPixelShader(ps);
    }
    void delete_vshader(uint32_t h) {
        auto it = c.vshaders.find(h);
        if (it == c.vshaders.end()) return;
        if (h == cur_vs) { x.dev->SetVertexShader(nullptr); cur_vs = ~0u; }
        it->second.release();
        c.vshaders.erase(it);
    }
    void delete_pshader(uint32_t h) {
        auto it = c.pshaders.find(h);
        if (it == c.pshaders.end()) return;
        x.dev->SetPixelShader(nullptr);         /* it may be the current one; the runtime sets another before drawing */
        if (it->second) it->second->Release();
        c.pshaders.erase(it);
    }
    static uint32_t prim_verts(uint32_t t, uint32_t count) {
        switch (t) {
        case D3DPT_POINTLIST: return count;
        case D3DPT_LINELIST: return count * 2;
        case D3DPT_LINESTRIP: return count + 1;
        case D3DPT_TRIANGLELIST: return count * 3;
        case D3DPT_TRIANGLESTRIP: case D3DPT_TRIANGLEFAN: return count + 2;
        default: return 0;
        }
    }
    /* v9: a range of a VRAM buffer (D3DPT_VS_BUFFER) named by a DRAW8, or
     * null (the draw is skipped, once logged) */
    const uint8_t *vram_range(uint32_t handle, uint32_t off, size_t bytes, const char *what) {
        VramSurf *s = surf(x, handle);
        if (!s || !(s->d.caps & D3DPT_VS_BUFFER)) {
            if (d.warn_once(0xa0100)) x.log("ddi: dp2: draw8 %s buffer %u unknown%s (draw skipped)", what, handle, s ? " (not a buffer)" : "");
            return nullptr;
        }
        if (off > s->d.width || bytes > s->d.width - off) {
            if (d.warn_once(0xa0101)) x.log("ddi: dp2: draw8 %s range %u+%zu beyond buffer %u (%u bytes; draw skipped)", what, off, bytes, handle, s->d.width);
            return nullptr;
        }
        return x.vram + s->d.offset + off;
    }
    /* v10: a shader's declaration with every stream it reads interleaved
     * into stream 0 at these strides (each element at its stream's base,
     * the strides of the streams before it added up), so a multi-stream
     * draw is one vertex again and takes the same DrawPrimitiveUP path */
    IDirect3DVertexDeclaration9 *ilv_decl(VShader8 &s, const uint32_t *ss) {
        uint32_t key[D3DPT_DRAW8_MAX_STREAMS] = {}, base[D3DPT_DRAW8_MAX_STREAMS] = {}, total = 0;
        for (uint32_t i = 0; i < D3DPT_DRAW8_MAX_STREAMS; i++)
            if (s.streams & (1u << i)) { key[i] = ss[i]; base[i] = total; total += ss[i]; }
        for (const VShader8::Ilv &e : s.ilv) if (!memcmp(e.stride, key, sizeof key)) return e.decl;
        std::vector<D3DVERTEXELEMENT9> el = s.el;
        for (D3DVERTEXELEMENT9 &e : el)
            if (e.Stream != 0xFF) { e.Offset = (WORD)(e.Offset + base[e.Stream]); e.Stream = 0; }
        IDirect3DVertexDeclaration9 *decl = nullptr;
        if (FAILED(x.dev->CreateVertexDeclaration(el.data(), &decl)) || !decl) return nullptr;
        if (s.ilv.size() >= 8) { s.ilv.front().decl->Release(); s.ilv.erase(s.ilv.begin()); }
        VShader8::Ilv n;
        memcpy(n.stride, key, sizeof key);
        n.decl = decl;
        s.ilv.push_back(n);
        return decl;
    }
    /* the streams a declaration reads interleaved into d.ilv, vertex by
     * vertex; an instance stream (sdiv, v16) gives every vertex its element
     * k / divider of instance k */
    void ilv_fill(uint32_t streams, const uint8_t *const *sd, const uint32_t *ss, const uint32_t *sdiv, uint32_t nverts, uint32_t k) {
        uint8_t *o = d.ilv.data();
        for (uint32_t v = 0; v < nverts; v++)
            for (uint32_t i = 0; i < D3DPT_DRAW8_MAX_STREAMS; i++)
                if (streams & (1u << i)) {
                    memcpy(o, sd[i] + (size_t)(sdiv[i] ? k / sdiv[i] : v) * ss[i], ss[i]);
                    o += ss[i];
                }
    }
    /* the driver's self-contained DX8 draw: vertices and 16-bit indices
     * inline, or (v9) in VRAM buffers it names by handle and offset; (v10)
     * under a vertex shader, the other streams it had bound after them */
    bool draw8(const uint8_t *q, size_t left, size_t &need) {
        d3dpt_dp2_draw8 h;
        if (left < sizeof h) return fail("truncated DRAW8");
        memcpy(&h, q, sizeof h);
        if (h.flags & ~(D3DPT_DRAW8_VRAM_VB | D3DPT_DRAW8_VRAM_IB | D3DPT_DRAW8_STREAMS)) return fail("bad DRAW8 flags");
        bool ext_vb = (h.flags & D3DPT_DRAW8_VRAM_VB) != 0, ext_ib = (h.flags & D3DPT_DRAW8_VRAM_IB) != 0;
        size_t vb = ext_vb ? 8 : ((size_t)h.nverts * h.stride + 3) & ~(size_t)3;
        size_t ib = !h.nindices ? 0 : ext_ib ? 8 : ((size_t)h.nindices * 2 + 3) & ~(size_t)3;
        need = sizeof h + vb + ib;
        if (need > left) return fail("truncated DRAW8 data");
        /* the other streams, all parsed before anything can skip the draw
         * (the next token starts after the last of them) */
        struct Ext { uint32_t stream, stride, flags, elems, div; const uint8_t *p; } ext[D3DPT_DRAW8_MAX_STREAMS];
        uint32_t next = 0, inst = 0;
        if (h.flags & D3DPT_DRAW8_STREAMS) {
            if (need + 8 > left) return fail("truncated DRAW8 streams");
            uint32_t n = u32(q + need), freq0 = u32(q + need + 4), last = 0;
            need += 8;
            /* v16: instanced, stream 0 INDEXEDDATA | n on an indexed draw */
            if (freq0) {
                inst = freq0 & 0x3fffffffu;
                if ((freq0 & 0xc0000000u) != D3DSTREAMSOURCE_INDEXEDDATA || !inst || inst > 0x10000 || !h.nindices)
                    return fail("bad DRAW8 instance count");
            }
            if ((!n && !inst) || n >= D3DPT_DRAW8_MAX_STREAMS) return fail("bad DRAW8 stream count");
            for (uint32_t i = 0; i < n; i++) {
                d3dpt_dp2_draw8_stream e;
                if (need + sizeof e > left) return fail("truncated DRAW8 stream");
                memcpy(&e, q + need, sizeof e);
                need += sizeof e;
                if (e.stream <= last || e.stream >= D3DPT_DRAW8_MAX_STREAMS || !e.stride || e.stride > 1024 || (e.flags & ~D3DPT_DRAW8_VRAM_VB))
                    return fail("bad DRAW8 stream");
                /* v16: an instance stream, ceil(inst / d) elements */
                uint32_t div = 0, elems = 0;
                if (e.freq) {
                    div = e.freq & 0x3fffffffu;
                    if (!inst || (e.freq & 0xc0000000u) != D3DSTREAMSOURCE_INSTANCEDATA || !div) return fail("bad DRAW8 stream frequency");
                    elems = (inst + div - 1) / div;
                }
                last = e.stream;
                size_t sz = (e.flags & D3DPT_DRAW8_VRAM_VB) ? 8 : ((size_t)(elems ? elems : h.nverts) * e.stride + 3) & ~(size_t)3;
                if (need + sz > left) return fail("truncated DRAW8 stream data");
                ext[next++] = { e.stream, e.stride, e.flags, elems, div, q + need };
                need += sz;
            }
        }
        bool shader = (h.fvf & 1) != 0;
        uint32_t st = shader ? 0 : stride_of_fvf(h.fvf);
        if ((!shader && (!st || st > h.stride)) || h.stride > 1024 || h.nverts > 0x10000 || h.nindices > 0x100000 || h.prim_type < 1 || h.prim_type > 6)
            return fail("bad DRAW8");
        const uint8_t *vd = q + sizeof h, *id = vd + vb;
        D3DPRIMITIVETYPE t = (D3DPRIMITIVETYPE)h.prim_type;
        uint32_t nv = prim_verts(t, h.prim_count);
        tr("draw8 type %u prims %u %s 0x%x stride %u vertices %u%s indices %u%s (min %u), %u more streams", h.prim_type, h.prim_count, shader ? "shader" : "fvf", h.fvf, h.stride,
           h.nverts, ext_vb ? " (vram)" : "", h.nindices, ext_ib && h.nindices ? " (vram)" : "", h.min_index, next);
        if (ext_vb) {
            vd = vram_range(u32(q + sizeof h), u32(q + sizeof h + 4), (size_t)h.nverts * h.stride, "vertex");
            if (!vd) return true;
        }
        if (h.nindices && ext_ib) {
            id = vram_range(u32(q + sizeof h + vb), u32(q + sizeof h + vb + 4), (size_t)h.nindices * 2, "index");
            if (!id) return true;
        }
        /* every stream the draw carried: stream 0 above, the others where
         * their VRAM range resolves (one that does not only matters if the
         * declaration reads it) */
        const uint8_t *sd[D3DPT_DRAW8_MAX_STREAMS] = { vd };
        uint32_t ss[D3DPT_DRAW8_MAX_STREAMS] = { h.stride }, sdiv[D3DPT_DRAW8_MAX_STREAMS] = {}, carried = 1;
        for (uint32_t i = 0; i < next; i++) {
            const uint8_t *p = ext[i].p;
            if (ext[i].flags & D3DPT_DRAW8_VRAM_VB)
                p = vram_range(u32(p), u32(p + 4), (size_t)(ext[i].elems ? ext[i].elems : h.nverts) * ext[i].stride, "stream");
            if (!p) continue;
            sd[ext[i].stream] = p;
            ss[ext[i].stream] = ext[i].stride;
            sdiv[ext[i].stream] = ext[i].div;
            carried |= 1u << ext[i].stream;
        }
        if (inst) tr("  instanced: %u instances", inst);
        uint32_t stride = h.stride, ilv_streams = 0;
        IDirect3DVertexDeclaration9 *ilv = nullptr;
        if (shader) {
            /* the vertices are read through the shader's declaration: it
             * must be known, every stream it reads must have come with the
             * draw and each stream's stride must cover what it reads */
            auto it = c.vshaders.find(h.fvf);
            if (it == c.vshaders.end()) { if (d.warn_once(0xc0000)) x.log("ddi: dp2: vertex shader handle 0x%x unknown (draws with it are skipped)", h.fvf); return true; }
            VShader8 &s = it->second;
            if (s.streams & ~carried) {
                /* a stream the declaration reads and nothing is bound to
                 * reads as zeros, as Direct3D 9 draws it (Wine's
                 * test_default_diffuse: black where the colour stream is
                 * missing), rather than the draw being skipped */
                if (d.warn_once(0xc0001)) x.log("ddi: dp2: vertex shader 0x%x reads streams 0x%x, the draw carried 0x%x: the others read as zeros", h.fvf, s.streams, carried);
                uint32_t zb = 0;
                for (uint32_t i = 0; i < D3DPT_DRAW8_MAX_STREAMS; i++)
                    if ((s.streams & ~carried & (1u << i)) && s.stream_bytes[i] > zb) zb = s.stream_bytes[i];
                d.zeros.assign((size_t)(inst > h.nverts ? inst : h.nverts) * zb, 0);
                for (uint32_t i = 0; i < D3DPT_DRAW8_MAX_STREAMS; i++)
                    if (s.streams & ~carried & (1u << i)) { sd[i] = d.zeros.data(); ss[i] = s.stream_bytes[i]; }
            }
            for (uint32_t i = 0; i < D3DPT_DRAW8_MAX_STREAMS; i++)
                if ((s.streams & (1u << i)) && s.stream_bytes[i] > ss[i]) {
                    if (d.warn_once(0xc0002)) x.log("ddi: dp2: vertex shader 0x%x reads %u bytes of stream %u's %u-byte vertex, draw skipped", h.fvf, s.stream_bytes[i], i, ss[i]);
                    return true;
                }
            if (s.streams & ~1u) {
                /* more than one stream (v10), or not stream 0: interleaved
                 * into one vertex, the streams in order, under a declaration
                 * that says so */
                uint32_t total = 0;
                for (uint32_t i = 0; i < D3DPT_DRAW8_MAX_STREAMS; i++) if (s.streams & (1u << i)) total += ss[i];
                ilv = total <= 1024 ? ilv_decl(s, ss) : nullptr;
                if (!ilv) {
                    if (d.warn_once(0xc0003)) x.log("ddi: dp2: vertex shader 0x%x: no interleaved declaration for streams 0x%x (%u bytes a vertex), draw skipped", h.fvf, s.streams, total);
                    return true;
                }
                d.ilv.resize((size_t)h.nverts * total);
                ilv_fill(s.streams, sd, ss, sdiv, h.nverts, 0);
                vd = d.ilv.data();
                stride = total;
                ilv_streams = s.streams;
            }
        } else if (d.trace) trv(vd, h.nverts, h.stride, h.fvf);
        apply_vs(h.fvf);
        if (ilv) x.dev->SetVertexDeclaration(ilv);  /* after apply_vs, which leaves the shader's own (and its constants) alone when it is current */
        if (!h.prim_count) return true;
        pre_draw();
        if (!h.nindices) {
            if (nv > h.nverts) { if (d.warn_once(0xa0000 | t)) x.log("ddi: dp2: draw8 primitive %u: %u vertices of %u", t, nv, h.nverts); return true; }
            x.dev->DrawPrimitiveUP(t, h.prim_count, vd, stride);
        } else {
            if (nv > h.nindices) { if (d.warn_once(0xa0010 | t)) x.log("ddi: dp2: draw8 indexed primitive %u: %u indices of %u", t, nv, h.nindices); return true; }
            d.idx.resize(nv);
            for (uint32_t i = 0; i < nv; i++) {
                uint32_t v = u16(id + 2 * i);
                if (v < h.min_index || v - h.min_index >= h.nverts) {
                    if (d.warn_once(0xa0020 | t)) x.log("ddi: dp2: draw8 index %u outside %u..%u", v, h.min_index, h.min_index + h.nverts);
                    return true;
                }
                d.idx[i] = (uint16_t)(v - h.min_index);
            }
            x.dev->DrawIndexedPrimitiveUP(t, 0, h.nverts, h.prim_count, d.idx.data(), D3DFMT_INDEX16, vd, stride);
            /* v16: the other instances, each with its instance streams'
             * element (a UP draw has no stream frequency of its own) */
            for (uint32_t k = 1; k < inst; k++) {
                if (ilv_streams) ilv_fill(ilv_streams, sd, ss, sdiv, h.nverts, k);
                x.dev->DrawIndexedPrimitiveUP(t, 0, h.nverts, h.prim_count, d.idx.data(), D3DFMT_INDEX16, vd, stride);
            }
        }
        d.draws++;
        snap();
        return true;
    }
    /* trace: the render target after a draw, as draw-<n>.ppm next to the flag file */
    void snap() {
        if (!d.trace) return;
        VramSurf *rt = surf(x, c.rt);
        if (!rt || !rt->rt) return;
        if (!ensure_stage(x, d, rt->d.width, rt->d.height, (D3DFORMAT)rt->d.format, false)) return;
        IDirect3DSurface9 *src = resolved(x, *rt);
        if (!src || FAILED(x.dev->GetRenderTargetData(src, d.stage))) return;
        D3DLOCKED_RECT lr;
        if (FAILED(d.stage->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return;
        char path[512]; const char *slash = strrchr(d.trace_flag, '/');
        snprintf(path, sizeof path, "%.*s/draw-%03u.ppm", slash ? (int)(slash - d.trace_flag) : 1, slash ? d.trace_flag : ".", ++d.trace_draws);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%u %u\n255\n", rt->d.width, rt->d.height);
            for (uint32_t yy = 0; yy < rt->d.height; yy++) {
                const uint8_t *row = (const uint8_t *)lr.pBits + (size_t)yy * lr.Pitch;
                for (uint32_t xx = 0; xx < rt->d.width; xx++) {
                    uint8_t px[3];
                    if (rt->d.format == D3DFMT_R5G6B5) { uint16_t v; memcpy(&v, row + xx * 2, 2); px[0] = (v >> 8) & 0xf8; px[1] = (v >> 3) & 0xfc; px[2] = (v << 3) & 0xf8; }
                    else { uint32_t v; memcpy(&v, row + xx * 4, 4); px[0] = v >> 16; px[1] = v >> 8; px[2] = v; }
                    fwrite(px, 1, 3, f);
                }
            }
            fclose(f);
        }
        d.stage->UnlockRect();
        tr("  -> draw-%03u.ppm", d.trace_draws);
    }
    bool fail(const char *why) {
        if (d.warn_once(0x10000 | (uint32_t)(uintptr_t)why)) { x.log("ddi: dp2: %s at offset %u", why, pos); log_hist(); }
        hr = D3DERR_COMMAND_UNPARSED_;
        return false;
    }
    bool fail_token(uint32_t op, uint32_t count) {
        if (d.warn_once(0x80000 | op)) { x.log("ddi: dp2: unknown token %u (count %u) at offset %u", op, count, pos); log_hist(); }
        hr = D3DERR_COMMAND_UNPARSED_;
        return false;
    }

    void draw(D3DPRIMITIVETYPE t, uint32_t first, uint32_t count) {
        bool ok; uint32_t nv = prim_verts(t, count);
        ok = count && (uint64_t)first + nv <= nverts;
        tr("draw type %u first %u prims %u (%u vertices)%s", t, first, count, nv, ok ? "" : " OUT OF RANGE");
        if (!ok) { if (d.warn_once(0x20000 | t)) x.log("ddi: dp2: primitive %u: vertices %u+%u of %u", t, first, nv, nverts); return; }
        if (d.trace) trv(vtx + (size_t)first * stride, nv);
        pre_draw();
        x.dev->DrawPrimitiveUP(t, count, vtx + (size_t)first * stride, stride);
        d.draws++;
        snap();
    }
    /* d.idx holds the indices (absolute); the draw uses the touched vertex range */
    void draw_indexed(D3DPRIMITIVETYPE t, uint32_t count) {
        uint32_t lo = ~0u, hi = 0;
        for (uint16_t i : d.idx) { if (i < lo) lo = i; if (i > hi) hi = i; }
        tr("draw indexed type %u prims %u indices %zu vertices %u..%u%s", t, count, d.idx.size(), lo, hi, hi >= nverts ? " OUT OF RANGE" : "");
        if (!count || d.idx.empty() || hi >= nverts) {
            if (d.warn_once(0x30000 | t)) x.log("ddi: dp2: indexed primitive %u: index %u of %u vertices", t, hi, nverts);
            return;
        }
        if (d.trace) trv(vtx + (size_t)d.idx[0] * stride, 1), trv(vtx + (size_t)d.idx[1] * stride, 1), trv(vtx + (size_t)d.idx[2 < d.idx.size() ? 2 : 0] * stride, 1);
        pre_draw();
        /*
         * Rebased to the touched range, as the DX8 path does: DXVK copies a
         * user-pointer draw's vertices from index 0 up to MinVertexIndex +
         * NumVertices, so passing the batch's base with MinVertexIndex = lo
         * copied the whole unused prefix on every draw (3DMark 99: 32 GB of
         * vertex copies in one run, 47 KB a draw, on the vCPU thread).
         */
        for (uint16_t &i : d.idx) i = (uint16_t)(i - lo);
        x.dev->DrawIndexedPrimitiveUP(t, 0, hi - lo + 1, count, d.idx.data(), D3DFMT_INDEX16,
                                      vtx + (size_t)lo * stride, stride);
        d.draws++;
        snap();
    }
    /* count indices at p, each + base */
    bool gather(const uint8_t *p, uint32_t count, uint32_t base) {
        d.idx.resize(count);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t v = u16(p + 2 * i) + base;
            if (v > 0xffff) return false;
            d.idx[i] = (uint16_t)v;
        }
        return true;
    }

    /* a DDI texture stage as a stage_tex index: 0..15, then 256..260 as
     * 16..20; ~0 for none */
    static uint32_t sampler_slot(uint32_t stage) {
        if (stage < 16) return stage;
        if (stage >= 256 && stage <= 260) return 16 + (stage - 256);
        return ~0u;
    }
    /* the texture stage states that are D3D9 sampler states, in the DDI's
     * numbering: DX7's 12..21, DX8's ADDRESSW 25, and DX9's sRGB read,
     * element index and displacement-map offset at 29..31 (d3dhal.h's
     * D3DTSS_SRGBTEXTURE, D3DTSS_ELEMENTINDEX, D3DTSS_DMAPOFFSET) */
    /* v20: a context whose caps claim NONPOW2CONDITIONAL (the DX7 / DX8
     * faces, and the DX9 face on 9x) samples a texture of other sizes
     * clamped whatever address mode the app set, as the era's cards do;
     * DXVK would wrap it (Wine's conditional_np2_repeat_test). d.addr keeps
     * the app's modes for a power-of-two texture. Not while a state set
     * records: the block keeps the app's own values */
    static bool pow2(uint32_t v) { return v && !(v & (v - 1)); }
    void apply_addr(uint32_t stage) {
        static const D3DSAMPLERSTATETYPE st[3] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW };
        uint32_t i = sampler_slot(stage);
        if (i == ~0u || d.recording || !c.np2cond) return;
        VramSurf *s = d.stage_tex[i] ? surf(x, d.stage_tex[i]) : nullptr;
        bool clamp = s && !(pow2(s->d.width) && pow2(s->d.height) && (!s->vol || pow2(s->depth)));
        for (int k = 0; k < 3; k++) x.dev->SetSamplerState(stage, st[k], clamp ? (uint32_t)D3DTADDRESS_CLAMP : d.addr[i][k]);
    }
    void address_state(uint32_t stage, uint32_t mask, uint32_t v) {
        static const D3DSAMPLERSTATETYPE st[3] = { D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW };
        uint32_t i = sampler_slot(stage);
        for (int k = 0; k < 3; k++)
            if (mask & (1u << k)) {
                if (i != ~0u) d.addr[i][k] = v;
                x.dev->SetSamplerState(stage, st[k], v);
            }
        apply_addr(stage);
    }
    void sampler_state(uint32_t stage, uint32_t st, uint32_t v) {
        switch (st) {
        case 12: address_state(stage, 3, v); break;
        case 13: address_state(stage, 1, v); break;
        case 14: address_state(stage, 2, v); break;
        case 15: x.dev->SetSamplerState(stage, D3DSAMP_BORDERCOLOR, v); break;
        /* the filters in the DDI's DX7 numbering (D3DTFG_* / D3DTFN_* / D3DTFP_*): the
         * driver rewrites a d3d8.dll context's D3DTEXF_* values before they get here */
        case 16: x.dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, v == 1 ? D3DTEXF_POINT : v == 5 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR); break;
        case 17: x.dev->SetSamplerState(stage, D3DSAMP_MINFILTER, v == 1 ? D3DTEXF_POINT : v == 3 ? D3DTEXF_ANISOTROPIC : D3DTEXF_LINEAR); break;
        case 18: x.dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, v == 2 ? D3DTEXF_POINT : v == 3 ? D3DTEXF_LINEAR : D3DTEXF_NONE); break;
        case 19: x.dev->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, v); break;
        case 20: x.dev->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, v); break;
        case 21: x.dev->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, v); break;
        case 25: address_state(stage, 4, v); break;
        case 29: x.dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, v); break;
        case 30: x.dev->SetSamplerState(stage, D3DSAMP_ELEMENTINDEX, v); break;
        case 31: x.dev->SetSamplerState(stage, D3DSAMP_DMAPOFFSET, v); break;
        default: break;
        }
    }

    /* a stage's texture: the surface's host object, re-read from VRAM when
     * dirty (the guest wrote it, its palette changed, its key changed) */
    void bind_texture(uint32_t stage, uint32_t handle) {
        IDirect3DBaseTexture9 *t = nullptr;
        VramSurf *s = handle ? surf(x, handle) : nullptr;
        if (s && ensure_object(x, *s) && (s->tex || s->cube || s->vol)) {
            if (s->dirty || s->faces_dirty) {
                if (d.pal_lines < 48 && needs_expand(*s)) { d.pal_lines++; x.log("ddi: dp2: expanding texture %u (fmt %u, palette %u%s, key %s 0x%x..0x%x) for stage %u", handle, s->d.format, s->palette, d.palettes.count(s->palette) ? "" : " unknown", s->ckey ? "on" : "off", s->ckey_lo, s->ckey_hi, stage); }
                if (s->rt) {
                    /* a render target (a render-target cube: face 0 here, the
                     * other faces through their own entries) */
                    if (s->dirty) upload_target(x, d, *s);
                    if (s->cube)
                        for (uint32_t f = 1; f < D3DPT_CUBE_FACES; f++) {
                            VramSurf *fs = s->face_h[f] ? surf(x, s->face_h[f]) : nullptr;
                            if (fs && fs->dirty && ensure_object(x, *fs)) upload_target(x, d, *fs);
                        }
                    s->faces_dirty = false;
                } else upload_texture(x, d, *s);
            }
            t = s->cube ? (IDirect3DBaseTexture9 *)s->cube : s->vol ? (IDirect3DBaseTexture9 *)s->vol : (IDirect3DBaseTexture9 *)s->tex;
            /* v15: an autogen target drawn into since: its levels again */
            if (s->mips_stale && s->tex) { s->tex->GenerateMipSubLevels(); s->mips_stale = false; }
        }
        x.dev->SetTexture(stage, t);
        if (sampler_slot(stage) != ~0u) d.stage_tex[sampler_slot(stage)] = t ? handle : 0;
        if (stage == 0) apply_ckey();
        apply_addr(stage);
    }
    /* before a draw: a bound texture whose VRAM / palette / key changed
     * since it was bound is uploaded again (the runtime re-sends TEXTUREMAP
     * only on a SetTexture) */
    void pre_draw() {
        for (uint32_t i = 0; i < 21; i++) {
            VramSurf *s = d.stage_tex[i] ? surf(x, d.stage_tex[i]) : nullptr;
            if (s && (s->dirty || s->faces_dirty || s->mips_stale || !(s->tex || s->cube || s->vol))) bind_texture(i < 16 ? i : 256 + (i - 16), d.stage_tex[i]);
        }
        /* last, after the uploads above, which each close the scene: the
         * DX7/DX8 DDI has no BeginScene of its own and Windows' own
         * Direct3D 9 draws nothing outside one */
        x.scene_begin();
    }

    /* colour keying (v8): a keyed texture's key texels carry alpha 0, so
     * while COLORKEYENABLE is on and such a texture is at stage 0 the alpha
     * test is forced on (GREATEREQUAL 1) unless the app runs its own, and
     * stage 0's alpha op is made to pass the texture alpha through when the
     * app's does not (the DX7 runtime's TEXTUREMAPBLEND emulation selects
     * the diffuse alpha for a texture format without alpha, i.e. every keyed
     * R5G6B5 / P8 texture); the app's states come back when the key no
     * longer applies */
    void apply_ckey() {
        VramSurf *s = surf(x, d.stage_tex[0]);
        bool want = d.ckey_rs && s && s->ckey && !(d.rs_set[15] && d.rs_val[15]);
        uint32_t aop = d.tss_set[0][4] ? d.tss_val[0][4] : D3DTOP_SELECTARG1;
        uint32_t a1 = d.tss_set[0][5] ? d.tss_val[0][5] : D3DTA_TEXTURE, a2 = d.tss_set[0][6] ? d.tss_val[0][6] : D3DTA_CURRENT;
        bool uses_tex = aop == D3DTOP_SELECTARG1 ? (a1 & 0xf) == D3DTA_TEXTURE
                      : aop == D3DTOP_SELECTARG2 ? (a2 & 0xf) == D3DTA_TEXTURE
                      : aop != D3DTOP_DISABLE && ((a1 & 0xf) == D3DTA_TEXTURE || (a2 & 0xf) == D3DTA_TEXTURE);
        bool ovr = want && !uses_tex;
        if (want != d.ckey_forced) {
            d.ckey_forced = want;
            tr("colour key alpha test %s", want ? "forced on" : "restored");
            if (want) {
                x.dev->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
                x.dev->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL);
                x.dev->SetRenderState(D3DRS_ALPHAREF, 1);
            } else {
                x.dev->SetRenderState(D3DRS_ALPHATESTENABLE, d.rs_set[15] ? d.rs_val[15] : 0);
                x.dev->SetRenderState(D3DRS_ALPHAFUNC, d.rs_set[25] ? d.rs_val[25] : D3DCMP_ALWAYS);
                x.dev->SetRenderState(D3DRS_ALPHAREF, d.rs_set[24] ? d.rs_val[24] : 0);
            }
        }
        if (ovr != d.ckey_alpha_ovr) {
            d.ckey_alpha_ovr = ovr;
            tr("colour key stage 0 alpha op %s", ovr ? "= texture alpha" : "restored");
            x.dev->SetTextureStageState(0, D3DTSS_ALPHAOP, ovr ? D3DTOP_SELECTARG1 : aop);
            x.dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, ovr ? D3DTA_TEXTURE : a1);
        }
    }

    /* The DirectX 3 / 5 texture states, which only a DX3 execute buffer
     * still delivers as render states (the DX6+ runtimes turn them into
     * stage states before the driver sees them; doc 15 "Execute buffers"):
     * TEXTUREHANDLE (1) binds stage 0, TEXTUREMAPBLEND (21) picks stage 0's
     * colour / alpha ops the way the old fixed function did. No texture:
     * the diffuse colour; MODULATE: texture x diffuse, the alpha from the
     * texture when its format has one (the colour-key expansion counts:
     * apply_ckey overrides it for a keyed texture anyway), else from the
     * diffuse. */
    static bool legacy_texture_state(uint32_t s) { return s == 1 || s == 3 || s == 5 || s == 6 || s == 17 || s == 18 || s == 21; }
    void apply_mapblend(bool args) {
        uint32_t blend = d.rs_set[21] ? d.rs_val[21] : 2 /* MODULATE */;
        VramSurf *t = d.stage_tex[0] ? surf(x, d.stage_tex[0]) : nullptr;
        bool tex_alpha = t && (fmt_has_alpha(t->d.format) || t->ckey || needs_expand(*t));
        uint32_t cop = D3DTOP_SELECTARG2, aop = D3DTOP_SELECTARG2;              /* no texture: diffuse only */
        if (t) {
            switch (blend) {
            case 1: case 5: case 7: cop = D3DTOP_SELECTARG1; aop = D3DTOP_SELECTARG1; break;      /* DECAL, DECALMASK, COPY */
            case 3: cop = D3DTOP_BLENDTEXTUREALPHA; aop = D3DTOP_SELECTARG2; break;              /* DECALALPHA */
            case 4: cop = D3DTOP_MODULATE; aop = D3DTOP_MODULATE; break;                           /* MODULATEALPHA */
            case 8: cop = D3DTOP_ADD; aop = D3DTOP_SELECTARG2; break;                              /* ADD */
            default: cop = D3DTOP_MODULATE; aop = tex_alpha ? D3DTOP_SELECTARG1 : D3DTOP_SELECTARG2; break;   /* MODULATE, MODULATEMASK */
            }
        }
        tr("map blend %u with%s texture: colour op %u%s alpha op %u%s", blend, t ? "" : "out",
           cop, d.legacy_cop ? "" : " (the app's)", aop, d.legacy_aop ? "" : " (the app's)");
        if (args) {
            stage_state(0, 2, D3DTA_TEXTURE); stage_state(0, 3, D3DTA_DIFFUSE);
            stage_state(0, 5, D3DTA_TEXTURE); stage_state(0, 6, D3DTA_DIFFUSE);
        }
        if (d.legacy_cop) stage_state(0, 1, cop);
        if (d.legacy_aop) stage_state(0, 4, aop);
    }
    void legacy_render_state(uint32_t s, uint32_t v) {
        switch (s) {
        case 1:                                                             /* TEXTUREHANDLE: the surface handle */
            d.legacy_cop = d.legacy_aop = true;
            stage_state(0, 0, v);
            apply_mapblend(true);
            break;
        case 3: address_state(0, 3, v); break;                            /* TEXTUREADDRESS */
        case 5: case 6: {                                                   /* WRAPU / WRAPV: WRAP0 bits */
            uint32_t w0 = ((d.rs_set[5] && d.rs_val[5]) ? 1u : 0u) | ((d.rs_set[6] && d.rs_val[6]) ? 2u : 0u);
            x.dev->SetRenderState(D3DRS_WRAP0, w0);
            break;
        }
        case 17: x.dev->SetSamplerState(0, D3DSAMP_MAGFILTER, v == 1 ? D3DTEXF_POINT : D3DTEXF_LINEAR); break;          /* TEXTUREMAG */
        case 18: {                                                          /* TEXTUREMIN: NEAREST, LINEAR, MIPNEAREST, MIPLINEAR, LINEARMIPNEAREST, LINEARMIPLINEAR */
            static const D3DTEXTUREFILTERTYPE minf[7] = { D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_LINEAR };
            static const D3DTEXTUREFILTERTYPE mipf[7] = { D3DTEXF_NONE, D3DTEXF_NONE, D3DTEXF_NONE, D3DTEXF_POINT, D3DTEXF_LINEAR, D3DTEXF_POINT, D3DTEXF_LINEAR };
            uint32_t i = v < 7 ? v : 2;
            x.dev->SetSamplerState(0, D3DSAMP_MINFILTER, minf[i]);
            x.dev->SetSamplerState(0, D3DSAMP_MIPFILTER, mipf[i]);
            break;
        }
        case 21: d.legacy_cop = d.legacy_aop = true; apply_mapblend(true); break;   /* TEXTUREMAPBLEND */
        }
    }

    /* Pre-transformed vertices draw with D3DRS_CLIPPING off. The driver
     * claims no D3DPMISCCAPS_CLIPTLVERTS (doc 15), so the guest's runtime
     * has clipped such geometry to the screen itself, and the cards of the
     * era draw the rest unclipped in depth (Wine's z_range_test and
     * depth_clamp_test); DXVK's depth clip follows the state
     * (patches/dxvk/10). Other vertices get the app's own value */
    void apply_clip(bool tl) {
        d.clip_tl = tl;
        if (d.recording) {                              /* a state set keeps the app's own value */
            if (d.rs_set[D3DRS_CLIPPING]) x.dev->SetRenderState(D3DRS_CLIPPING, d.rs_val[D3DRS_CLIPPING]);
            d.clip_host = ~0u;
            return;
        }
        uint32_t v = tl ? FALSE : d.rs_set[D3DRS_CLIPPING] ? d.rs_val[D3DRS_CLIPPING] : TRUE;
        if (v != d.clip_host) { x.dev->SetRenderState(D3DRS_CLIPPING, v); d.clip_host = v; }
    }
    void render_state(uint32_t s, uint32_t v) {
        tr("rs %u = 0x%x%s", s, v, rs_passthrough(s) || s == 41 || s == 47 || legacy_texture_state(s) ? "" : " (dropped)");
        if (s < 256) { d.rs_val[s] = v; d.rs_set[s] = 1; }
        if (s == D3DRS_CLIPPING) { apply_clip(d.clip_tl); return; }
        if (s == 28 && d.nofog) v = 0;
        if (s == 41) { d.ckey_rs = v; apply_ckey(); return; }              /* COLORKEYENABLE */
        if (s == 47) {
            /* ZBIAS (0..16, DX6-DX8) -> d3d9's DEPTHBIAS: the scale DXVK's
             * own d3d8 layer uses (-1/65535 per step, D16 precision) */
            float bias = (float)(v & 0xffff) * (-1.0f / 65535.0f);
            uint32_t bits; memcpy(&bits, &bias, 4);
            x.dev->SetRenderState(D3DRS_DEPTHBIAS, bits);
            return;
        }
        if (legacy_texture_state(s)) { legacy_render_state(s, v); return; }
        if (rs_passthrough(s)) {
            x.dev->SetRenderState((D3DRENDERSTATETYPE)s, v);
            /* the app's alpha test states: re-forced for the key, or followed */
            if ((s == 15 || s == 24 || s == 25) && d.ckey_forced) { d.ckey_forced = false; apply_ckey(); }
            return;
        }
        if (s < 64 && d.warn_once(0x40000 | s)) x.log("ddi: dp2: render state %u dropped (no d3d9 equivalent)", s);
    }

    void stage_state(uint32_t stage, uint32_t st, uint32_t v) {
        if (stage >= 8) {
            /* a DX9 sampler past the fixed function's 8 stages: its texture
             * and sampler states only */
            if (sampler_slot(stage) == ~0u) {
                if (d.warn_once(0x70000 | (stage & 0xffff))) x.log("ddi: dp2: texture stage %u dropped", stage);
                return;
            }
            tr("sampler %u.%u = 0x%x", stage, st, v);
            if (st == 0) bind_texture(stage, v);
            else sampler_state(stage, st, v);
            return;
        }
        tr("tss %u.%u = 0x%x", stage, st, v);
        if (st < 33) { d.tss_val[stage][st] = v; d.tss_set[stage][st] = 1; }
        switch (st) {
        case 0: {                                           /* TEXTUREMAP: the surface handle */
            IDirect3DBaseTexture9 *t = nullptr;
            if (v) {
                VramSurf *s = surf(x, v);
                if (s && d.reread_all) s->dirty = true;
                if (s && d.trace) {
                    /* the mean of the level-0 texels in VRAM (every 8th pixel of every 8th row, 32/16-bit only):
                     * black VRAM = the guest never wrote it, content = the host copy may be stale.
                     * Trace only: it reads the texture on every bind (7 % of the vCPU thread in
                     * Max Payne 2, whose DXT textures it read a quarter of) */
                    uint32_t bpp = fmt_row_bytes(s->d.format, 1), n = 0; uint64_t sum = 0;
                    if (fmt_dxt(s->d.format)) {                 /* compressed: the mean of the block bytes */
                        uint32_t rows = fmt_rows(s->d.format, s->d.height), row = fmt_row_bytes(s->d.format, s->d.width);
                        for (uint32_t yy = 0; yy < rows; yy++) for (uint32_t xx = 0; xx < row; xx += 4, n++) sum += 3 * x.vram[s->d.offset + (size_t)yy * s->d.pitch + xx];
                    } else if (bpp == 4 || bpp == 2) {
                        for (uint32_t yy = 0; yy < s->d.height; yy += 8) {
                            const uint8_t *row = x.vram + s->d.offset + (size_t)yy * s->d.pitch;
                            for (uint32_t xx = 0; xx < s->d.width; xx += 8, n++) {
                                if (bpp == 4) { uint32_t px; memcpy(&px, row + xx * 4, 4); sum += ((px >> 16) & 0xff) + ((px >> 8) & 0xff) + (px & 0xff); }
                                else { uint16_t px; memcpy(&px, row + xx * 2, 2); sum += px ? 128 : 0; }
                            }
                        }
                    }
                    tr("  texture %u: %ux%u fmt %u levels %u caps 0x%x%s, vram mean %u/765 of %u samples", v, s->d.width, s->d.height, s->d.format, s->d.levels, s->d.caps, s->dirty ? " (re-read)" : "", n ? (unsigned)(sum / n) : 0, n);
                    if (d.trace && (bpp == 4 || bpp == 2 || fmt_dxt(s->d.format))) dump_texture(*s, v);
                }
                if (!s) { if (d.warn_once(0x50000)) x.log("ddi: dp2: texture handle %u unknown", v); }
            }
            (void)t;
            bind_texture(stage, v);
            break;
        }
        case 12: case 13: case 14: case 15: case 16: case 17: case 18: case 19: case 20: case 21: case 25:
        case 29: case 30: case 31:
            sampler_state(stage, st, v);
            break;
        default:
            if (st < 33) x.dev->SetTextureStageState(stage, (D3DTEXTURESTAGESTATETYPE)st, v);
            /* the app's stage-0 alpha op while the key overrides it: re-applied or followed */
            if (stage == 0 && st >= 4 && st <= 6 && d.ckey_alpha_ovr) { d.ckey_alpha_ovr = false; apply_ckey(); }
            break;
        }
    }

    void set_targets(uint32_t rt, uint32_t z) {
        tr("render target %u z %u", rt, z);
        if (!surf(x, rt)) { if (d.warn_once(0x60000)) x.log("ddi: dp2: render target handle %u unknown", rt); return; }
        if (z && !surf(x, z)) z = 0;
        c.rt = rt; c.z = z;
        bind_ctx(x, d, c, b, true);
    }

    void clear(uint32_t flags, uint32_t color, float z, uint32_t stencil, uint32_t nrects, const uint8_t *rects) {
        VramSurf *rt = surf(x, c.rt);
        tr("clear flags 0x%x color 0x%08x z %g stencil %u rects %u", flags, color, z, stencil, nrects);
        /* a full clear of the target makes any guest-side content moot: skip its upload */
        if (rt && (flags & D3DCLEAR_TARGET) && nrects == 0) { rt->dirty = false; rt->checked = true; }
        if (!bind_ctx(x, d, c, b, true)) return;
        std::vector<D3DRECT> r(nrects);
        if (nrects) memcpy(r.data(), rects, nrects * sizeof(D3DRECT));
        x.dev->Clear(nrects, nrects ? r.data() : nullptr, flags & 7, color, z, stencil);
    }

    /* the body size that puts the offset after it at a DWORD boundary
     * (the buffer itself is 8-aligned in the record); align_next(0) is the
     * padding between a token's header and its DWORD-aligned payload */
    size_t align_next(size_t need) const {
        size_t next = ((size_t)pos + 4 + need + 3) & ~(size_t)3;
        return next - pos - 4;
    }

    bool run() {
        /* the context's targets, unless the target it names is gone: a
         * DX9 application releases a render-target texture it drew into
         * while still bound, and the runtime's SETRENDERTARGET to another
         * one comes in this stream (set_targets binds that) */
        if (surf(x, c.rt) && !bind_ctx(x, d, c, b, true)) return false;
        const uint8_t *p = cmd;
        while (p < cmd_end) {
            pos = (uint32_t)(p - cmd);
            if (cmd_end - p < 4) return fail("truncated command");
            uint32_t op = p[0], count = u16(p + 2);
            const uint8_t *q = p + 4;
            hist[nhist++ % 24] = { pos, op, count };
            tr("op %u x%u", op, count);
            size_t left = cmd_end - q, need = 0;
            switch (op) {
            case DP2_POINTS:
                need = count * 4u;
                if (need > left) return fail("truncated POINTS");
                for (uint32_t i = 0; i < count; i++) draw(D3DPT_POINTLIST, u16(q + 4 * i + 2), u16(q + 4 * i));
                break;
            case DP2_INDEXEDLINELIST:
                need = count * 4u;
                if (need > left) return fail("truncated INDEXEDLINELIST");
                if (gather(q, count * 2, 0)) draw_indexed(D3DPT_LINELIST, count);
                break;
            case DP2_INDEXEDTRIANGLELIST:
                need = count * 8u;
                if (need > left) return fail("truncated INDEXEDTRIANGLELIST");
                d.idx.resize(count * 3);
                for (uint32_t i = 0; i < count; i++) {
                    d.idx[3 * i] = u16(q + 8 * i); d.idx[3 * i + 1] = u16(q + 8 * i + 2); d.idx[3 * i + 2] = u16(q + 8 * i + 4);
                }
                draw_indexed(D3DPT_TRIANGLELIST, count);
                break;
            case DP2_RENDERSTATE:
                need = count * 8u;
                if (need > left) return fail("truncated RENDERSTATE");
                for (uint32_t i = 0; i < count; i++) render_state(u32(q + 8 * i), u32(q + 8 * i + 4));
                break;
            case DP2_LINELIST:
                need = 2;
                if (need > left) return fail("truncated LINELIST");
                draw(D3DPT_LINELIST, u16(q), count);
                break;
            case DP2_LINESTRIP:
                need = 2;
                if (need > left) return fail("truncated LINESTRIP");
                draw(D3DPT_LINESTRIP, u16(q), count);
                break;
            case DP2_INDEXEDLINESTRIP:
                need = 2 + (count + 1) * 2u;
                if (need > left) return fail("truncated INDEXEDLINESTRIP");
                if (gather(q + 2, count + 1, u16(q))) draw_indexed(D3DPT_LINESTRIP, count);
                break;
            case DP2_TRIANGLELIST:
                need = 2;
                if (need > left) return fail("truncated TRIANGLELIST");
                draw(D3DPT_TRIANGLELIST, u16(q), count);
                break;
            case DP2_TRIANGLESTRIP:
                need = 2;
                if (need > left) return fail("truncated TRIANGLESTRIP");
                draw(D3DPT_TRIANGLESTRIP, u16(q), count);
                break;
            case DP2_INDEXEDTRIANGLESTRIP:
                need = 2 + (count + 2) * 2u;
                if (need > left) return fail("truncated INDEXEDTRIANGLESTRIP");
                if (gather(q + 2, count + 2, u16(q))) draw_indexed(D3DPT_TRIANGLESTRIP, count);
                break;
            case DP2_TRIANGLEFAN:
                need = 2;
                if (need > left) return fail("truncated TRIANGLEFAN");
                draw(D3DPT_TRIANGLEFAN, u16(q), count);
                break;
            case DP2_INDEXEDTRIANGLEFAN:
                need = 2 + (count + 2) * 2u;
                if (need > left) return fail("truncated INDEXEDTRIANGLEFAN");
                if (gather(q + 2, count + 2, u16(q))) draw_indexed(D3DPT_TRIANGLEFAN, count);
                break;
            case DP2_TRIANGLEFAN_IMM: {
                /* the payload (edge flags, then count + 2 vertices inline)
                 * starts at the next DWORD-aligned *offset* of the buffer,
                 * and so does the next token: a token after an odd-length
                 * one (INDEXEDTRIANGLELIST2: 2 + 6n bytes) sits at offset
                 * 2 mod 4 and the runtime pads after its 4-byte header. The
                 * DX7 runtime never produced that sequence (D3D7TEST, FIFA);
                 * the DX8 runtime's legacy path does it every frame, and
                 * reading the vertices 2 bytes early drew Max Payne's alley
                 * with black bands. */
                size_t pad = align_next(0);
                need = pad + 4 + (size_t)(count + 2) * stride;
                if (need > left) return fail("truncated TRIANGLEFAN_IMM");
                if (count) { if (d.trace) trv(q + pad + 4, count + 2); pre_draw(); x.dev->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, count, q + pad + 4, stride); d.draws++; snap(); }
                need = align_next(need);
                break;
            }
            case DP2_LINELIST_IMM: {
                size_t pad = align_next(0);
                need = pad + (size_t)count * 2 * stride;
                if (need > left) return fail("truncated LINELIST_IMM");
                if (count) { if (d.trace) trv(q + pad, count * 2); pre_draw(); x.dev->DrawPrimitiveUP(D3DPT_LINELIST, count, q + pad, stride); d.draws++; snap(); }
                need = align_next(need);
                break;
            }
            case DP2_TEXTURESTAGESTATE:
                need = count * 8u;
                if (need > left) return fail("truncated TEXTURESTAGESTATE");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t stg = u16(q + 8 * i), st = u16(q + 8 * i + 2), v = u32(q + 8 * i + 4);
                    if (stg == 0 && st == 1) d.legacy_cop = false;                  /* the app's own op from now on */
                    if (stg == 0 && st == 4) d.legacy_aop = false;
                    stage_state(stg, st, v);
                    /* a DirectX 6 title picks its blend with TEXTUREMAPBLEND once (no texture bound
                     * yet: the diffuse alone) and binds textures as a stage state per draw; the
                     * DX6 runtime passes both through, so the blend is re-evaluated for the
                     * texture now bound (GTA 2's menu text drew as white boxes without it). Only
                     * an op the app set itself ends that, not an ARG (Crimson Skies' menu drew
                     * white silhouettes when COLORARG2, set after the blend, ended it) */
                    if (stg == 0 && st == 0 && (d.legacy_cop || d.legacy_aop)) apply_mapblend(false);
                }
                break;
            case DP2_INDEXEDTRIANGLELIST2: {
                need = 2 + count * 6u;
                if (need > left) return fail("truncated INDEXEDTRIANGLELIST2");
                uint32_t base = u16(q);
                if (gather(q + 2, count * 3, base)) draw_indexed(D3DPT_TRIANGLELIST, count);
                break;
            }
            case DP2_INDEXEDLINELIST2: {
                need = 2 + count * 4u;
                if (need > left) return fail("truncated INDEXEDLINELIST2");
                uint32_t base = u16(q);
                if (gather(q + 2, count * 2, base)) draw_indexed(D3DPT_LINELIST, count);
                break;
            }
            case DP2_VIEWPORTINFO:
                need = count * 16u;
                if (need > left) return fail("truncated VIEWPORTINFO");
                if (count) {
                    const uint8_t *v = q + 16 * (count - 1);
                    c.vp.X = u32(v); c.vp.Y = u32(v + 4); c.vp.Width = u32(v + 8); c.vp.Height = u32(v + 12);
                    tr("viewport %u,%u %ux%u", c.vp.X, c.vp.Y, c.vp.Width, c.vp.Height);
                    x.dev->SetViewport(&c.vp);
                }
                break;
            case DP2_WINFO:
                need = count * 8u;
                if (need > left) return fail("truncated WINFO");
                break;                                          /* w-buffer range: no d3d9 equivalent */
            case DP2_SETPALETTE:
                /* palette handle, flags (DDRAWIPAL_ALPHA 0x2000: the entries' peFlags are alpha), surface handle */
                need = count * 12u;
                if (need > left) return fail("truncated SETPALETTE");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t ph = u32(q + 12 * i), fl = u32(q + 12 * i + 4), sh = u32(q + 12 * i + 8);
                    VramSurf *s = surf(x, sh);
                    tr("palette %u (flags 0x%x) on surface %u%s", ph, fl, sh, s ? "" : " (unknown)");
                    if (d.pal_lines < 48) { d.pal_lines++; x.log("ddi: dp2: SETPALETTE palette %u flags 0x%x on surface %u%s", ph, fl, sh, s ? "" : " (unknown)"); }
                    if (!s) { if (d.warn_once(0xd0000)) x.log("ddi: dp2: SETPALETTE on unknown surface %u", sh); continue; }
                    if (s->palette != ph || s->pal_alpha != ((fl & 0x2000) != 0)) { s->palette = ph; s->pal_alpha = (fl & 0x2000) != 0; s->dirty = true; }
                }
                break;
            case DP2_UPDATEPALETTE: {
                /* palette handle, start index, entry count, then PALETTEENTRYs (r, g, b, flags) */
                if (left < 8) return fail("truncated UPDATEPALETTE");
                uint32_t ph = u32(q), start = u16(q + 4), n = u16(q + 6);
                need = 8 + (size_t)n * 4;
                if (need > left) return fail("truncated UPDATEPALETTE");
                if (start + n > 256) return fail("UPDATEPALETTE beyond 256 entries");
                tr("palette %u entries %u..%u", ph, start, start + n);
                if (d.pal_lines < 48) { d.pal_lines++; x.log("ddi: dp2: UPDATEPALETTE palette %u entries %u..%u (first %02x%02x%02x/%02x)", ph, start, start + n, q[8], q[9], q[10], q[11]); }
                Palette &pal = d.palettes[ph];
                for (uint32_t i = 0; i < n; i++) {
                    const uint8_t *e = q + 8 + 4 * i;
                    pal.argb[start + i] = ((uint32_t)e[3] << 24) | ((uint32_t)e[0] << 16) | ((uint32_t)e[1] << 8) | e[2];
                }
                for (auto &kv : d.surfs) if (kv.second.palette == ph && kv.second.d.format == D3DFMT_P8) kv.second.dirty = true;
                break;
            }
            case DP2_ZRANGE:
                need = count * 8u;
                if (need > left) return fail("truncated ZRANGE");
                if (count) {
                    const uint8_t *v = q + 8 * (count - 1);
                    memcpy(&c.vp.MinZ, v, 4); memcpy(&c.vp.MaxZ, v + 4, 4);
                    x.dev->SetViewport(&c.vp);
                }
                break;
            case DP2_SETMATERIAL:
                need = count * 68u;
                if (need > left) return fail("truncated SETMATERIAL");
                if (count) { D3DMATERIAL9 m; memcpy(&m, q + 68 * (count - 1), sizeof m); x.dev->SetMaterial(&m); }
                break;
            case DP2_SETLIGHT: {
                const uint8_t *e = q;
                for (uint32_t i = 0; i < count; i++) {
                    if (cmd_end - e < 8) return fail("truncated SETLIGHT");
                    uint32_t index = u32(e), type = u32(e + 4);
                    e += 8;
                    /* DXVK grows its light array to the index (a garbage
                     * index = std::bad_alloc, which cannot be caught across
                     * its statically linked unwinder: QEMU aborts) */
                    bool ok = index < 1024;
                    if (!ok && d.warn_once(0x90000)) x.log("ddi: dp2: light index %u out of range, dropped", index);
                    if (type == 2) {                            /* D3DHAL_SETLIGHT_DATA: a D3DLIGHT7 follows */
                        if (cmd_end - e < 104) return fail("truncated SETLIGHT data");
                        D3DLIGHT9 l; memcpy(&l, e, sizeof l);
                        if (ok) x.dev->SetLight(index, &l);
                        e += 104;
                    } else if (ok) {
                        x.dev->LightEnable(index, type == 0);
                        d.lights_on[index] = type == 0;
                    }
                }
                need = e - q;
                break;
            }
            case DP2_CREATELIGHT:
                need = count * 4u;
                if (need > left) return fail("truncated CREATELIGHT");
                break;
            case DP2_SETTRANSFORM:
            case DP2_MULTIPLYTRANSFORM:
                need = count * 68u;
                if (need > left) return fail("truncated SETTRANSFORM");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t t = u32(q + 68 * i);
                    D3DMATRIX m; memcpy(&m, q + 68 * i + 4, sizeof m);
                    /* DX7 WORLD(1) / WORLD1..3 (4..6) -> d3d9 WORLD = 256..
                     * (DX8 already numbers them 256..); anything else DXVK
                     * indexes an array with: dropped */
                    if (t == 1) t = 256; else if (t >= 4 && t <= 6) t = 256 + (t - 3);
                    tr("%s %u", op == DP2_SETTRANSFORM ? "transform" : "multiply transform", t);
                    if (t == 2 || t == 3 || (t >= 16 && t <= 23) || (t >= 256 && t <= 511)) {
                        if (op == DP2_SETTRANSFORM) x.dev->SetTransform((D3DTRANSFORMSTATETYPE)t, &m);
                        else x.dev->MultiplyTransform((D3DTRANSFORMSTATETYPE)t, &m);
                    } else if (d.warn_once(0x90001)) x.log("ddi: dp2: transform %u out of range, dropped", u32(q + 68 * i));
                }
                break;
            case DP2_TEXBLT:
                need = count * 36u;
                if (need > left) return fail("truncated TEXBLT");
                if (d.warn_once(op)) x.log("ddi: dp2: TEXBLT is not supported (no driver-managed textures)");
                break;
            case DP2_STATESET:
                /* the runtime's state sets: BEGIN records what follows into a
                 * block (a predefined block type captures the device state
                 * at once), EXECUTE applies it, CAPTURE refreshes it */
                need = count * 12u;
                if (need > left) return fail("truncated STATESET");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t sop = u32(q + 12 * i), handle = u32(q + 12 * i + 4), type = u32(q + 12 * i + 8);
                    auto it = d.sblocks.find(handle);
                    tr("stateset op %u handle %u type %u", sop, handle, type);
                    switch (sop) {
                    case 0:                                             /* BEGIN */
                        if (d.recording) { IDirect3DStateBlock9 *sb = nullptr; x.dev->EndStateBlock(&sb); if (sb) sb->Release(); d.recording = false; }
                        if (it != d.sblocks.end()) { if (it->second) it->second->Release(); d.sblocks.erase(it); }
                        if (type >= 1 && type <= 3) {
                            IDirect3DStateBlock9 *sb = nullptr;
                            if (SUCCEEDED(x.dev->CreateStateBlock((D3DSTATEBLOCKTYPE)type, &sb)) && sb) d.sblocks[handle] = sb;
                        } else if (SUCCEEDED(x.dev->BeginStateBlock())) {
                            d.recording = true;
                            d.sblocks[handle] = nullptr;
                        }
                        break;
                    case 1:                                             /* END */
                        if (d.recording) {
                            IDirect3DStateBlock9 *sb = nullptr;
                            x.dev->EndStateBlock(&sb);
                            d.recording = false;
                            if (it != d.sblocks.end() && !it->second) it->second = sb;
                            else if (sb) sb->Release();
                        }
                        break;
                    case 2:                                             /* DELETE */
                        if (it != d.sblocks.end()) { if (it->second) it->second->Release(); d.sblocks.erase(it); }
                        break;
                    case 3:                                             /* EXECUTE */
                        /* DX9: the viewport stays the stream's. d3d9.dll sends
                         * a SetRenderTarget and the viewport it resets at the
                         * next draw, so a CreateStateBlock after one captured
                         * the old target's viewport here, and executing it put
                         * a 4x4 one under a 640x480 frame (D3DFEAT9 after its
                         * float target). The runtime sends none after EXECUTE */
                        if (it != d.sblocks.end() && it->second) {
                            it->second->Apply();
                            if (c.dx9) x.dev->SetViewport(&c.vp);
                            else x.dev->GetViewport(&c.vp);
                            /* the block's address modes are the app's now
                             * (one captured while a clamp was forced keeps
                             * the clamp), then the clamp again */
                            /* its D3DRS_CLIPPING is the app's now too */
                            DWORD clip;
                            if (SUCCEEDED(x.dev->GetRenderState(D3DRS_CLIPPING, &clip))) {
                                d.rs_val[D3DRS_CLIPPING] = clip; d.rs_set[D3DRS_CLIPPING] = 1; d.clip_host = clip;
                            }
                            apply_clip(d.clip_tl);
                            if (c.np2cond)
                                for (uint32_t i = 0; i < 21; i++) {
                                    uint32_t stage = i < 16 ? i : 256 + (i - 16);
                                    DWORD v;
                                    for (int k = 0; k < 3; k++)
                                        if (SUCCEEDED(x.dev->GetSamplerState(stage, (D3DSAMPLERSTATETYPE)(D3DSAMP_ADDRESSU + k), &v))) d.addr[i][k] = v;
                                    apply_addr(stage);
                                }
                        }
                        else if (d.warn_once(0xb0000)) x.log("ddi: dp2: state set %u executed before it was recorded", handle);
                        break;
                    case 4:                                             /* CAPTURE */
                        if (it != d.sblocks.end() && it->second) it->second->Capture();
                        break;
                    }
                }
                break;
            case DP2_SETPRIORITY:
                need = count * 8u;
                if (need > left) return fail("truncated SETPRIORITY");
                break;
            case DP2_SETRENDERTARGET:
                need = count * 8u;
                if (need > left) return fail("truncated SETRENDERTARGET");
                if (count) set_targets(u32(q + 8 * (count - 1)), u32(q + 8 * (count - 1) + 4));
                if (b.err) return false;
                break;
            case DP2_CLEAR: {
                need = 16 + count * 16u;
                if (need > left) return fail("truncated CLEAR");
                float z; memcpy(&z, q + 8, 4);
                clear(u32(q), u32(q + 4), z, u32(q + 12), count, q + 16);
                if (b.err) return false;
                break;
            }
            case DP2_SETTEXLOD:
                need = count * 8u;
                if (need > left) return fail("truncated SETTEXLOD");
                break;
            case DP2_SETCLIPPLANE:
                need = count * 20u;
                if (need > left) return fail("truncated SETCLIPPLANE");
                for (uint32_t i = 0; i < count; i++) {
                    float pl[4]; memcpy(pl, q + 20 * i + 4, sizeof pl);
                    if (u32(q + 20 * i) < 6) x.dev->SetClipPlane(u32(q + 20 * i), pl);
                }
                break;
            case D3DPT_DP2_DRAW8:
                if (!draw8(q, left, need)) return false;
                break;
            case DP2_SETVERTEXSHADER:                            /* an FVF, or a shader handle (bit 0) */
                need = count * 4u;
                if (need > left) return fail("truncated SETVERTEXSHADER");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t h = u32(q + 4 * i);
                    tr("vertex shader 0x%x", h);
                    apply_vs(h);
                }
                break;
            case DP2_CREATEVERTEXSHADER: case DP2_CREATEPIXELSHADER: case DP2_SETVERTEXSHADERCONST: case DP2_SETPIXELSHADERCONST: {
                /* variable: handle [, decl size], code size / register, count */
                need = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (left < need + 8) return fail("truncated shader token");
                    const uint8_t *e = q + need;
                    if (op == DP2_CREATEVERTEXSHADER) {
                        if (left < need + 12) return fail("truncated CREATEVERTEXSHADER");
                        size_t ds = u32(e + 4), cs = u32(e + 8);
                        need += 12 + ds + cs;
                        if (need > left) return fail("truncated CREATEVERTEXSHADER data");
                        create_vshader(u32(e), e + 12, (uint32_t)ds, e + 12 + ds, (uint32_t)cs);
                    } else if (op == DP2_CREATEPIXELSHADER) {
                        size_t cs = u32(e + 4);
                        need += 8 + cs;
                        if (need > left) return fail("truncated CREATEPIXELSHADER data");
                        create_pshader(u32(e), e + 8, (uint32_t)cs);
                    } else {
                        uint32_t reg = u32(e), cnt = u32(e + 4);
                        need += 8 + (size_t)cnt * 16;
                        if (need > left) return fail("truncated shader constants");
                        /* DXVK indexes its constant arrays with these: keep them inside d3d9's */
                        bool ok = cnt && (uint64_t)reg + cnt <= (op == DP2_SETVERTEXSHADERCONST ? 256u : 224u);
                        tr("%s constants c%u.. x%u%s", op == DP2_SETVERTEXSHADERCONST ? "vertex shader" : "pixel shader", reg, cnt, ok ? "" : " (dropped)");
                        if (!ok) { if (cnt && d.warn_once(0xc1007 | (op << 16))) x.log("ddi: dp2: shader constants c%u x%u out of range, dropped", reg, cnt); continue; }
                        std::vector<float> f(cnt * 4);
                        memcpy(f.data(), e + 8, (size_t)cnt * 16);
                        if (op == DP2_SETVERTEXSHADERCONST) x.dev->SetVertexShaderConstantF(reg, f.data(), cnt);
                        else x.dev->SetPixelShaderConstantF(reg, f.data(), cnt);
                    }
                }
                break;
            }
            case DP2_DELETEVERTEXSHADER: case DP2_DELETEPIXELSHADER: case DP2_SETPIXELSHADER:
                need = count * 4u;
                if (need > left) return fail("truncated shader handle token");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t h = u32(q + 4 * i);
                    if (op == DP2_SETPIXELSHADER) set_pshader(h);
                    else if (op == DP2_DELETEVERTEXSHADER) { tr("delete vertex shader 0x%x", h); delete_vshader(h); }
                    else { tr("delete pixel shader 0x%x", h); delete_pshader(h); }
                }
                break;
            case DP2_SETSTREAMSOURCE: case DP2_DRAWPRIMITIVE: case DP2_DRAWPRIMITIVE2: case DP2_CLIPPEDTRIANGLEFAN:
                need = count * 12u;
                if (need > left) return fail("truncated DX8 stream token");
                if (d.warn_once(0xc0000 | op)) x.log("ddi: dp2: DX8 token %u reached the host (the driver did not rewrite it)", op);
                break;
            case DP2_SETSTREAMSOURCEUM: case DP2_SETINDICES:
                need = count * 8u;
                if (need > left) return fail("truncated DX8 stream token");
                break;
            case DP2_DRAWINDEXEDPRIMITIVE: case DP2_DRAWINDEXEDPRIMITIVE2:
                need = count * 24u;
                if (need > left) return fail("truncated DX8 draw token");
                if (d.warn_once(0xc0000 | op)) x.log("ddi: dp2: DX8 token %u reached the host (the driver did not rewrite it)", op);
                break;
            case DP2_DRAWRECTPATCH: case DP2_DRAWTRIPATCH: {
                need = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (left < need + 8) return fail("truncated patch token");
                    uint32_t fl = u32(q + need + 4);
                    need += 8 + ((fl & 1) ? (op == DP2_DRAWRECTPATCH ? 16 : 12) : 0) + ((fl & 2) ? (op == DP2_DRAWRECTPATCH ? 28 : 16) : 0);
                }
                if (need > left) return fail("truncated patch token");
                if (d.warn_once(0xc0000 | op)) x.log("ddi: dp2: patches are not supported: dropped");
                break;
            }
            case DP2_VOLUMEBLT:
                need = count * 48u;
                if (need > left) return fail("truncated VOLUMEBLT");
                if (d.warn_once(0xc0000 | op)) x.log("ddi: dp2: volume textures are not supported: VOLUMEBLT dropped");
                break;
            case DP2_BUFFERBLT:                                  /* done in the driver (VRAM buffers, v9); 24 bytes: dst, src, dst offset, D3DRANGE, one more */
                need = count * 24u;
                if (need > left) return fail("truncated BUFFERBLT");
                break;
            case DP2_ADDDIRTYRECT:
                need = count * 20u;
                if (need > left) return fail("truncated ADDDIRTYRECT");
                break;
            case DP2_ADDDIRTYBOX:
                need = count * 28u;
                if (need > left) return fail("truncated ADDDIRTYBOX");
                break;
            case DP2_CREATEVERTEXSHADERDECL:                     /* handle, n, n D3DVERTEXELEMENT9 */
                need = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (left < need + 8) return fail("truncated CREATEVERTEXSHADERDECL");
                    const uint8_t *e = q + need;
                    uint32_t n = u32(e + 4);
                    if (n > 64) return fail("CREATEVERTEXSHADERDECL with more than 64 elements");
                    need += 8 + (size_t)n * 8;
                    if (need > left) return fail("truncated CREATEVERTEXSHADERDECL elements");
                    create_vdecl(u32(e), e + 8, n);
                }
                break;
            case DP2_CREATEVERTEXSHADERFUNC:                     /* handle, code size, code */
                need = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (left < need + 8) return fail("truncated CREATEVERTEXSHADERFUNC");
                    const uint8_t *e = q + need;
                    size_t cs = u32(e + 4);
                    need += 8 + cs;
                    if (need > left) return fail("truncated CREATEVERTEXSHADERFUNC code");
                    create_vfunc(u32(e), e + 8, (uint32_t)cs);
                }
                break;
            case DP2_DELETEVERTEXSHADERDECL: case DP2_SETVERTEXSHADERDECL: case DP2_DELETEVERTEXSHADERFUNC: case DP2_SETVERTEXSHADERFUNC:
                need = count * 4u;
                if (need > left) return fail("truncated DX9 shader handle token");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t h = u32(q + 4 * i);
                    if (op == DP2_DELETEVERTEXSHADERDECL) { tr("delete vertex declaration 0x%x", h); delete_vshader(h); }
                    else if (op == DP2_DELETEVERTEXSHADERFUNC) { tr("delete vertex shader function 0x%x", h); delete_vfunc(h); }
                    else if (op == DP2_SETVERTEXSHADERDECL) { tr("vertex declaration 0x%x", h); apply_vs(h); }
                    else {
                        if (h && !c.vfuncs.count(h) && d.warn_once(0xc1015)) x.log("ddi: dp2: vertex shader function 0x%x unknown (the fixed function instead)", h);
                        tr("vertex shader function 0x%x", h);
                        c.dx9 = true;
                        c.vfunc = c.vfuncs.count(h) ? h : 0;
                    }
                }
                break;
            case DP2_SETVERTEXSHADERCONSTI: case DP2_SETPIXELSHADERCONSTI:
            case DP2_SETVERTEXSHADERCONSTB: case DP2_SETPIXELSHADERCONSTB: {
                /* register, count, then count int4s (I) or BOOLs (B); d3d9
                 * has 16 of each, per stage */
                bool ints = op == DP2_SETVERTEXSHADERCONSTI || op == DP2_SETPIXELSHADERCONSTI;
                bool vsc = op == DP2_SETVERTEXSHADERCONSTI || op == DP2_SETVERTEXSHADERCONSTB;
                need = 0;
                for (uint32_t i = 0; i < count; i++) {
                    if (left < need + 8) return fail("truncated shader constants");
                    const uint8_t *e = q + need;
                    uint32_t reg = u32(e), cnt = u32(e + 4);
                    if (cnt > 256) return fail("shader constant count out of range");
                    need += 8 + (size_t)cnt * (ints ? 16 : 4);
                    if (need > left) return fail("truncated shader constants");
                    bool ok = cnt && (uint64_t)reg + cnt <= 16;
                    tr("%s %s constants %u.. x%u%s", vsc ? "vertex shader" : "pixel shader", ints ? "integer" : "boolean", reg, cnt, ok ? "" : " (dropped)");
                    if (!ok) { if (cnt && d.warn_once(0xc1016 | (op << 16))) x.log("ddi: dp2: %s constants %u x%u out of range, dropped", ints ? "integer" : "boolean", reg, cnt); continue; }
                    std::vector<int> v((size_t)cnt * (ints ? 4 : 1));
                    memcpy(v.data(), e + 8, v.size() * 4);
                    if (ints && vsc) x.dev->SetVertexShaderConstantI(reg, v.data(), cnt);
                    else if (ints) x.dev->SetPixelShaderConstantI(reg, v.data(), cnt);
                    else if (vsc) x.dev->SetVertexShaderConstantB(reg, (const BOOL *)v.data(), cnt);
                    else x.dev->SetPixelShaderConstantB(reg, (const BOOL *)v.data(), cnt);
                }
                break;
            }
            case DP2_BLT:
                /* v18: StretchRect between two depth buffers: whole
                 * surfaces of one size, as Direct3D 9 allows it, outside a
                 * scene. v22: also between two colour render targets, the
                 * rectangles and the filter (flags: 1 point, 2 linear) as
                 * given; the WDDM driver sends it for a surface the CPU
                 * cannot map (a shared one: DWM's redirection surface of a
                 * windowed Direct3D program). Every other BLT is the
                 * driver's. */
                need = count * 52u;
                if (need > left) return fail("truncated BLT");
                for (uint32_t i = 0; i < count; i++) {
                    const uint8_t *e = q + 52 * i;
                    VramSurf *src = surf(x, u32(e)), *dst = surf(x, u32(e + 24));
                    RECT sr, dr;
                    memcpy(&sr, e + 4, sizeof sr); memcpy(&dr, e + 28, sizeof dr);
                    const uint32_t colour = D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY;
                    if (src && dst && (src->d.caps & colour) && (dst->d.caps & colour) &&
                        !((src->d.caps | dst->d.caps) & D3DPT_VS_ZBUFFER)) {
                        tr("colour blt %u (%d,%d,%d,%d) -> %u (%d,%d,%d,%d)", u32(e), (int)sr.left, (int)sr.top, (int)sr.right,
                           (int)sr.bottom, u32(e + 24), (int)dr.left, (int)dr.top, (int)dr.right, (int)dr.bottom);
                        if (!ensure_object(x, *src) || !ensure_object(x, *dst) || !src->rt || !dst->rt) {
                            if (d.warn_once(0xf0002)) x.log("ddi: dp2: colour BLT %u -> %u: no host target, dropped", u32(e), u32(e + 24));
                            continue;
                        }
                        if (src->dirty) upload_target(x, d, *src);
                        bool whole = dr.left == 0 && dr.top == 0 && (uint32_t)dr.right == dst->d.width && (uint32_t)dr.bottom == dst->d.height;
                        if (dst->dirty && !whole) upload_target(x, d, *dst);
                        x.scene_end();
                        HRESULT hr = x.dev->StretchRect(src->rt, &sr, dst->rt, &dr, (u32(e + 48) & 2) ? D3DTEXF_LINEAR : D3DTEXF_NONE);
                        if (FAILED(hr)) {
                            if (d.warn_once(0xf0003)) x.log("ddi: dp2: colour StretchRect %u -> %u: 0x%08x", u32(e), u32(e + 24), (unsigned)hr);
                            continue;
                        }
                        dst->dirty = false;
                        dst->rendered = true;
                        dst->checked = true;
                        if (dst->d.caps & D3DPT_VS_AUTOGEN) dst->mips_stale = true;
                        continue;
                    }
                    tr("depth blt %u -> %u", u32(e), u32(e + 24));
                    if (!src || !dst || !(src->d.caps & D3DPT_VS_ZBUFFER) || !(dst->d.caps & D3DPT_VS_ZBUFFER) ||
                        !ensure_object(x, *src) || !ensure_object(x, *dst) || !src->rt || !dst->rt ||
                        src->d.width != dst->d.width || src->d.height != dst->d.height) {
                        if (d.warn_once(0xf0000)) x.log("ddi: dp2: BLT %u -> %u is no blit between two depth buffers of one size or two colour targets, dropped", u32(e), u32(e + 24));
                        continue;
                    }
                    x.scene_end();
                    HRESULT hr = x.dev->StretchRect(src->rt, nullptr, dst->rt, nullptr, D3DTEXF_NONE);
                    if (FAILED(hr) && d.warn_once(0xf0001)) x.log("ddi: dp2: depth StretchRect %u -> %u: 0x%08x", u32(e), u32(e + 24), (unsigned)hr);
                }
                break;
            case DP2_SETRENDERTARGET2:
                /* v17: render targets 1..3 {index, surface handle; 0 = none}
                 * (the driver turns index 0 into SETRENDERTARGET) */
                need = count * 8u;
                if (need > left) return fail("truncated SETRENDERTARGET2");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t idx = u32(q + 8 * i), h = u32(q + 8 * i + 4);
                    VramSurf *m = h ? surf(x, h) : nullptr;
                    tr("render target %u: %u%s", idx, h, h && !m ? " (unknown)" : "");
                    if (idx < 1 || idx > 3) return fail("SETRENDERTARGET2 index");
                    if (h && (!m || !(m->d.caps & (D3DPT_VS_RENDER_TARGET | D3DPT_VS_PRIMARY)) || (m->d.caps & D3DPT_VS_ZBUFFER))) {
                        if (d.warn_once(0x60001)) x.log("ddi: dp2: render target %u: surface %u is no target, unbound", idx, h);
                        h = 0;
                    }
                    c.mrt[idx] = h;
                }
                if (surf(x, c.rt)) bind_ctx(x, d, c, b, false);
                break;
            case DP2_GENERATEMIPSUBLEVELS:
                /* surface handle, D3DTEXTUREFILTERTYPE: an autogen texture's
                 * levels from its level 0, uploaded first if the guest wrote
                 * it (v15, M16); any other surface is left as it is */
                need = count * 8u;
                if (need > left) return fail("truncated GENERATEMIPSUBLEVELS");
                for (uint32_t i = 0; i < count; i++) {
                    uint32_t h = u32(q + 8 * i), filter = u32(q + 8 * i + 4);
                    VramSurf *s = surf(x, h);
                    tr("generate mip sublevels of %u, filter %u%s", h, filter, s && (s->d.caps & D3DPT_VS_AUTOGEN) ? "" : " (not an autogen texture)");
                    if (!s || !(s->d.caps & D3DPT_VS_AUTOGEN) || !ensure_object(x, *s) || !s->tex) {
                        if (d.warn_once(0xe0000)) x.log("ddi: dp2: GENERATEMIPSUBLEVELS on surface %u, which is no autogen texture", h);
                        continue;
                    }
                    if (s->dirty) {
                        if (s->rt) upload_target(x, d, *s);
                        else upload_texture(x, d, *s);
                    }
                    s->tex->SetAutoGenFilterType(filter == D3DTEXF_POINT ? D3DTEXF_POINT : D3DTEXF_LINEAR);
                    s->tex->PreLoad();          /* a managed one's video copy current first (upload_texture) */
                    s->tex->GenerateMipSubLevels();
                    s->mips_stale = false;
                }
                break;
            case DP2_SETSCISSORRECT:
                need = count * 16u;
                if (need > left) return fail("truncated SETSCISSORRECT");
                if (count) {
                    RECT r; memcpy(&r, q + 16 * (count - 1), sizeof r);
                    tr("scissor %ld,%ld..%ld,%ld", (long)r.left, (long)r.top, (long)r.right, (long)r.bottom);
                    if (r.left <= r.right && r.top <= r.bottom) x.dev->SetScissorRect(&r);
                }
                break;
            default:
                return fail_token(op, count);
            }
            p = q + need;
        }
        return true;
    }
};

} // namespace

void exec_ddi_release(Exec &x)
{
    if (!x.ddi) return;
    for (auto &kv : x.ddi->surfs) kv.second.release();
    for (auto &kv : x.ddi->ctxs) kv.second.release_shaders();
    for (auto &kv : x.ddi->sblocks) if (kv.second) kv.second->Release();
    x.ddi->sblocks.clear();
    if (x.ddi->fresh) { x.ddi->fresh->Release(); x.ddi->fresh = nullptr; }
    x.ddi->lights_on.reset();
    x.ddi->drop_stage();
    delete x.ddi;
    x.ddi = nullptr;
}

/* Windows' own Direct3D 9 can lose a device (a driver reset, another
 * program taking the display); DXVK's never is, so this is the native
 * backend's alone. Everything the host holds for the display driver is a
 * copy of something in guest VRAM, and the guest sends a VRAM_SURFACE
 * once and never again, so the copies go and the registrations stay,
 * each surface marked dirty and made again at its next use. The DX8
 * shaders and state sets go too: the runtime re-creates them on the
 * device it is handed, and a stale handle draws nothing. */
void exec_ddi_device_reset(Exec &x)
{
    if (!x.ddi) return;
    for (auto &kv : x.ddi->surfs) {
        kv.second.release();
        kv.second.dirty = true;
        kv.second.rendered = false;
        kv.second.checked = false;
        kv.second.shadow.clear();
    }
    for (auto &kv : x.ddi->ctxs) kv.second.release_shaders();
    for (auto &kv : x.ddi->sblocks) if (kv.second) kv.second->Release();
    x.ddi->sblocks.clear();
    if (x.ddi->fresh) { x.ddi->fresh->Release(); x.ddi->fresh = nullptr; }
    x.ddi->lights_on.reset();
    x.ddi->recording = false;
    x.ddi->drop_stage();
    x.ddi->bound_rt = x.ddi->bound_z = 0;
    memset(x.ddi->stage_tex, 0, sizeof x.ddi->stage_tex);
    x.ddi->reset_addr();
    x.ddi->clip_tl = false; x.ddi->clip_host = ~0u;
    x.log("ddi: %zu surfaces dropped for the device reset; each is re-read from VRAM", x.ddi->surfs.size());
}

bool exec_ddi_op(Batch &b, const d3dpt_cmd *c)
{
    Exec &x = b.x;
    switch (c->op) {
    case D3DPT_OP_VRAM_SURFACE: {
        auto *a = body<d3dpt_vram_surface>(c, 0, b); if (!a) return true;
        if (!x.vram) { b.err = D3DPT_ERR_NO_DEVICE; return true; }
        if (a->caps & D3DPT_VS_BUFFER) {
            /* v9: a vertex / index buffer in VRAM: width = pitch = bytes,
             * height 1, no format, no levels, nothing else; no host object
             * (a DRAW8 reads its range from VRAM) */
            if (!a->handle || !a->width || a->height != 1 || a->pitch != a->width || a->format || a->levels > 1 ||
                (a->caps & ~D3DPT_VS_BUFFER) || (uint64_t)a->offset + a->width > x.vram_size ||
                c->size < sizeof(d3dpt_cmd) + sizeof *a) {
                if (ddi(x).warn_once(0x72000000u | (a->handle & 0xffffffu)))
                    x.log("ddi: buffer %u refused: %u bytes pitch %u height %u format %u levels %u caps 0x%x offset 0x%x", a->handle,
                          a->width, a->pitch, a->height, a->format, a->levels, a->caps, a->offset);
                b.err = D3DPT_ERR_BAD_ARG;
                return true;
            }
            Ddi &d = ddi(x);
            VramSurf &s = d.surfs[a->handle];
            if (s.tex || s.rt || s.cube || s.vol) s.release();       /* the handle was a texture / target before */
            s.shadow.clear();
            s.levels.clear();
            s.d = *a;
            s.d.levels = 1;
            s.cube_root = s.face = 0;
            s.depth = s.slice = 0;
            memset(s.face_h, 0, sizeof s.face_h);
            s.dirty = true;
            s.rendered = false;
            break;
        }
        uint32_t levels = a->levels ? a->levels : 1;
        bool cube = (a->caps & D3DPT_VS_CUBE) != 0, vol = (a->caps & D3DPT_VS_VOLUME) != 0;
        uint32_t nlv = cube ? D3DPT_CUBE_FACES * levels - 1 : levels - 1;     /* the tail's level entries (v11: face-major) */
        uint32_t row = fmt_row_bytes(a->format, a->width), rows = fmt_rows(a->format, a->height);
        /* a refused surface says which, once per handle: the batch's later
         * records (a context, its draws) are lost with it */
        auto refuse = [&](const char *why) {
            if (ddi(x).warn_once(0x71000000u | (a->handle & 0xffffffu)))
                x.log("ddi: surface %u refused (%s): %ux%u format %u pitch %u offset 0x%x levels %u caps 0x%x", a->handle, why,
                      a->width, a->height, a->format, a->pitch, a->offset, a->levels, a->caps);
            b.err = D3DPT_ERR_BAD_ARG;
            return true;
        };
        if (!a->handle || !a->width || !a->height || a->width > 8192 || a->height > 8192 || levels > 16 ||
            (cube && (a->width != a->height || !(a->caps & D3DPT_VS_TEXTURE) || (a->caps & (D3DPT_VS_PRIMARY | D3DPT_VS_ZBUFFER)))) ||
            (vol && (cube || !(a->caps & D3DPT_VS_TEXTURE) || (a->caps & (D3DPT_VS_PRIMARY | D3DPT_VS_ZBUFFER | D3DPT_VS_RENDER_TARGET)) ||
                     a->width > D3DPT_VOLUME_MAX_DEPTH || a->height > D3DPT_VOLUME_MAX_DEPTH)) ||
            /* v15: an autogen texture is a 2D texture of one level the guest keeps */
            ((a->caps & D3DPT_VS_AUTOGEN) && (cube || vol || levels != 1 || !(a->caps & D3DPT_VS_TEXTURE) ||
                                              (a->caps & (D3DPT_VS_PRIMARY | D3DPT_VS_ZBUFFER | D3DPT_VS_SAMPLES_MASK)))) ||
            c->size < sizeof(d3dpt_cmd) + sizeof *a + (nlv + (vol ? 1 : 0)) * sizeof(d3dpt_u32x2)) return refuse("shape");
        if (!row) {
            if (ddi(x).warn_once(0x70000 | a->format)) x.log("ddi: surface format %u (0x%08x) not mirrored", a->format, a->format);
            return true;
        }
        if (a->pitch < row || (uint64_t)a->offset + (uint64_t)a->pitch * rows > x.vram_size) return refuse("pitch or VRAM range");
        const d3dpt_u32x2 *lv = (const d3dpt_u32x2 *)tail(a);
        for (uint32_t i = 1; i <= nlv; i++) {
            uint32_t l = i % levels, w = a->width >> l, h = a->height >> l;     /* a cube's entry i is face i / levels */
            if (!w) w = 1;
            if (!h) h = 1;
            if (lv[i - 1].b < fmt_row_bytes(a->format, w) ||
                (uint64_t)lv[i - 1].a + (uint64_t)lv[i - 1].b * fmt_rows(a->format, h) > x.vram_size) return refuse("a level");
        }
        /* v12: a volume's {depth, slice pitch} after its levels; every slice of every level inside VRAM */
        uint32_t depth = 0, slice = 0;
        if (vol) {
            depth = lv[nlv].a;
            slice = lv[nlv].b;
            if (!depth || depth > D3DPT_VOLUME_MAX_DEPTH || (uint64_t)slice < (uint64_t)a->pitch * rows ||
                (uint64_t)a->offset + (uint64_t)slice * depth > x.vram_size) return refuse("volume depth");
            for (uint32_t l = 1; l < levels; l++) {
                uint32_t h = a->height >> l, dep = depth >> l;
                if (!h) h = 1;
                if (!dep) dep = 1;
                if ((uint64_t)lv[l - 1].a + (uint64_t)lv[l - 1].b * fmt_rows(a->format, h) * dep > x.vram_size) { b.err = D3DPT_ERR_BAD_ARG; return true; }
            }
        }
        Ddi &d = ddi(x);
        VramSurf &s = d.surfs[a->handle];
        /* a level of a live render-target texture (v19) stays that when the
         * runtime registers the same level on its own as well; a handle the
         * runtime gave another surface since is that surface */
        if (s.mip_root && surf(x, s.mip_root) && a->offset == s.d.offset && a->width == s.d.width && a->height == s.d.height &&
            !(a->caps & (D3DPT_VS_CUBE | D3DPT_VS_VOLUME | D3DPT_VS_BUFFER)))
            break;
        if (s.mip_root) {
            VramSurf *r = surf(x, s.mip_root);
            if (r) r->mip_h.erase(std::remove(r->mip_h.begin(), r->mip_h.end(), a->handle), r->mip_h.end());
            s.release();
        }
        s.mip_root = s.mip_level = 0;
        d3dpt_vram_surface nd = *a;
        nd.levels = levels;
        /* the same surface again (a flip moved it): keep the host object if it still fits */
        if ((s.tex || s.rt || s.cube || s.vol) && (s.d.width != nd.width || s.d.height != nd.height || s.d.format != nd.format ||
                                          s.d.caps != nd.caps || s.d.levels != nd.levels || s.depth != depth)) {
            /* a cube's face entries hold surfaces of the object going away */
            for (uint32_t f = 0; f < D3DPT_CUBE_FACES; f++) {
                VramSurf *fs = s.face_h[f] ? surf(x, s.face_h[f]) : nullptr;
                if (fs) { fs->release(); fs->dirty = true; }
            }
            for (uint32_t h : s.mip_h) {                   /* v19: its levels' surfaces too */
                VramSurf *ls = surf(x, h);
                if (ls && ls->mip_root == a->handle) { ls->release(); ls->dirty = true; }
            }
            s.release();
        }
        /* moved (a runtime that swaps two flip buffers' memory, doc 15): what the shadow
         * remembers of the old memory says nothing about the new, or the next readback
         * would keep every differing pixel as the guest's */
        if (s.d.offset != nd.offset || s.d.pitch != nd.pitch) { s.dirty = true; s.shadow.clear(); }
        if (!s.tex && !s.rt && !s.cube && !s.vol) s.dirty = true;
        s.d = nd;
        s.levels.assign(lv, lv + nlv);
        s.depth = depth;
        s.slice = slice;
        s.cube_root = s.face = 0;
        if (!cube) memset(s.face_h, 0, sizeof s.face_h);
        break;
    }
    case D3DPT_OP_VRAM_MIP_LEVEL: {
        /* v19: a render-target texture's level l under its own handle: a
         * target SETRENDERTARGET / READBACK / VRAM_DIRTY can name, the
         * texture's surface of that level on the host */
        auto *a = body<d3dpt_u32x4>(c, 0, b); if (!a) return true;
        if (!a->a || a->a == a->b || a->c < 1 || a->c > 15) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        VramSurf *r = surf(x, a->b);
        if (!r || !(r->d.caps & D3DPT_VS_TEXTURE) || !(r->d.caps & D3DPT_VS_RENDER_TARGET) || (r->d.caps & (D3DPT_VS_CUBE | D3DPT_VS_VOLUME)) ||
            r->mip_root) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        if (a->c >= r->d.levels || a->c > r->levels.size()) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        uint32_t root_h = a->b;
        d3dpt_u32x2 lvl = r->levels[a->c - 1];
        d3dpt_vram_surface rd = r->d;
        VramSurf &s = ddi(x).surfs[a->a];               /* node-based map: r stays valid */
        r = surf(x, root_h);
        if (s.tex || s.rt || s.cube || s.vol) s.release();
        s.shadow.clear();
        s.levels.clear();
        s.mip_h.clear();
        s.d = rd;
        s.d.handle = a->a;
        s.d.offset = lvl.a;
        s.d.pitch = lvl.b;
        s.d.width = rd.width >> a->c ? rd.width >> a->c : 1;
        s.d.height = rd.height >> a->c ? rd.height >> a->c : 1;
        s.d.caps = rd.caps & ~D3DPT_VS_AUTOGEN;
        s.d.levels = 1;
        s.cube_root = s.face = 0;
        memset(s.face_h, 0, sizeof s.face_h);
        s.mip_root = root_h;
        s.mip_level = a->c;
        s.dirty = true;
        s.rendered = false;
        if (std::find(r->mip_h.begin(), r->mip_h.end(), a->a) == r->mip_h.end()) r->mip_h.push_back(a->a);
        break;
    }
    case D3DPT_OP_VRAM_CUBE_FACE: {
        /* v11: a cube face's level 0 under its own handle, what a
         * SETRENDERTARGET / READBACK of a render-target cube names, and
         * whose VRAM_DIRTY means the cube */
        auto *a = body<d3dpt_u32x4>(c, 0, b); if (!a) return true;
        if (!a->a || a->a == a->b || a->c < 1 || a->c >= D3DPT_CUBE_FACES) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        VramSurf *r = surf(x, a->b);
        if (!r || !(r->d.caps & D3DPT_VS_CUBE)) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        uint32_t i = a->c * r->d.levels;                 /* face c's level 0 in the level table (entry i - 1) */
        if (i > r->levels.size()) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        VramSurf &s = ddi(x).surfs[a->a];               /* node-based map: r stays valid */
        if (s.tex || s.rt || s.cube || s.vol) s.release();
        s.shadow.clear();
        s.levels.clear();
        s.d = r->d;
        s.d.handle = a->a;
        s.d.offset = r->levels[i - 1].a;
        s.d.pitch = r->levels[i - 1].b;
        s.d.caps = r->d.caps & ~D3DPT_VS_CUBE;
        s.d.levels = 1;
        s.cube_root = a->b;
        s.face = a->c;
        memset(s.face_h, 0, sizeof s.face_h);
        s.dirty = true;
        s.rendered = false;
        r->face_h[a->c] = a->a;
        break;
    }
    case D3DPT_OP_VRAM_RELEASE: {
        auto *a = body<d3dpt_handle>(c, 0, b); if (!a) return true;
        if (!x.ddi) return true;
        auto it = x.ddi->surfs.find(a->handle);
        if (it == x.ddi->surfs.end()) return true;
        if (x.ddi->bound_rt == a->handle || x.ddi->bound_z == a->handle) {
            /* the device may still reference it: unbind first */
            if (x.dev) { x.dev->SetDepthStencilSurface(nullptr); }
            x.ddi->bound_rt = x.ddi->bound_z = 0;
        }
        for (uint32_t &h : x.ddi->stage_tex) if (h == a->handle) h = 0;
        /* a context whose target or depth this was names none until the
         * runtime's next SETRENDERTARGET (the handle may be reused first) */
        for (auto &kv : x.ddi->ctxs) {
            if (kv.second.rt == a->handle) kv.second.rt = 0;
            if (kv.second.z == a->handle) kv.second.z = 0;
            for (uint32_t &m : kv.second.mrt) if (m == a->handle) m = 0;
        }
        for (uint32_t i = 1; i < 4; i++)
            if (x.ddi->bound_mrt[i] == a->handle) {
                if (x.dev) x.dev->SetRenderTarget(i, nullptr);
                x.ddi->bound_mrt[i] = 0;
            }
        VramSurf &gone = it->second;
        for (uint32_t f = 0; f < D3DPT_CUBE_FACES; f++) {   /* a cube: its faces' surfaces of it go too */
            VramSurf *fs = gone.face_h[f] ? surf(x, gone.face_h[f]) : nullptr;
            if (fs) { fs->release(); fs->dirty = true; }
        }
        for (uint32_t h : gone.mip_h) {                     /* v19: a texture's levels become plain handles */
            VramSurf *ls = surf(x, h);
            if (ls && ls->mip_root == a->handle) { ls->release(); ls->mip_root = ls->mip_level = 0; ls->dirty = true; }
        }
        if (gone.cube_root) {                           /* a face: its cube forgets it */
            VramSurf *r = surf(x, gone.cube_root);
            if (r && r->face_h[gone.face] == a->handle) r->face_h[gone.face] = 0;
        }
        gone.release();
        x.ddi->surfs.erase(it);
        break;
    }
    case D3DPT_OP_VRAM_DIRTY: {
        auto *a = body<d3dpt_handle>(c, 0, b); if (!a) return true;
        VramSurf *s = surf(x, a->handle);
        if (s) { s->dirty = true; s->rendered = false; }
        if (s && s->cube_root) {
            /* a cube face (v11): a plain cube is read again whole; a
             * render-target cube uploads the faces whose entries are dirty */
            VramSurf *r = surf(x, s->cube_root);
            if (r && r->rt) r->faces_dirty = true;
            else if (r) r->dirty = true;
            if (x.ddi->warn_once(0xd0001))
                x.log("ddi: cube face %u written by the guest: cube %u %s", a->handle, s->cube_root, r && r->rt ? "uploads that face" : "is read again whole");
        }
        break;
    }
    case D3DPT_OP_VRAM_DIRTY_RANGE: {
        /* v9: a range of a VRAM buffer the guest wrote (Unlock, BUFFERBLT).
         * Nothing is cached of a buffer yet (a DRAW8 reads it from VRAM),
         * so this only keeps the flag honest (and counts, for the log) */
        auto *a = body<d3dpt_u32x3>(c, 0, b); if (!a) return true;
        VramSurf *s = surf(x, a->a);
        if (s) { s->dirty = true; ddi(x).buffer_writes++; ddi(x).buffer_bytes += a->c; }
        break;
    }
    case D3DPT_OP_VRAM_COLORKEY: {
        auto *a = body<d3dpt_u32x4>(c, 0, b); if (!a) return true;
        VramSurf *s = surf(x, a->a);
        if (ddi(x).pal_lines < 48) { ddi(x).pal_lines++; x.log("ddi: colour key on surface %u: 0x%x..0x%x flags %u%s", a->a, a->b, a->c, a->d, s ? "" : " (unknown)"); }
        if (!s) { if (ddi(x).warn_once(0xd0001)) x.log("ddi: colour key on unknown surface %u", a->a); break; }
        bool on = (a->d & 1) != 0;
        if (s->ckey != on || s->ckey_lo != a->b || s->ckey_hi != a->c) {
            s->ckey = on; s->ckey_lo = a->b; s->ckey_hi = a->c;
            refresh_object(*s);
        }
        break;
    }
    case D3DPT_OP_CTX_CREATE: {
        auto *a = body<d3dpt_ctx_create>(c, 0, b); if (!a) return true;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return true;
        Ddi &d = ddi(x);
        if (!a->handle) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        auto stale = d.ctxs.find(a->handle);
        if (stale != d.ctxs.end()) {
            /* the guest never destroyed it (a display driver that lost its context table
             * with the PDEV, as old drivers did): the new one takes its place */
            x.log("ddi: context %u still open, replaced", a->handle);
            if (x.dev && (!stale->second.vshaders.empty() || !stale->second.pshaders.empty())) { x.dev->SetVertexShader(nullptr); x.dev->SetPixelShader(nullptr); }
            stale->second.release_shaders();
            d.ctxs.erase(stale);
        }
        VramSurf *rt = surf(x, a->rt);
        if (!rt) { r->hr = (uint32_t)D3DERR_INVALIDCALL; x.log("ddi: context %u: render target %u unknown", a->handle, a->rt); return true; }
        /* the device as it was new: its state then, no light on (the
         * runtime sends the new context's own render and stage states,
         * never a light it has not enabled) */
        if (x.dev) {
            save_ctx(x, d);                     /* the context that had the device keeps its state */
            if (!d.fresh && d.ctxs.empty()) x.dev->CreateStateBlock(D3DSBT_ALL, &d.fresh);
            else if (d.fresh) {
                for (uint32_t i = 0; i < d.lights_on.size(); i++) if (d.lights_on[i]) x.dev->LightEnable(i, FALSE);
                d.lights_on.reset();
                d.fresh->Apply();
                memset(d.stage_tex, 0, sizeof d.stage_tex);
                d.reset_addr();
                d.clip_tl = false; d.clip_host = ~0u;
                d.bound_rt = d.bound_z = 0;
                memset(d.bound_mrt, 0, sizeof d.bound_mrt);
            }
        }
        Ctx cx;
        cx.rt = a->rt;
        cx.z = surf(x, a->z) ? a->z : 0;
        cx.vp = { 0, 0, rt->d.width, rt->d.height, 0.0f, 1.0f };
        cx.np2cond = (a->flags & D3DPT_CTX_NP2_CONDITIONAL) != 0;
        if (!ensure_object(x, *rt) || (cx.z && !ensure_object(x, *surf(x, cx.z)))) { r->hr = (uint32_t)E_FAIL; return true; }
        d.ctxs[a->handle] = cx;
        if (x.dev) d.cur_ctx = a->handle;      /* the device's state is the new context's now */
        x.log("ddi: context %u on %ux%u fmt %u (z %u%s), %zu contexts", a->handle, rt->d.width, rt->d.height, rt->d.format, cx.z,
              cx.np2cond ? ", conditional NP2" : "", d.ctxs.size());
        r->hr = S_OK;
        break;
    }
    case D3DPT_OP_CTX_DESTROY: {
        auto *a = body<d3dpt_handle>(c, 0, b); if (!a) return true;
        if (x.ddi) {
            auto it = x.ddi->ctxs.find(a->handle);
            if (it != x.ddi->ctxs.end()) {
                if (x.dev && (!it->second.vshaders.empty() || !it->second.pshaders.empty())) { x.dev->SetVertexShader(nullptr); x.dev->SetPixelShader(nullptr); }
                it->second.release_shaders();
                x.ddi->ctxs.erase(it);
                if (x.ddi->cur_ctx == a->handle) x.ddi->cur_ctx = 0;
            }
        }
        break;
    }
    case D3DPT_OP_CTX_SET_RT: {
        auto *a = body<d3dpt_u32x3>(c, 0, b); if (!a) return true;
        if (!x.ddi) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        auto it = x.ddi->ctxs.find(a->a);
        if (it == x.ddi->ctxs.end() || !surf(x, a->b)) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        it->second.rt = a->b;
        it->second.z = surf(x, a->c) ? a->c : 0;
        VramSurf *rt = surf(x, a->b);
        it->second.vp = { 0, 0, rt->d.width, rt->d.height, 0.0f, 1.0f };
        use_ctx(x, *x.ddi, it->first, it->second);
        bind_ctx(x, *x.ddi, it->second, b, false);
        break;
    }
    case D3DPT_OP_CTX_CLEAR: {
        auto *a = body<d3dpt_ctx_clear>(c, 0, b); if (!a) return true;
        if (a->count > 64 || c->size < sizeof(d3dpt_cmd) + sizeof *a + a->count * sizeof(D3DRECT)) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        if (!x.ddi) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        auto it = x.ddi->ctxs.find(a->ctx);
        if (it == x.ddi->ctxs.end()) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        if (!x.dev) { b.err = D3DPT_ERR_NO_DEVICE; return true; }
        use_ctx(x, *x.ddi, it->first, it->second);
        Dp2 p = { x, *x.ddi, it->second, b, nullptr, nullptr, nullptr, 0, 0 };
        p.clear(a->flags, a->color, a->z, a->stencil, a->count, tail(a));
        break;
    }
    case D3DPT_OP_DP2: {
        auto *a = body<d3dpt_dp2>(c, 0, b); if (!a) return true;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return true;
        uint64_t cmd_aligned = D3DPT_ALIGN8(a->command_bytes);
        uint32_t stride = stride_of_fvf(a->fvf);
        if (a->command_bytes > (48u << 20) || a->vertex_bytes > (32u << 20) ||
            c->size < sizeof(d3dpt_cmd) + sizeof *a + cmd_aligned + a->vertex_bytes) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        /* a record without vertices (DX8: every draw carries its own) may name any FVF */
        if (a->vertex_bytes && (!stride || stride != a->vertex_stride || a->vertex_bytes % stride)) { b.err = D3DPT_ERR_BAD_ARG; return true; }
        if (!x.ddi) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        auto it = x.ddi->ctxs.find(a->ctx);
        if (it == x.ddi->ctxs.end()) { b.err = D3DPT_ERR_BAD_HANDLE; return true; }
        if (!x.dev) { b.err = D3DPT_ERR_NO_DEVICE; return true; }
        use_ctx(x, *x.ddi, it->first, it->second);
        const uint8_t *cmds = tail(a);
        Dp2 p = { x, *x.ddi, it->second, b, cmds, cmds + a->command_bytes, cmds + cmd_aligned, stride, stride ? a->vertex_bytes / stride : 0, a->fvf };
        x.ddi->dp2_calls++;
        if (x.ddi->trace_flag && !x.ddi->trace && !x.ddi->trace_armed && file_exists(x.ddi->trace_flag)) {
            x.ddi->trace_armed = true;                 /* the trace starts with the next frame */
            x.log("ddi: trace: armed at dp2 call %u", x.ddi->dp2_calls);
        }
        if (x.ddi->trace) x.log("ddi: trace: dp2 call %u: ctx %u flags 0x%x fvf 0x%x stride %u, %u vertices, %u command bytes", x.ddi->dp2_calls, a->ctx, a->flags, a->fvf, stride, stride ? a->vertex_bytes / stride : 0, a->command_bytes);
        if (stride) {                                   /* a DX7 record: its vertices, the fixed function */
            x.dev->SetVertexShader(nullptr); x.dev->SetFVF(a->fvf); p.cur_vs = a->fvf;
            p.apply_clip((a->fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZRHW);
        }
        bool ok = p.run();
        r->hr = b.err ? (uint32_t)E_FAIL : ok ? (uint32_t)S_OK : (uint32_t)p.hr;
        r->bytes = ok ? 0 : p.pos;
        x.ddi->flush_hint(x.dev);
        break;
    }
    case D3DPT_OP_READBACK: {
        auto *a = body<d3dpt_sync>(c, 0, b); if (!a) return true;
        d3dpt_ret *r = b.slot(a->ret_off, 0); if (!r) return true;
        VramSurf *s = surf(x, a->handle);
        if (!s) { r->hr = (uint32_t)D3DERR_INVALIDCALL; return true; }
        bool had = s->rendered;
        r->hr = had ? (uint32_t)readback(x, *x.ddi, *s) : (uint32_t)S_FALSE;
        if (x.ddi->trace && had && !x.ddi->trace_draws) {
            /* a readback with no draw before it (Crimson Skies reads its target back
             * after every render-target switch): not the frame that was asked for */
            x.log("ddi: trace: readback of %u with no draw, the frame goes on", a->handle);
        } else if (x.ddi->trace && had) {
            x.ddi->trace = false;
            unlink(x.ddi->trace_flag);
            x.log("ddi: trace: frame ends (readback of %u)", a->handle);
        } else if (x.ddi->trace_armed && had) {
            Ddi &d = *x.ddi;
            d.trace_armed = false;
            d.trace = true;
            d.trace_draws = 0;
            x.log("ddi: trace: frame starts after dp2 call %u; the states set so far:", d.dp2_calls);
            char line[400]; size_t n = 0;
            for (uint32_t i = 0; i < 256; i++) if (d.rs_set[i]) {
                n += (size_t)snprintf(line + n, sizeof line - n, " %u=0x%x", i, d.rs_val[i]);
                if (n > sizeof line - 40) { x.log("ddi: trace:   rs%s", line); n = 0; }
            }
            if (n) x.log("ddi: trace:   rs%s", line);
            for (uint32_t st = 0; st < 8; st++) {
                n = 0;
                for (uint32_t i = 0; i < 33; i++) if (d.tss_set[st][i]) n += (size_t)snprintf(line + n, sizeof line - n, " %u=0x%x", i, d.tss_val[st][i]);
                if (n) x.log("ddi: trace:   tss %u:%s", st, line);
            }
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

} // namespace d3dpt
