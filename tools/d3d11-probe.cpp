/*
 * d3d11-probe: Direct3D 11 on the WDDM driver's own Direct3D 11 user-mode
 * driver (track M25), checked against WARP in the same guest. For each
 * adapter the feature level D3D11CreateDevice gives with every level
 * asked for, and with 10_0 alone; then one scene drawn on our adapter at
 * 10_0 and on WARP, read back and compared pixel by pixel: clears, an
 * indexed textured quad (an immutable texture, a sampler that wraps), a
 * blended triangle in front of it (depth test, a texture written by
 * UpdateSubresource), a dynamic constant buffer mapped per draw, a copy
 * into a staging texture. Then the scene at 10_1 with shader model 4.1
 * (Gather) on ours and on WARP, and through Direct3D 10's API on ours
 * (d3d10.dll, and d3d10_1.dll at 10_1 and 10_0: the driver's 10.0 and 10.1
 * interfaces) against WARP's 10_0 frame. At feature level 11_0, when our
 * adapter offers it (D3D11FL11 lists the program while 11_0 is built), the
 * compute pipeline and unordered access against WARP's: a dispatch with a
 * structured SRV and UAV, atomics on a raw buffer, an append buffer and
 * CopyStructureCount, DispatchIndirect, a typed texture UAV with both
 * UAV clears, and a pixel shader's UAV at the output merger through Draw
 * and DrawInstancedIndirect; and tessellation, a quad patch through hull
 * and domain shaders with uneven factors, its frame against WARP's. With
 * "present" also a windowed swap chain's
 * frames, read off the screen (a third argument: how many milliseconds
 * each window stays up before that, 1000 by default). With "flags", for
 * each D3D11_CREATE_DEVICE flag the level our adapter gives and the
 * device made (VIDEO_SUPPORT needs the 11.1 interface's video functions,
 * which DWM's device asks for; track M25 step 4).
 *
 * Output into the file argv[1] (stdout without one), each frame as a PPM
 * beside it (d3d11-ours.ppm, d3d11-warp.ppm). The last line says
 * "d3d11-probe: PASS" or "FAIL".
 *
 * Built on the PC in MSYS2's MINGW64 shell, x64 MSVC:
 *   . scripts/msvc-env.sh && MSYS2_ARG_CONV_EXCL='*' cl -nologo -O1 -MT -EHsc tools/d3d11-probe.cpp
 * Run in the guest's logged-on session (a scheduled task with an
 * interactive principal; docs/testing.md "d3d11-probe").
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <d3d11.h>
#include <d3d10_1.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3d10.lib")
#pragma comment(lib, "d3d10_1.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

static FILE *out;
static DWORD hold_ms = 1000;      /* how long a presented window stays before it is read (argv[3]) */
static char dir[MAX_PATH];
static CRITICAL_SECTION out_lock;
#define P(...) do { EnterCriticalSection(&out_lock); fprintf(out, __VA_ARGS__); fflush(out); LeaveCriticalSection(&out_lock); } while (0)

/* The runtime's own words: OutputDebugString lines (DXGI's and Direct3D's
 * reasons for a failure), taken through the DBWIN buffer the way a
 * debug-output viewer takes them, by a thread of this process */
static DWORD WINAPI dbwin(void *)
{
    HANDLE buf_ready = CreateEventA(NULL, FALSE, FALSE, "DBWIN_BUFFER_READY");
    HANDLE data_ready = CreateEventA(NULL, FALSE, FALSE, "DBWIN_DATA_READY");
    HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4096, "DBWIN_BUFFER");
    const char *view = map ? (const char *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 4096) : NULL;

    if (!buf_ready || !data_ready || !view) {
        P("debug output: not listening (error %lu)\n", GetLastError());
        return 0;
    }
    for (;;) {
        SetEvent(buf_ready);
        if (WaitForSingleObject(data_ready, INFINITE) != WAIT_OBJECT_0) return 0;
        char line[4096];
        size_t n;

        strncpy(line, view + 4, sizeof(line) - 1);      /* after the writer's process id */
        line[sizeof(line) - 1] = 0;
        n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        P("debug output: %s\n", line);
    }
}

static const char *fl_name(D3D_FEATURE_LEVEL f)
{
    switch (f) {
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    default: return "?";
    }
}

static const char *src =
    "cbuffer c : register(b0) { float4x4 m; float4 tint; };\n"
    "Texture2D t : register(t0); SamplerState s : register(s0);\n"
    "struct v2p { float4 pos : SV_Position; float4 col : COLOR; float2 uv : TEXCOORD; };\n"
    "v2p vs(float3 pos : POSITION, float4 col : COLOR, float2 uv : TEXCOORD) {\n"
    "  v2p o; o.pos = mul(m, float4(pos, 1)); o.col = col; o.uv = uv; return o; }\n"
    "float4 ps(v2p i) : SV_Target { return t.Sample(s, i.uv) * i.col * tint; }\n"
    /* shader model 4.1 (feature level 10_1): Gather, the red of the four
     * texels a bilinear sample reads */
    "float4 ps41(v2p i) : SV_Target { float4 g = t.Gather(s, i.uv);\n"
    "  return lerp(t.Sample(s, i.uv), float4(g.wzy, 1), 0.25) * i.col * tint; }\n";

struct Vtx { float x, y, z; float r, g, b, a; float u, v; };

#define W 256
#define H 256

static ID3DBlob *compile(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(src, strlen(src), "probe", NULL, NULL, entry, target, 0, 0, &code, &err))) {
        P("  compile %s: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
        return NULL;
    }
    return code;
}

static void cb_set(ID3D11DeviceContext *c, ID3D11Buffer *cb, float sx, float sy, float tx, float ty, const float tint[4])
{
    D3D11_MAPPED_SUBRESOURCE m;
    float d[20] = { sx, 0, 0, tx,  0, sy, 0, ty,  0, 0, 1, 0,  0, 0, 0, 1 };
    memcpy(d + 16, tint, 16);
    if (SUCCEEDED(c->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, d, sizeof(d));
        c->Unmap(cb, 0);
    }
}

/* the scene into pixels (B8G8R8A8, W*H), drawn into `target` when given
 * (a swap chain's buffer) or a texture of its own; false when a step failed */
static bool scene(ID3D11Device *dev, ID3D11DeviceContext *c, unsigned *px, ID3D11Texture2D *target = NULL, bool sm41 = false)
{
    bool ok = false;
    ID3DBlob *vsb = compile("vs", sm41 ? "vs_4_1" : "vs_4_0"), *psb = compile(sm41 ? "ps41" : "ps", sm41 ? "ps_4_1" : "ps_4_0");
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11InputLayout *il = NULL;
    ID3D11Buffer *vb = NULL, *ib = NULL, *cb = NULL;
    ID3D11Texture2D *rt = NULL, *ds = NULL, *tex = NULL, *white = NULL, *st = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11ShaderResourceView *srv = NULL, *wsrv = NULL;
    ID3D11SamplerState *smp = NULL;
    ID3D11BlendState *blend = NULL;
    ID3D11DepthStencilState *dss = NULL;
    HRESULT hr;
#define CK(x) do { hr = (x); if (FAILED(hr)) { P("  %s: 0x%08lx\n", #x, (unsigned long)hr); goto done; } } while (0)
    if (!vsb || !psb) goto done;
    CK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &vs));
    CK(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &ps));
    {
        D3D11_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
        CK(dev->CreateInputLayout(el, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il));
    }
    {
        /* a quad (4 vertices, 6 indices) and a triangle in front of it */
        Vtx v[] = {
            { -0.8f, -0.8f, 0.5f, 1, 1, 1, 1, 0, 4 }, { -0.8f, 0.8f, 0.5f, 1, 1, 1, 1, 0, 0 },
            { 0.8f, 0.8f, 0.5f, 1, 1, 1, 1, 4, 0 }, { 0.8f, -0.8f, 0.5f, 1, 1, 1, 1, 4, 4 },
            { -0.6f, -0.5f, 0.25f, 1, 0, 0, 0.5f, 0, 0 }, { 0.0f, 0.7f, 0.25f, 0, 1, 0, 0.5f, 0, 0 },
            { 0.6f, -0.5f, 0.25f, 0, 0, 1, 0.5f, 0, 0 } };
        unsigned short idx[] = { 0, 1, 2, 0, 2, 3 };
        D3D11_BUFFER_DESC bd = { sizeof(v), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA sd = { v, 0, 0 };
        CK(dev->CreateBuffer(&bd, &sd, &vb));
        D3D11_BUFFER_DESC ibd = { sizeof(idx), D3D11_USAGE_DEFAULT, D3D11_BIND_INDEX_BUFFER, 0, 0, 0 };
        D3D11_SUBRESOURCE_DATA isd = { idx, 0, 0 };
        CK(dev->CreateBuffer(&ibd, &isd, &ib));
        D3D11_BUFFER_DESC cbd = { 80, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
        CK(dev->CreateBuffer(&cbd, NULL, &cb));
    }
    {
        /* a 64x64 checker, immutable; a 1x1 white written by UpdateSubresource */
        static unsigned checker[64 * 64];
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) checker[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xffffffffu : 0xff4080c0u;
        D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_IMMUTABLE,
                                    D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        D3D11_SUBRESOURCE_DATA sd = { checker, 64 * 4, 0 };
        CK(dev->CreateTexture2D(&td, &sd, &tex));
        CK(dev->CreateShaderResourceView(tex, NULL, &srv));
        td.Width = td.Height = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        CK(dev->CreateTexture2D(&td, NULL, &white));
        unsigned w1 = 0xffffffffu;
        c->UpdateSubresource(white, 0, NULL, &w1, 4, 0);
        CK(dev->CreateShaderResourceView(white, NULL, &wsrv));
    }
    {
        D3D11_TEXTURE2D_DESC td = { W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
        if (target) {
            rt = target;
            rt->AddRef();
        } else {
            CK(dev->CreateTexture2D(&td, NULL, &rt));
        }
        CK(dev->CreateRenderTargetView(rt, NULL, &rtv));
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        CK(dev->CreateTexture2D(&td, NULL, &ds));
        CK(dev->CreateDepthStencilView(ds, NULL, &dsv));
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.BindFlags = 0;
        td.Usage = D3D11_USAGE_STAGING;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        CK(dev->CreateTexture2D(&td, NULL, &st));
    }
    {
        D3D11_SAMPLER_DESC s = {};
        s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        s.MaxLOD = D3D11_FLOAT32_MAX;
        CK(dev->CreateSamplerState(&s, &smp));
        D3D11_BLEND_DESC b = {};
        b.RenderTarget[0].BlendEnable = TRUE;
        b.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        b.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        b.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        b.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        b.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        b.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        CK(dev->CreateBlendState(&b, &blend));
        D3D11_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D11_COMPARISON_LESS;
        CK(dev->CreateDepthStencilState(&d, &dss));
    }
    {
        const float clear[4] = { 0.1f, 0.2f, 0.3f, 1.0f }, one[4] = { 1, 1, 1, 1 }, warm[4] = { 1, 0.9f, 0.8f, 1 };
        D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
        UINT stride = sizeof(Vtx), off = 0;
        c->OMSetRenderTargets(1, &rtv, dsv);
        c->RSSetViewports(1, &vp);
        c->ClearRenderTargetView(rtv, clear);
        c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        c->IASetInputLayout(il);
        c->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        c->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->VSSetShader(vs, NULL, 0);
        c->PSSetShader(ps, NULL, 0);
        c->VSSetConstantBuffers(0, 1, &cb);
        c->PSSetConstantBuffers(0, 1, &cb);
        c->PSSetSamplers(0, 1, &smp);
        c->OMSetDepthStencilState(dss, 0);
        cb_set(c, cb, 1, 1, 0, 0, warm);
        c->PSSetShaderResources(0, 1, &srv);
        c->DrawIndexed(6, 0, 0);
        cb_set(c, cb, 1.1f, 1.1f, 0.05f, 0.0f, one);
        c->PSSetShaderResources(0, 1, &wsrv);
        c->OMSetBlendState(blend, NULL, 0xffffffff);
        c->Draw(3, 4);
        c->CopyResource(st, rt);
    }
    {
        D3D11_MAPPED_SUBRESOURCE m;
        CK(c->Map(st, 0, D3D11_MAP_READ, 0, &m));
        for (int y = 0; y < H; y++) memcpy(px + y * W, (const BYTE *)m.pData + y * m.RowPitch, W * 4);
        c->Unmap(st, 0);
    }
    ok = true;
done:
#define R(p) if (p) p->Release()
    R(vsb); R(psb); R(vs); R(ps); R(il); R(vb); R(ib); R(cb); R(rt); R(ds); R(tex); R(white); R(st);
    R(rtv); R(dsv); R(srv); R(wsrv); R(smp); R(blend); R(dss);
    return ok;
}

/* The same scene through Direct3D 10's API (d3d10.dll or d3d10_1.dll's
 * device), with 10.0's blend state, into R8G8B8A8 */
static bool scene10(ID3D10Device *dev, unsigned *px)
{
    bool ok = false;
    ID3DBlob *vsb = compile("vs", "vs_4_0"), *psb = compile("ps", "ps_4_0");
    ID3D10VertexShader *vs = NULL;
    ID3D10PixelShader *ps = NULL;
    ID3D10InputLayout *il = NULL;
    ID3D10Buffer *vb = NULL, *ib = NULL, *cb = NULL;
    ID3D10Texture2D *rt = NULL, *ds = NULL, *tex = NULL, *white = NULL, *st = NULL;
    ID3D10RenderTargetView *rtv = NULL;
    ID3D10DepthStencilView *dsv = NULL;
    ID3D10ShaderResourceView *srv = NULL, *wsrv = NULL;
    ID3D10SamplerState *smp = NULL;
    ID3D10BlendState *blend = NULL;
    ID3D10DepthStencilState *dss = NULL;
    HRESULT hr;
    if (!vsb || !psb) goto done;
    CK(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), &vs));
    CK(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), &ps));
    {
        D3D10_INPUT_ELEMENT_DESC el[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D10_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D10_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 28, D3D10_INPUT_PER_VERTEX_DATA, 0 } };
        CK(dev->CreateInputLayout(el, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &il));
    }
    {
        Vtx v[] = {
            { -0.8f, -0.8f, 0.5f, 1, 1, 1, 1, 0, 4 }, { -0.8f, 0.8f, 0.5f, 1, 1, 1, 1, 0, 0 },
            { 0.8f, 0.8f, 0.5f, 1, 1, 1, 1, 4, 0 }, { 0.8f, -0.8f, 0.5f, 1, 1, 1, 1, 4, 4 },
            { -0.6f, -0.5f, 0.25f, 1, 0, 0, 0.5f, 0, 0 }, { 0.0f, 0.7f, 0.25f, 0, 1, 0, 0.5f, 0, 0 },
            { 0.6f, -0.5f, 0.25f, 0, 0, 1, 0.5f, 0, 0 } };
        unsigned short idx[] = { 0, 1, 2, 0, 2, 3 };
        D3D10_BUFFER_DESC bd = { sizeof(v), D3D10_USAGE_IMMUTABLE, D3D10_BIND_VERTEX_BUFFER, 0, 0 };
        D3D10_SUBRESOURCE_DATA sd = { v, 0, 0 };
        CK(dev->CreateBuffer(&bd, &sd, &vb));
        D3D10_BUFFER_DESC ibd = { sizeof(idx), D3D10_USAGE_DEFAULT, D3D10_BIND_INDEX_BUFFER, 0, 0 };
        D3D10_SUBRESOURCE_DATA isd = { idx, 0, 0 };
        CK(dev->CreateBuffer(&ibd, &isd, &ib));
        D3D10_BUFFER_DESC cbd = { 80, D3D10_USAGE_DYNAMIC, D3D10_BIND_CONSTANT_BUFFER, D3D10_CPU_ACCESS_WRITE, 0 };
        CK(dev->CreateBuffer(&cbd, NULL, &cb));
    }
    {
        static unsigned checker[64 * 64];
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) checker[y * 64 + x] = ((x / 8 + y / 8) & 1) ? 0xffffffffu : 0xff4080c0u;
        D3D10_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D10_USAGE_IMMUTABLE,
                                    D3D10_BIND_SHADER_RESOURCE, 0, 0 };
        D3D10_SUBRESOURCE_DATA sd = { checker, 64 * 4, 0 };
        CK(dev->CreateTexture2D(&td, &sd, &tex));
        CK(dev->CreateShaderResourceView(tex, NULL, &srv));
        td.Width = td.Height = 1;
        td.Usage = D3D10_USAGE_DEFAULT;
        CK(dev->CreateTexture2D(&td, NULL, &white));
        unsigned w1 = 0xffffffffu;
        dev->UpdateSubresource(white, 0, NULL, &w1, 4, 0);
        CK(dev->CreateShaderResourceView(white, NULL, &wsrv));
    }
    {
        D3D10_TEXTURE2D_DESC td = { W, H, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET, 0, 0 };
        CK(dev->CreateTexture2D(&td, NULL, &rt));
        CK(dev->CreateRenderTargetView(rt, NULL, &rtv));
        td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        td.BindFlags = D3D10_BIND_DEPTH_STENCIL;
        CK(dev->CreateTexture2D(&td, NULL, &ds));
        CK(dev->CreateDepthStencilView(ds, NULL, &dsv));
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.BindFlags = 0;
        td.Usage = D3D10_USAGE_STAGING;
        td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
        CK(dev->CreateTexture2D(&td, NULL, &st));
    }
    {
        D3D10_SAMPLER_DESC s = {};
        s.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
        s.AddressU = s.AddressV = s.AddressW = D3D10_TEXTURE_ADDRESS_WRAP;
        s.MaxLOD = D3D10_FLOAT32_MAX;
        CK(dev->CreateSamplerState(&s, &smp));
        D3D10_BLEND_DESC b = {};
        b.BlendEnable[0] = TRUE;
        b.SrcBlend = D3D10_BLEND_SRC_ALPHA;
        b.DestBlend = D3D10_BLEND_INV_SRC_ALPHA;
        b.BlendOp = D3D10_BLEND_OP_ADD;
        b.SrcBlendAlpha = D3D10_BLEND_ONE;
        b.DestBlendAlpha = D3D10_BLEND_ZERO;
        b.BlendOpAlpha = D3D10_BLEND_OP_ADD;
        for (int i = 0; i < 8; i++) b.RenderTargetWriteMask[i] = D3D10_COLOR_WRITE_ENABLE_ALL;
        CK(dev->CreateBlendState(&b, &blend));
        D3D10_DEPTH_STENCIL_DESC d = {};
        d.DepthEnable = TRUE;
        d.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
        d.DepthFunc = D3D10_COMPARISON_LESS;
        CK(dev->CreateDepthStencilState(&d, &dss));
    }
    {
        const float clear[4] = { 0.1f, 0.2f, 0.3f, 1.0f }, one[4] = { 1, 1, 1, 1 }, warm[4] = { 1, 0.9f, 0.8f, 1 };
        D3D10_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
        UINT stride = sizeof(Vtx), off = 0;
        float m[20] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
        void *p;
        dev->OMSetRenderTargets(1, &rtv, dsv);
        dev->RSSetViewports(1, &vp);
        dev->ClearRenderTargetView(rtv, clear);
        dev->ClearDepthStencilView(dsv, D3D10_CLEAR_DEPTH | D3D10_CLEAR_STENCIL, 1.0f, 0);
        dev->IASetInputLayout(il);
        dev->IASetVertexBuffers(0, 1, &vb, &stride, &off);
        dev->IASetIndexBuffer(ib, DXGI_FORMAT_R16_UINT, 0);
        dev->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        dev->VSSetShader(vs);
        dev->PSSetShader(ps);
        dev->VSSetConstantBuffers(0, 1, &cb);
        dev->PSSetConstantBuffers(0, 1, &cb);
        dev->PSSetSamplers(0, 1, &smp);
        dev->OMSetDepthStencilState(dss, 0);
        memcpy(m + 16, warm, 16);
        CK(cb->Map(D3D10_MAP_WRITE_DISCARD, 0, &p));
        memcpy(p, m, sizeof(m));
        cb->Unmap();
        dev->PSSetShaderResources(0, 1, &srv);
        dev->DrawIndexed(6, 0, 0);
        float m2[20] = { 1.1f, 0, 0, 0.05f,  0, 1.1f, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
        memcpy(m2 + 16, one, 16);
        CK(cb->Map(D3D10_MAP_WRITE_DISCARD, 0, &p));
        memcpy(p, m2, sizeof(m2));
        cb->Unmap();
        dev->PSSetShaderResources(0, 1, &wsrv);
        float factor[4] = { 0, 0, 0, 0 };
        dev->OMSetBlendState(blend, factor, 0xffffffff);
        dev->Draw(3, 4);
        dev->CopyResource(st, rt);
    }
    {
        D3D10_MAPPED_TEXTURE2D m;
        CK(st->Map(0, D3D10_MAP_READ, 0, &m));
        for (int y = 0; y < H; y++) memcpy(px + y * W, (const BYTE *)m.pData + y * m.RowPitch, W * 4);
        for (int i = 0; i < W * H; i++)     /* R8G8B8A8 (d3d10.dll's 10.0 has no B8G8R8A8 target) to B8G8R8A8 */
            px[i] = (px[i] & 0xff00ff00u) | (px[i] >> 16 & 0xffu) | (px[i] & 0xffu) << 16;
        st->Unmap(0);
    }
    ok = true;
done:
    R(vsb); R(psb); R(vs); R(ps); R(il); R(vb); R(ib); R(cb); R(rt); R(ds); R(tex); R(white); R(st);
    R(rtv); R(dsv); R(srv); R(wsrv); R(smp); R(blend); R(dss);
    return ok;
}

/* pixel differences beyond 8 in a channel, and the largest */
static unsigned diff(const unsigned *a, const unsigned *b, unsigned *maxd)
{
    unsigned off = 0;

    *maxd = 0;
    for (int i = 0; i < W * H; i++) {
        unsigned dm = 0;
        for (int k = 0; k < 24; k += 8) {
            int dd = (int)((a[i] >> k) & 0xff) - (int)((b[i] >> k) & 0xff);
            if (dd < 0) dd = -dd;
            if ((unsigned)dd > dm) dm = dd;
        }
        if (dm > *maxd) *maxd = dm;
        if (dm > 8) off++;
    }
    return off;
}

/* The scene presented: a window with a swap chain on our adapter (a
 * blt-model DISCARD one, then a flip-model FLIP_SEQUENTIAL one), thirty
 * presents, then the window's client area read off the screen, which is
 * what DWM composed */
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) { return DefWindowProcA(h, m, w, l); }

static bool presented(IDXGIAdapter1 *adapter, DXGI_SWAP_EFFECT effect, unsigned *px)
{
    static const D3D_FEATURE_LEVEL ten[] = { D3D_FEATURE_LEVEL_10_0 };
    WNDCLASSA wc = {};
    RECT r = { 0, 0, W, H };
    HWND wnd;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *c = NULL;
    IDXGIFactory1 *fac = NULL;
    IDXGISwapChain *sc = NULL;
    ID3D11Texture2D *bb = NULL;
    static unsigned drawn[W * H];
    bool ok = false;
    HRESULT hr;

    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d11probe";
    RegisterClassA(&wc);
    AdjustWindowRect(&r, WS_POPUP, FALSE);
    wnd = CreateWindowExA(WS_EX_TOPMOST, "d3d11probe", "d3d11-probe", WS_POPUP | WS_VISIBLE, 100, 100, W, H, NULL, NULL,
                          wc.hInstance, NULL);
    if (!wnd) { P("  no window\n"); return false; }
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, ten, 1, D3D11_SDK_VERSION, &dev, NULL, &c);
    if (FAILED(hr)) { P("  device 0x%08lx\n", (unsigned long)hr); goto done; }
    adapter->GetParent(__uuidof(IDXGIFactory1), (void **)&fac);
    {
        DXGI_SWAP_CHAIN_DESC d = {};
        d.BufferDesc.Width = W;
        d.BufferDesc.Height = H;
        d.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = effect == DXGI_SWAP_EFFECT_DISCARD ? 1 : 2;
        d.OutputWindow = wnd;
        d.Windowed = TRUE;
        d.SwapEffect = effect;
        hr = fac->CreateSwapChain(dev, &d, &sc);
        if (FAILED(hr)) { P("  swap chain 0x%08lx\n", (unsigned long)hr); goto done; }
    }
    for (int f = 0; f < 30; f++) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&m);
        if (bb) { bb->Release(); bb = NULL; }
        sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb);
        if (!scene(dev, c, drawn, bb)) goto done;
        hr = sc->Present(1, 0);
        if (FAILED(hr)) { P("  present 0x%08lx\n", (unsigned long)hr); goto done; }
    }
    Sleep(hold_ms);
    {
        HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
        BITMAPINFO bi = {};
        void *bits = NULL;
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        BitBlt(mem, 0, 0, W, H, screen, 100, 100, SRCCOPY);
        memcpy(px, bits, W * H * 4);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(NULL, screen);
        for (int i = 0; i < W * H; i++) px[i] |= 0xff000000u;
    }
    ok = true;
done:
    if (bb) bb->Release();
    if (sc) sc->Release();
    if (fac) fac->Release();
    if (c) c->Release();
    if (dev) dev->Release();
    DestroyWindow(wnd);
    return ok;
}

/* ------------------------------------------------ feature level 11_0's compute
 * (track M25, "Feature level 11_0" step 1): the same work on ours and on
 * WARP, each answer compared. */
static const char cs_src[] =
    "cbuffer C : register(b0) { uint base; uint3 pad; };\n"
    "StructuredBuffer<float> inb : register(t0);\n"
    "RWStructuredBuffer<uint> outb : register(u0);\n"
    "RWByteAddressBuffer counter : register(u1);\n"
    "RWTexture2D<float4> img : register(u2);\n"
    "AppendStructuredBuffer<uint> app : register(u3);\n"
    "[numthreads(64, 1, 1)] void cs_main(uint3 id : SV_DispatchThreadID) {\n"
    "  uint i = id.x, old;\n"
    "  outb[i] = i * i + base + (uint)(inb[i] * 2.0);\n"
    "  counter.InterlockedAdd(0, 1, old);\n"
    "  if ((i & 3) == 0) app.Append(i);\n"
    "}\n"
    "[numthreads(8, 8, 1)] void cs_img(uint3 id : SV_DispatchThreadID) {\n"
    "  img[id.xy] = float4(id.x / 63.0, id.y / 63.0, (id.x ^ id.y) / 63.0, 1);\n"
    "}\n"
    "RWByteAddressBuffer pcount : register(u1);\n"
    "float4 vs_tri(uint v : SV_VertexID) : SV_Position {\n"
    "  float2 uv = float2((v << 1) & 2, v & 2); return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); }\n"
    "float4 ps_uav(float4 p : SV_Position) : SV_Target { uint o; pcount.InterlockedAdd(0, 1, o); return float4(1, 0, 0, 1); }\n";

struct CsOut {
    unsigned outb[256];
    unsigned counter, appended, append_sum, pixels;
    unsigned img[64 * 64];
};

static ID3DBlob *compile_cs(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(cs_src, strlen(cs_src), "probe-cs", NULL, NULL, entry, target, 0, 0, &code, &err))) {
        P("  compile %s: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
        return NULL;
    }
    return code;
}

/* a GPU resource's bytes, through a staging copy */
static bool read_back(ID3D11Device *dev, ID3D11DeviceContext *c, ID3D11Resource *src, void *dst, UINT bytes, UINT row = 0, UINT rows = 1)
{
    ID3D11Resource *stg = NULL;
    ID3D11Buffer *sb = NULL;
    ID3D11Texture2D *st = NULL;
    D3D11_RESOURCE_DIMENSION dim;
    src->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        ((ID3D11Buffer *)src)->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; d.StructureByteStride = 0;
        if (FAILED(dev->CreateBuffer(&d, NULL, &sb))) return false;
        stg = sb;
    } else {
        D3D11_TEXTURE2D_DESC d;
        ((ID3D11Texture2D *)src)->GetDesc(&d);
        d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
        if (FAILED(dev->CreateTexture2D(&d, NULL, &st))) return false;
        stg = st;
    }
    c->CopyResource(stg, src);
    D3D11_MAPPED_SUBRESOURCE m;
    bool ok = SUCCEEDED(c->Map(stg, 0, D3D11_MAP_READ, 0, &m));
    if (ok) {
        for (UINT y = 0; y < rows; y++)
            memcpy((BYTE *)dst + (size_t)y * (row ? row : bytes), (const BYTE *)m.pData + (size_t)y * m.RowPitch, row ? row : bytes);
        c->Unmap(stg, 0);
    }
    stg->Release();
    return ok;
}

static bool compute(ID3D11Device *dev, ID3D11DeviceContext *c, CsOut *o)
{
    memset(o, 0, sizeof(*o));
    ID3DBlob *bm = compile_cs("cs_main", "cs_5_0"), *bi = compile_cs("cs_img", "cs_5_0"), *bv = compile_cs("vs_tri", "vs_5_0"),
             *bp = compile_cs("ps_uav", "ps_5_0");
    if (!bm || !bi || !bv || !bp) return false;
    ID3D11ComputeShader *csm = NULL, *csi = NULL;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr = dev->CreateComputeShader(bm->GetBufferPointer(), bm->GetBufferSize(), NULL, &csm);
    if (SUCCEEDED(hr)) hr = dev->CreateComputeShader(bi->GetBufferPointer(), bi->GetBufferSize(), NULL, &csi);
    if (SUCCEEDED(hr)) hr = dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), NULL, &vs);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), NULL, &ps);
    if (FAILED(hr)) { P("  compute: shaders 0x%08lx\n", (unsigned long)hr); return false; }

    /* the input (an SRV of a structured buffer), the constants */
    float in[256];
    for (int i = 0; i < 256; i++) in[i] = (float)(i % 17);
    D3D11_BUFFER_DESC bd = { sizeof(in), D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4 };
    D3D11_SUBRESOURCE_DATA sd = { in, 0, 0 };
    ID3D11Buffer *inb = NULL, *cb = NULL, *outb = NULL, *cnt = NULL, *app = NULL, *args = NULL, *count_dst = NULL, *dargs = NULL,
                 *cnt_save = NULL;
    hr = dev->CreateBuffer(&bd, &sd, &inb);
    unsigned cbv[4] = { 1000, 0, 0, 0 };
    D3D11_BUFFER_DESC cd = { 16, D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA cs = { cbv, 0, 0 };
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&cd, &cs, &cb);
    /* the outputs: a structured buffer, a raw counter, an append buffer */
    D3D11_BUFFER_DESC od = { 256 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4 };
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&od, NULL, &outb);
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&od, NULL, &app);
    D3D11_BUFFER_DESC rd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0 };
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&rd, NULL, &cnt);
    D3D11_BUFFER_DESC kd = { 16, D3D11_USAGE_DEFAULT, 0, 0, 0, 0 };
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&kd, NULL, &count_dst);
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&kd, NULL, &cnt_save);
    /* DispatchIndirect's and DrawInstancedIndirect's arguments */
    unsigned av[4] = { 4, 1, 1, 0 }, dv[4] = { 3, 2, 0, 0 };
    D3D11_BUFFER_DESC ad = { 16, D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0 };
    D3D11_SUBRESOURCE_DATA as = { av, 0, 0 }, ds = { dv, 0, 0 };
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&ad, &as, &args);
    if (SUCCEEDED(hr)) hr = dev->CreateBuffer(&ad, &ds, &dargs);
    /* a 64x64 RGBA8 image written by a compute shader, and a target */
    D3D11_TEXTURE2D_DESC td = { 64, 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    ID3D11Texture2D *img = NULL, *rt = NULL;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&td, NULL, &img);
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&td, NULL, &rt);
    if (FAILED(hr)) { P("  compute: resources 0x%08lx\n", (unsigned long)hr); return false; }

    ID3D11ShaderResourceView *srv = NULL;
    ID3D11UnorderedAccessView *u_out = NULL, *u_cnt = NULL, *u_img = NULL, *u_app = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    hr = dev->CreateShaderResourceView(inb, NULL, &srv);
    if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(outb, NULL, &u_out);
    D3D11_UNORDERED_ACCESS_VIEW_DESC raw = {};
    raw.Format = DXGI_FORMAT_R32_TYPELESS; raw.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    raw.Buffer.NumElements = 4; raw.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
    if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(cnt, &raw, &u_cnt);
    if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(img, NULL, &u_img);
    D3D11_UNORDERED_ACCESS_VIEW_DESC ap = {};
    ap.Format = DXGI_FORMAT_UNKNOWN; ap.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ap.Buffer.NumElements = 256; ap.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
    if (SUCCEEDED(hr)) hr = dev->CreateUnorderedAccessView(app, &ap, &u_app);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(rt, NULL, &rtv);
    if (FAILED(hr)) { P("  compute: views 0x%08lx\n", (unsigned long)hr); return false; }

    /* ClearUnorderedAccessViewUint on the raw counter, Float on the image
     * (the compute shader writes its top-left 32x32 only) */
    const UINT zero[4] = { 0, 0, 0, 0 };
    const float grey[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
    c->ClearUnorderedAccessViewUint(u_cnt, zero);
    c->ClearUnorderedAccessViewFloat(u_img, grey);
    /* Dispatch, then DispatchIndirect (4 x 64 threads each); the append
     * buffer's count starts at 0 and is kept for the second */
    ID3D11UnorderedAccessView *uv[4] = { u_out, u_cnt, NULL, u_app };
    UINT init[4] = { 0, 0, 0, 0 };
    c->CSSetShader(csm, NULL, 0);
    c->CSSetConstantBuffers(0, 1, &cb);
    c->CSSetShaderResources(0, 1, &srv);
    c->CSSetUnorderedAccessViews(0, 4, uv, init);
    c->Dispatch(4, 1, 1);
    UINT keep[4] = { (UINT)-1, (UINT)-1, (UINT)-1, (UINT)-1 };
    c->CSSetUnorderedAccessViews(0, 4, uv, keep);
    c->DispatchIndirect(args, 0);
    c->CopyStructureCount(count_dst, 0, u_app);
    c->CSSetShader(csi, NULL, 0);
    ID3D11UnorderedAccessView *none[4] = {};
    c->CSSetUnorderedAccessViews(0, 4, none, NULL);
    c->CSSetUnorderedAccessViews(2, 1, &u_img, NULL);
    c->Dispatch(4, 4, 1);
    c->CSSetUnorderedAccessViews(0, 4, none, NULL);
    c->CSSetShader(NULL, NULL, 0);
    c->CopyResource(cnt_save, cnt);             /* the threads' count, before the pixels' */

    /* a pixel shader's UAV at the output merger: a count of the pixels
     * shaded, one triangle over 64x64, then DrawInstancedIndirect's two
     * instances of it */
    c->ClearUnorderedAccessViewUint(u_cnt, zero);
    D3D11_VIEWPORT vp = { 0, 0, 64, 64, 0, 1 };
    c->RSSetViewports(1, &vp);
    c->IASetInputLayout(NULL);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs, NULL, 0);
    c->PSSetShader(ps, NULL, 0);
    c->OMSetRenderTargetsAndUnorderedAccessViews(1, &rtv, NULL, 1, 1, &u_cnt, NULL);
    c->Draw(3, 0);
    c->DrawInstancedIndirect(dargs, 0);
    c->OMSetRenderTargetsAndUnorderedAccessViews(0, NULL, NULL, 0, 0, NULL, NULL);

    bool ok = read_back(dev, c, outb, o->outb, sizeof(o->outb));
    unsigned k[4] = {}, a[256] = {};
    ok = ok && read_back(dev, c, count_dst, k, 16);
    o->appended = k[0];
    ok = ok && read_back(dev, c, app, a, sizeof(a));
    for (unsigned i = 0; i < o->appended && i < 256; i++) o->append_sum += a[i];
    ok = ok && read_back(dev, c, cnt, k, 16);
    o->pixels = k[0];
    ok = ok && read_back(dev, c, img, o->img, 64 * 4, 64 * 4, 64);
    ok = ok && read_back(dev, c, cnt_save, k, 16);
    o->counter = k[0];
    R(cnt_save); R(srv); R(u_out); R(u_cnt); R(u_img); R(u_app); R(rtv); R(img); R(rt);
    R(inb); R(cb); R(outb); R(cnt); R(app); R(args); R(count_dst); R(dargs);
    R(csm); R(csi); R(vs); R(ps); R(bm); R(bi); R(bv); R(bp);
    return ok;
}

/* ------------------------------------------- feature level 11_0's tessellation
 * (step 2): one quad patch, its edges and inside tessellated unevenly
 * (integer partitioning), displaced and coloured by its domain location in
 * the domain shader; the colour's sine follows the triangles the
 * tessellator made, so another pattern shows as other pixels. Patch
 * constants beside the factors (a TINT) cross from hull to domain. */
static const char tess_src[] =
    "struct VSO { float3 p : POS; };\n"
    "VSO vs_cp(uint id : SV_VertexID) { VSO o; float2 c = float2(id & 1, id >> 1); o.p = float3(c * 1.6 - 0.8, 0); return o; }\n"
    "struct PC { float e[4] : SV_TessFactor; float i[2] : SV_InsideTessFactor; float tint : TINT; };\n"
    "PC hs_pc(InputPatch<VSO, 4> ip, uint pid : SV_PrimitiveID) {\n"
    "  PC o; o.e[0] = 4; o.e[1] = 6; o.e[2] = 8; o.e[3] = 5; o.i[0] = 7; o.i[1] = 3; o.tint = 0.5; return o; }\n"
    "[domain(\"quad\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] [outputcontrolpoints(4)]\n"
    "[patchconstantfunc(\"hs_pc\")] VSO hs_main(InputPatch<VSO, 4> ip, uint i : SV_OutputControlPointID) { return ip[i]; }\n"
    "struct DSO { float4 pos : SV_Position; float3 col : COL; };\n"
    "[domain(\"quad\")] DSO ds_main(PC pc, float2 uv : SV_DomainLocation, const OutputPatch<VSO, 4> q) {\n"
    "  float3 a = lerp(q[0].p, q[1].p, uv.x), b = lerp(q[2].p, q[3].p, uv.x), p = lerp(a, b, uv.y);\n"
    "  p.z = 0.5; p.xy += 0.05 * sin(uv.yx * 12.0);\n"
    "  DSO o; o.pos = float4(p, 1); o.col = float3(uv.x, sin(uv.x * 20.0 + uv.y * 9.0) * 0.5 + 0.5, pc.tint); return o; }\n"
    "float4 ps_tess(DSO i) : SV_Target { return float4(i.col, 1); }\n";

static ID3DBlob *compile_tess(const char *entry, const char *target)
{
    ID3DBlob *code = NULL, *err = NULL;
    if (FAILED(D3DCompile(tess_src, strlen(tess_src), "probe-tess", NULL, NULL, entry, target, 0, 0, &code, &err))) {
        P("  compile %s: %s\n", entry, err ? (const char *)err->GetBufferPointer() : "?");
        return NULL;
    }
    return code;
}

/* the frame into px (BGRA as the other scenes read it); the pipeline
 * statistics' hull and domain invocations into *hs, *ds */
static bool tess_scene(ID3D11Device *dev, ID3D11DeviceContext *c, unsigned *px, UINT64 *hs, UINT64 *ds)
{
    ID3DBlob *bv = compile_tess("vs_cp", "vs_5_0"), *bh = compile_tess("hs_main", "hs_5_0"), *bd = compile_tess("ds_main", "ds_5_0"),
             *bp = compile_tess("ps_tess", "ps_5_0");
    if (!bv || !bh || !bd || !bp) return false;
    ID3D11VertexShader *vs = NULL;
    ID3D11HullShader *h = NULL;
    ID3D11DomainShader *d = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr = dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), NULL, &vs);
    if (SUCCEEDED(hr)) hr = dev->CreateHullShader(bh->GetBufferPointer(), bh->GetBufferSize(), NULL, &h);
    if (SUCCEEDED(hr)) hr = dev->CreateDomainShader(bd->GetBufferPointer(), bd->GetBufferSize(), NULL, &d);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), NULL, &ps);
    if (FAILED(hr)) { P("  tessellation: shaders 0x%08lx\n", (unsigned long)hr); return false; }
    D3D11_TEXTURE2D_DESC td = { W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *rt = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Query *q = NULL;
    D3D11_QUERY_DESC qd = { D3D11_QUERY_PIPELINE_STATISTICS, 0 };
    hr = dev->CreateTexture2D(&td, NULL, &rt);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(rt, NULL, &rtv);
    if (SUCCEEDED(hr)) hr = dev->CreateQuery(&qd, &q);
    /* no culling: the domain's v runs up the screen, which turns the
     * tessellator's clockwise triangles around (with the default state
     * every one was culled, on ours and WARP alike: a clear frame each) */
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    ID3D11RasterizerState *rs = NULL;
    if (SUCCEEDED(hr)) hr = dev->CreateRasterizerState(&rd, &rs);
    if (FAILED(hr)) { P("  tessellation: target 0x%08lx\n", (unsigned long)hr); return false; }
    c->RSSetState(rs);
    const float clear[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
    c->ClearRenderTargetView(rtv, clear);
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    c->RSSetViewports(1, &vp);
    c->OMSetRenderTargets(1, &rtv, NULL);
    c->IASetInputLayout(NULL);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST);
    c->VSSetShader(vs, NULL, 0);
    c->HSSetShader(h, NULL, 0);
    c->DSSetShader(d, NULL, 0);
    c->PSSetShader(ps, NULL, 0);
    c->Begin(q);
    c->Draw(4, 0);
    c->End(q);
    c->HSSetShader(NULL, NULL, 0);
    c->DSSetShader(NULL, NULL, 0);
    c->RSSetState(NULL);
    D3D11_QUERY_DATA_PIPELINE_STATISTICS st = {};
    for (int i = 0; i < 2000 && c->GetData(q, &st, sizeof(st), 0) == S_FALSE; i++) Sleep(1);
    *hs = st.HSInvocations;
    *ds = st.DSInvocations;
    bool ok = read_back(dev, c, rt, px, W * 4, W * 4, H);
    R(rs); R(q); R(rtv); R(rt); R(vs); R(h); R(d); R(ps); R(bv); R(bh); R(bd); R(bp);
    return ok;
}

/* -------------------------------------------------------- multisampling
 * (2026-11-27): two slanted triangles into a 4x target with a 4x depth
 * buffer, the second half behind the first, resolved and read back. The
 * edges' coverage is the samples' (the standard 4x pattern on every
 * implementation), so a missing sample count or resolve shows. */
static const char msaa_src[] =
    "struct V { float4 p : SV_Position; float3 c : COL; };\n"
    "V vs_ms(uint id : SV_VertexID) {\n"
    "  static const float2 P[6] = { float2(-0.9, -0.7), float2(0.1, 0.9), float2(0.8, -0.85),\n"
    "                               float2(-0.6, 0.8), float2(0.95, 0.3), float2(-0.2, -0.95) };\n"
    "  V o; o.p = float4(P[id], id < 3 ? 0.3 : 0.6, 1); o.c = id < 3 ? float3(1, 0.5, 0.1) : float3(0.1, 0.6, 1); return o; }\n"
    "float4 ps_ms(V i) : SV_Target { return float4(i.c, 1); }\n";

static bool msaa_scene(ID3D11Device *dev, ID3D11DeviceContext *c, unsigned *px)
{
    ID3DBlob *bv = NULL, *bp = NULL, *err = NULL;
    if (FAILED(D3DCompile(msaa_src, strlen(msaa_src), "probe-ms", NULL, NULL, "vs_ms", "vs_4_0", 0, 0, &bv, &err)) ||
        FAILED(D3DCompile(msaa_src, strlen(msaa_src), "probe-ms", NULL, NULL, "ps_ms", "ps_4_0", 0, 0, &bp, &err))) {
        P("  msaa compile: %s\n", err ? (const char *)err->GetBufferPointer() : "?");
        return false;
    }
    UINT q4 = 0, qd = 0;
    dev->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, 4, &q4);
    dev->CheckMultisampleQualityLevels(DXGI_FORMAT_D24_UNORM_S8_UINT, 4, &qd);
    if (!q4 || !qd) { P("  msaa: 4x not offered (colour %u levels, depth %u)\n", q4, qd); return false; }
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    HRESULT hr = dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), NULL, &vs);
    if (SUCCEEDED(hr)) hr = dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), NULL, &ps);
    D3D11_TEXTURE2D_DESC td = { W, H, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 4, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *ms = NULL, *one = NULL, *dt = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11DepthStencilState *dss = NULL;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&td, NULL, &ms);
    if (SUCCEEDED(hr)) hr = dev->CreateRenderTargetView(ms, NULL, &rtv);
    td.SampleDesc.Count = 1;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&td, NULL, &one);
    td.SampleDesc.Count = 4;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (SUCCEEDED(hr)) hr = dev->CreateTexture2D(&td, NULL, &dt);
    if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilView(dt, NULL, &dsv);
    D3D11_DEPTH_STENCIL_DESC dd = {};
    dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS;
    if (SUCCEEDED(hr)) hr = dev->CreateDepthStencilState(&dd, &dss);
    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE; rd.MultisampleEnable = TRUE;
    ID3D11RasterizerState *rs = NULL;
    if (SUCCEEDED(hr)) hr = dev->CreateRasterizerState(&rd, &rs);
    if (FAILED(hr)) { P("  msaa: objects 0x%08lx\n", (unsigned long)hr); return false; }
    const float clear[4] = { 0.1f, 0.1f, 0.15f, 1.0f };
    c->ClearRenderTargetView(rtv, clear);
    c->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)W, (float)H, 0, 1 };
    c->RSSetViewports(1, &vp);
    c->RSSetState(rs);
    c->OMSetRenderTargets(1, &rtv, dsv);
    c->OMSetDepthStencilState(dss, 0);
    c->IASetInputLayout(NULL);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs, NULL, 0);
    c->PSSetShader(ps, NULL, 0);
    c->Draw(6, 0);
    c->OMSetRenderTargets(0, NULL, NULL);
    c->OMSetDepthStencilState(NULL, 0);
    c->RSSetState(NULL);
    c->ResolveSubresource(one, 0, ms, 0, DXGI_FORMAT_B8G8R8A8_UNORM);
    bool ok = read_back(dev, c, one, px, W * 4, W * 4, H);
    R(rs); R(dss); R(dsv); R(dt); R(rtv); R(one); R(ms); R(vs); R(ps); R(bv); R(bp);
    return ok;
}

static void ppm(const char *name, const unsigned *px)
{
    char path[MAX_PATH];
    FILE *f;
    snprintf(path, sizeof(path), "%s%s", dir, name);
    f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        unsigned char rgb[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i] };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    static unsigned ours[W * H], warp[W * H];
    IDXGIFactory1 *fac = NULL;
    IDXGIAdapter1 *a = NULL, *mine = NULL;
    bool pass = true;

    /* the screen is read at the window's coordinates: a process that is
     * not DPI-aware gets its window scaled (175% on a 1080p desktop) */
    SetProcessDPIAware();
    InitializeCriticalSection(&out_lock);
    out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out) out = stdout;
    CloseHandle(CreateThread(NULL, 0, dbwin, NULL, 0, NULL));
    Sleep(100);
    if (argc > 1) {
        strncpy(dir, argv[1], sizeof(dir) - 1);
        char *s = strrchr(dir, '\\');
        if (s) s[1] = 0; else dir[0] = 0;
    }
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) {
        P("no DXGI factory\nd3d11-probe: FAIL\n");
        return 1;
    }
    for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        P("adapter %u: %ls (%04x:%04x)\n", i, d.Description, d.VendorId, d.DeviceId);
        static const D3D_FEATURE_LEVEL all[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                 D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1 };
        static const D3D_FEATURE_LEVEL ten[] = { D3D_FEATURE_LEVEL_10_0 };
        D3D_FEATURE_LEVEL got;
        HRESULT hr = D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, all, 7, D3D11_SDK_VERSION, NULL, &got, NULL);
        P("  every level: 0x%08lx, %s\n", (unsigned long)hr, SUCCEEDED(hr) ? fl_name(got) : "-");
        hr = D3D11CreateDevice(a, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, ten, 1, D3D11_SDK_VERSION, NULL, &got, NULL);
        P("  10_0 alone: 0x%08lx\n", (unsigned long)hr);
        if (d.VendorId == 0x1234 && !mine) {
            mine = a;
            mine->AddRef();
        }
        a->Release();
    }
    if (!mine) {
        P("no d3dpt-vga adapter (1234:xxxx)\nd3d11-probe: FAIL\n");
        return 1;
    }
    if (argc > 2 && !strcmp(argv[2], "flags")) {
        /* the level each creation flag gets on ours with every level asked
         * for: 10_0 is this driver, 9_x the runtime's d3d10level9 */
        static const D3D_FEATURE_LEVEL all[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                                 D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1 };
        static const struct { UINT flag; const char *name; } fl[] = {
            { 0, "none" }, { D3D11_CREATE_DEVICE_SINGLETHREADED, "SINGLETHREADED" }, { D3D11_CREATE_DEVICE_BGRA_SUPPORT, "BGRA_SUPPORT" },
            { D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS, "PREVENT_INTERNAL_THREADING" },
            { D3D11_CREATE_DEVICE_DEBUGGABLE, "DEBUGGABLE" }, { D3D11_CREATE_DEVICE_DISABLE_GPU_TIMEOUT, "DISABLE_GPU_TIMEOUT" },
            { D3D11_CREATE_DEVICE_VIDEO_SUPPORT, "VIDEO_SUPPORT" }, { 0x20000, "0x20000" }, { 0x40000, "0x40000" }, { 0x80000, "0x80000" },
        };
        for (UINT i = 0; i < ARRAYSIZE(fl); i++) {
            D3D_FEATURE_LEVEL got;
            ID3D11Device *dev = NULL;
            HRESULT hr = D3D11CreateDevice(mine, D3D_DRIVER_TYPE_UNKNOWN, NULL, fl[i].flag, all, 7, D3D11_SDK_VERSION, NULL, &got, NULL);
            P("  flag %s: 0x%08lx, %s", fl[i].name, (unsigned long)hr, SUCCEEDED(hr) ? fl_name(got) : "-");
            /* and made: asking the level alone makes no device */
            hr = D3D11CreateDevice(mine, D3D_DRIVER_TYPE_UNKNOWN, NULL, fl[i].flag, all, 7, D3D11_SDK_VERSION, &dev, &got, NULL);
            P("; made: 0x%08lx, %s\n", (unsigned long)hr, SUCCEEDED(hr) ? fl_name(got) : "-");
            if (dev) dev->Release();
        }
        return 0;
    }
    {
        static const D3D_FEATURE_LEVEL ten[] = { D3D_FEATURE_LEVEL_10_0 };
        ID3D11Device *dev = NULL, *wdev = NULL;
        ID3D11DeviceContext *c = NULL, *wc = NULL;
        HRESULT hr = D3D11CreateDevice(mine, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, ten, 1, D3D11_SDK_VERSION, &dev, NULL, &c);
        P("scene on ours (10_0): device 0x%08lx\n", (unsigned long)hr);
        bool a_ok = SUCCEEDED(hr) && scene(dev, c, ours);
        hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, ten, 1, D3D11_SDK_VERSION, &wdev, NULL, &wc);
        P("scene on WARP (10_0): device 0x%08lx\n", (unsigned long)hr);
        bool b_ok = SUCCEEDED(hr) && scene(wdev, wc, warp);
        if (!a_ok || !b_ok) {
            P("scene: ours %s, WARP %s\n", a_ok ? "drawn" : "failed", b_ok ? "drawn" : "failed");
            pass = false;
        } else {
            unsigned maxd, off = diff(ours, warp, &maxd);
            P("scene: %u of %u pixels differ by more than 8 (largest %u); pixel (128,128) ours 0x%08x WARP 0x%08x, (4,4) 0x%08x / 0x%08x\n",
              off, W * H, maxd, ours[128 * W + 128], warp[128 * W + 128], ours[4 * W + 4], warp[4 * W + 4]);
            pass = off < 200;
        }
        if (argc > 1) {
            ppm("d3d11-ours.ppm", ours);
            ppm("d3d11-warp.ppm", warp);
        }
        /* multisampling: 4x, resolved, on the 10_0 devices (2026-11-27) */
        if (a_ok && b_ok) {
            static unsigned mo[W * H], mw[W * H];
            bool om = msaa_scene(dev, c, mo), wm = msaa_scene(wdev, wc, mw);
            if (!om || !wm) {
                P("msaa 4x: ours %s, WARP %s\n", om ? "drawn" : "failed", wm ? "drawn" : "failed");
                pass = false;
            } else {
                /* the three flat colours (clear, the two triangles) are the
                 * three most common; the rest are edge blends, a few
                 * hundred with antialiasing and none without */
                unsigned maxd, off = diff(mo, mw, &maxd), blended = 0, col[16] = {}, cnt[16] = {}, ncol = 0;
                for (int i = 0; i < W * H; i++) {
                    unsigned p = mo[i] & 0xffffff, k = 0;
                    while (k < ncol && col[k] != p) k++;
                    if (k == ncol && ncol < 16) col[ncol++] = p;
                    if (k < 16) cnt[k]++;
                }
                unsigned big[3] = {};                   /* the three largest counts */
                for (unsigned k = 0; k < ncol; k++) {
                    unsigned v = cnt[k];
                    for (int t = 0; t < 3; t++)
                        if (v > big[t]) { unsigned s = big[t]; big[t] = v; v = s; }
                }
                blended = W * H - big[0] - big[1] - big[2];
                P("msaa 4x: %u of %u pixels differ by more than 8 (largest %u), %u edge pixels blended in %u colours\n", off, W * H,
                  maxd, blended, ncol);
                pass = pass && off < 200 && blended > 200 && blended < 5000;
                if (argc > 1) {
                    ppm("d3d11-msaa-ours.ppm", mo);
                    ppm("d3d11-msaa-warp.ppm", mw);
                }
            }
        }
        /* feature level 10_1: the scene with shader model 4.1 (Gather) */
        {
            static const D3D_FEATURE_LEVEL ten1[] = { D3D_FEATURE_LEVEL_10_1 };
            static unsigned ours41[W * H], warp41[W * H];
            ID3D11Device *d41 = NULL, *w41 = NULL;
            ID3D11DeviceContext *c41 = NULL, *wc41 = NULL;
            hr = D3D11CreateDevice(mine, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, ten1, 1, D3D11_SDK_VERSION, &d41, NULL, &c41);
            P("scene on ours (10_1, shader model 4.1): device 0x%08lx\n", (unsigned long)hr);
            bool o41 = SUCCEEDED(hr) && scene(d41, c41, ours41, NULL, true);
            hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, ten1, 1, D3D11_SDK_VERSION, &w41, NULL, &wc41);
            bool k41 = SUCCEEDED(hr) && scene(w41, wc41, warp41, NULL, true);
            if (!o41 || !k41) {
                P("scene 10_1: ours %s, WARP %s\n", o41 ? "drawn" : "failed", k41 ? "drawn" : "failed");
                pass = false;
            } else {
                unsigned maxd, off = diff(ours41, warp41, &maxd);
                P("scene 10_1: %u of %u pixels differ by more than 8 (largest %u)\n", off, W * H, maxd);
                pass = pass && off < 200;
            }
            if (argc > 1) ppm("d3d11-ours-10_1.ppm", ours41);
            R(c41); R(d41); R(wc41); R(w41);
        }
        /* feature level 11_0: the compute pipeline and unordered access
         * against WARP (offered while D3D11FL11 lists this program; none
         * offered is no failure yet) */
        {
            static const D3D_FEATURE_LEVEL eleven[] = { D3D_FEATURE_LEVEL_11_0 };
            static CsOut co, cw;
            ID3D11Device *d11 = NULL, *w11 = NULL;
            ID3D11DeviceContext *c11 = NULL, *wc11 = NULL;
            hr = D3D11CreateDevice(mine, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, eleven, 1, D3D11_SDK_VERSION, &d11, NULL, &c11);
            P("compute on ours (11_0): device 0x%08lx\n", (unsigned long)hr);
            if (SUCCEEDED(hr)) {
                bool oc = compute(d11, c11, &co);
                hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, eleven, 1, D3D11_SDK_VERSION, &w11, NULL, &wc11);
                bool wcok = SUCCEEDED(hr) && compute(w11, wc11, &cw);
                if (!oc || !wcok) {
                    P("compute: ours %s, WARP %s\n", oc ? "ran" : "failed", wcok ? "ran" : "failed");
                    pass = false;
                } else {
                    unsigned bad = 0, maxd = 0, ioff = 0;
                    for (int i = 0; i < 256; i++) bad += co.outb[i] != cw.outb[i];
                    for (int i = 0; i < 64 * 64; i++) {
                        unsigned d = 0;
                        for (int k = 0; k < 32; k += 8) {
                            int x = (int)((co.img[i] >> k) & 255) - (int)((cw.img[i] >> k) & 255);
                            d = d > (unsigned)abs(x) ? d : (unsigned)abs(x);
                        }
                        maxd = maxd > d ? maxd : d;
                        ioff += d > 1;
                    }
                    P("compute: structured output %u of 256 differ (ours [255] %u, WARP %u)\n", bad, co.outb[255], cw.outb[255]);
                    P("compute: atomic counter ours %u WARP %u; appended ours %u (sum %u) WARP %u (sum %u)\n", co.counter, cw.counter,
                      co.appended, co.append_sum, cw.appended, cw.append_sum);
                    P("compute: pixel shader UAV count ours %u WARP %u\n", co.pixels, cw.pixels);
                    P("compute: image %u of 4096 pixels differ by more than 1 (largest %u); (0,0) 0x%08x / 0x%08x, (63,63) 0x%08x / 0x%08x\n",
                      ioff, maxd, co.img[0], cw.img[0], co.img[4095], cw.img[4095]);
                    pass = pass && !bad && co.counter == cw.counter && co.appended == cw.appended && co.append_sum == cw.append_sum &&
                           co.pixels == cw.pixels && !ioff;
                }
                /* tessellation (step 2) */
                static unsigned to[W * H], tw[W * H];
                UINT64 ohs = 0, ods = 0, whs = 0, wds = 0;
                bool ot = tess_scene(d11, c11, to, &ohs, &ods);
                bool wt = w11 && tess_scene(w11, wc11, tw, &whs, &wds);
                if (!ot || !wt) {
                    P("tessellation: ours %s, WARP %s\n", ot ? "drawn" : "failed", wt ? "drawn" : "failed");
                    pass = false;
                } else {
                    unsigned maxd, off = diff(to, tw, &maxd), drawn = 0;
                    for (int i = 0; i < W * H; i++) drawn += to[i] != to[0];
                    P("tessellation: %u of %u pixels differ by more than 8 (largest %u), %u drawn; hull / domain invocations ours %llu / "
                      "%llu, WARP %llu / %llu\n", off, W * H, maxd, drawn, (unsigned long long)ohs, (unsigned long long)ods,
                      (unsigned long long)whs, (unsigned long long)wds);
                    pass = pass && off < 200 && drawn > W * H / 4;   /* the patch covers ~64% of the frame */
                }
                if (argc > 1) {
                    ppm("d3d11-tess-ours.ppm", to);
                    ppm("d3d11-tess-warp.ppm", tw);
                }
                R(c11); R(d11); R(wc11); R(w11);
            }
        }
        /* Direct3D 10 programs: d3d10.dll's device, and d3d10_1.dll's at
         * 10_1 and at 10_0, against the 10_0 scene on WARP */
        if (b_ok) {
            static unsigned ten_px[W * H];
            static const struct { int lib; D3D10_FEATURE_LEVEL1 fl; const char *name, *file; } d10[] = {
                { 0, D3D10_FEATURE_LEVEL_10_0, "d3d10.dll", "d3d10-ours.ppm" },
                { 1, D3D10_FEATURE_LEVEL_10_1, "d3d10_1.dll at 10_1", "d3d10_1-ours.ppm" },
                { 1, D3D10_FEATURE_LEVEL_10_0, "d3d10_1.dll at 10_0", "d3d10_1-ours-10_0.ppm" } };
            for (int k = 0; k < 3; k++) {
                ID3D10Device *d10dev = NULL;
                ID3D10Device1 *d101 = NULL;
                if (d10[k].lib == 0) {
                    hr = D3D10CreateDevice(mine, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &d10dev);
                } else {
                    hr = D3D10CreateDevice1(mine, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, d10[k].fl, D3D10_1_SDK_VERSION, &d101);
                    d10dev = d101;
                }
                P("Direct3D 10, %s: device 0x%08lx\n", d10[k].name, (unsigned long)hr);
                if (FAILED(hr) || !scene10(d10dev, ten_px)) {
                    P("Direct3D 10, %s: failed\n", d10[k].name);
                    pass = false;
                } else {
                    unsigned maxd, off = diff(ten_px, warp, &maxd);
                    P("Direct3D 10, %s: %u of %u pixels differ by more than 8 (largest %u)\n", d10[k].name, off, W * H, maxd);
                    pass = pass && off < 200;
                    if (argc > 1) ppm(d10[k].file, ten_px);
                }
                R(d10dev);
            }
        }
        /* "present": the scene through a swap chain and DWM, both models */
        if (argc > 3) hold_ms = (DWORD)atoi(argv[3]);
        if (b_ok && argc > 2 && !strcmp(argv[2], "present")) {
            static unsigned shown[W * H];
            static const struct { DXGI_SWAP_EFFECT e; const char *name, *file; } models[] = {
                { DXGI_SWAP_EFFECT_DISCARD, "blt model (DISCARD)", "d3d11-present-blt.ppm" },
                { DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL, "flip model (FLIP_SEQUENTIAL)", "d3d11-present-flip.ppm" } };
            for (int m = 0; m < 2; m++) {
                if (!presented(mine, models[m].e, shown)) {
                    P("present, %s: failed\n", models[m].name);
                    pass = false;
                    continue;
                }
                unsigned maxd, off = diff(shown, warp, &maxd);
                P("present, %s: %u of %u pixels differ by more than 8 (largest %u); (128,128) 0x%08x, (4,4) 0x%08x\n", models[m].name,
                  off, W * H, maxd, shown[128 * W + 128], shown[4 * W + 4]);
                pass = pass && off < 200;
                if (argc > 1) ppm(models[m].file, shown);
            }
        }
    }
    P("d3d11-probe: %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
