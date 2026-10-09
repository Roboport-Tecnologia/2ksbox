/*
 * wddm-probe: what Windows 11's Direct3D gets from each display adapter,
 * the WDDM driver's (track M20) beside Microsoft's Basic Render Driver.
 * For adapter 0 the D3DCAPS9 fields Direct3D 10Level9 tests (the WDK's
 * "Required Direct3D 9 capabilities"); for every adapter the Direct3D 11
 * device a WinUI 3 compositor makes (its feature level: the WDDM driver
 * must reach 9_2, WinUI 3 apps refuse a 9_1 device), the support of each
 * format d3d10level9's feature level 9_2 table names, shared and
 * keyed-mutex textures, Direct2D, DirectComposition and a composition
 * swap chain. Output into the file argv[1] (stdout without one).
 *
 * Built on the PC in MSYS2's MINGW64 shell, x64 MSVC:
 *   . scripts/msvc-env.sh && MSYS2_ARG_CONV_EXCL='*' cl -nologo -O1 -MT -EHsc tools/wddm-probe.cpp
 * Run in the guest's logged-on session (a scheduled task with an
 * interactive principal; SYSTEM's session 0 has no desktop to compose),
 * docs/testing.md "wddm-probe".
 */
#define INITGUID
#include <windows.h>
#include <stdio.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <d2d1_1.h>
#include <dcomp.h>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxguid.lib")

static FILE *out;
#define P(...) do { fprintf(out, __VA_ARGS__); fflush(out); } while (0)

/* adapter 0's D3DCAPS9: the fields d3d10level9's RequiredCaps tables test */
static void caps9(void)
{
    IDirect3D9 *d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DCAPS9 c;

    if (!d || FAILED(d->GetDeviceCaps(0, D3DDEVTYPE_HAL, &c))) {
        P("D3DCAPS9: none\n");
        return;
    }
#define X(f) P("  %-28s 0x%08lx\n", #f, (unsigned long)c.f)
#define D(f) P("  %-28s %lu\n", #f, (unsigned long)c.f)
#define F(f) P("  %-28s %g\n", #f, (double)c.f)
    P("D3DCAPS9 of adapter 0:\n");
    X(Caps2); X(PresentationIntervals); X(PrimitiveMiscCaps); X(ShadeCaps); X(TextureFilterCaps); X(TextureCaps);
    X(TextureAddressCaps); X(VolumeTextureAddressCaps); X(TextureOpCaps); X(SrcBlendCaps); X(DestBlendCaps);
    X(StretchRectFilterCaps); X(ZCmpCaps); X(RasterCaps); X(StencilCaps); X(DevCaps2);
    D(MaxTextureWidth); D(MaxTextureHeight); D(NumSimultaneousRTs); D(MaxSimultaneousTextures); D(MaxTextureBlendStages);
    X(PixelShaderVersion); X(VertexShaderVersion); D(MaxPrimitiveCount); D(MaxVertexIndex); D(MaxVolumeExtent);
    D(MaxTextureRepeat); D(MaxAnisotropy); F(MaxVertexW); D(MaxVertexShaderConst);
    X(PS20Caps.Caps); D(PS20Caps.NumInstructionSlots); D(PS20Caps.NumTemps); D(VS20Caps.NumTemps);
#undef X
#undef D
#undef F
    d->Release();
}

static void probe(IDXGIAdapter1 *ad)
{
    static const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                            D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2,
                                            D3D_FEATURE_LEVEL_9_1 };
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;
    HRESULT hr;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT, support = 0;

    hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, flags, fl, 7, D3D11_SDK_VERSION, &dev, &got, &ctx);
    P("  D3D11CreateDevice(BGRA): 0x%08lx, feature level 0x%x\n", hr, got);
    if (FAILED(hr)) {
        hr = D3D11CreateDevice(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, fl, 7, D3D11_SDK_VERSION, &dev, &got, &ctx);
        P("  D3D11CreateDevice(no BGRA): 0x%08lx, feature level 0x%x\n", hr, got);
        if (FAILED(hr)) return;
    }
    (dev)->CheckFormatSupport(DXGI_FORMAT_B8G8R8A8_UNORM, &support);
    P("  B8G8R8A8 support 0x%x\n", support);
    {
        /* what d3d10level9 wants for feature level 9_2 (its MinCapsLevel2 table) and what it derives */
        static const struct { UINT fmt, want; const char *name; } req[] = {
            { 87, 0x010af3e0, "B8G8R8A8_UNORM" }, { 91, 0x010af3e0, "B8G8R8A8_UNORM_SRGB" },
            { 2, 0x24120, "R32G32B32A32_FLOAT" }, { 10, 0x271e0, "R16G16B16A16_FLOAT" },
            { 11, 0x273e0, "R16G16B16A16_UNORM" }, { 41, 0x271e0, "R32_FLOAT" },
            { 28, 0x010af3e0, "R8G8B8A8_UNORM" }, { 29, 0x010af3e0, "R8G8B8A8_UNORM_SRGB" },
            { 34, 0x271e0, "R16G16_FLOAT" }, { 37, 0x213e0, "R16G16_SNORM" }, { 35, 0x273e0, "R16G16_UNORM" },
            { 56, 0x213e0, "R16_UNORM" }, { 65, 0x201e0, "A8_UNORM" }, { 42, 0x4, "R32_UINT" },
        };
        for (UINT k = 0; k < sizeof req / sizeof req[0]; k++) {
            UINT s2 = 0;
            dev->CheckFormatSupport((DXGI_FORMAT)req[k].fmt, &s2);
            P("  %-22s have 0x%08x want 0x%08x missing 0x%08x\n", req[k].name, s2, req[k].want, req[k].want & ~s2);
        }
    }
    {
        D3D11_FEATURE_DATA_THREADING th = { 0 };
        D3D11_FEATURE_DATA_D3D9_OPTIONS o9 = { 0 };
        D3D11_FEATURE_DATA_D3D11_OPTIONS o11 = { 0 };
        hr = (dev)->CheckFeatureSupport(D3D11_FEATURE_THREADING, &th, sizeof th);
        P("  threading 0x%08lx: concurrent creates %d, command lists %d\n", hr, th.DriverConcurrentCreates, th.DriverCommandLists);
        hr = (dev)->CheckFeatureSupport(D3D11_FEATURE_D3D9_OPTIONS, &o9, sizeof o9);
        P("  d3d9 options 0x%08lx: full non-pow2 %d\n", hr, o9.FullNonPow2TextureSupport);
        hr = (dev)->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o11, sizeof o11);
        P("  d3d11 options 0x%08lx\n", hr);
    }
    {
        D3D11_TEXTURE2D_DESC td = { 0 };
        ID3D11Texture2D *t = NULL;
        td.Width = 256; td.Height = 256; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
        hr = (dev)->CreateTexture2D(&td, NULL, &t);
        P("  shared BGRA texture: 0x%08lx\n", hr);
        if (t) (t)->Release();
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        hr = (dev)->CreateTexture2D(&td, NULL, &t);
        P("  keyed-mutex BGRA texture: 0x%08lx\n", hr);
        if (t) (t)->Release();
        td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        hr = (dev)->CreateTexture2D(&td, NULL, &t);
        P("  NT-handle BGRA texture: 0x%08lx\n", hr);
        if (t) (t)->Release();
        td.MiscFlags = D3D11_RESOURCE_MISC_GDI_COMPATIBLE;
        hr = (dev)->CreateTexture2D(&td, NULL, &t);
        P("  GDI-compatible BGRA texture: 0x%08lx\n", hr);
        if (t) (t)->Release();
    }
    {
        IDXGIDevice *dx = NULL;
        ID2D1Factory1 *f = NULL;
        ID2D1Device *d2 = NULL;
        IDCompositionDevice *dc = NULL;
        IDXGIFactory2 *fac = NULL;
        IDXGISwapChain1 *sc = NULL;
        DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
        D2D1_FACTORY_OPTIONS fo = { D2D1_DEBUG_LEVEL_NONE };

        (dev)->QueryInterface(__uuidof(IDXGIDevice), (void **)&dx);
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, (void **)&f);
        if (SUCCEEDED(hr)) hr = (f)->CreateDevice(dx, &d2);
        P("  D2D device: 0x%08lx\n", hr);
        hr = DCompositionCreateDevice(dx, __uuidof(IDCompositionDevice), (void **)&dc);
        P("  DCompositionCreateDevice: 0x%08lx\n", hr);
        hr = CreateDXGIFactory2(0, __uuidof(IDXGIFactory2), (void **)&fac);
        sd.Width = 256; sd.Height = 256; sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (SUCCEEDED(hr)) hr = (fac)->CreateSwapChainForComposition(dev, &sd, NULL, &sc);
        P("  CreateSwapChainForComposition: 0x%08lx\n", hr);
        if (sc) (sc)->Release();
        if (fac) (fac)->Release();
        if (dc) (dc)->Release();
        if (d2) (d2)->Release();
        if (f) (f)->Release();
        if (dx) (dx)->Release();
    }
    (ctx)->Release();
    (dev)->Release();
}

int main(int argc, char **argv)
{
    IDXGIFactory1 *f;
    IDXGIAdapter1 *ad;
    UINT i;

    out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out) return 1;
    caps9();
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) { P("no DXGI factory\n"); return 1; }
    for (i = 0; f->EnumAdapters1(i, &ad) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 d;
        (ad)->GetDesc1(&d);
        P("adapter %u: %ls (vendor 0x%04x device 0x%04x, flags 0x%x)\n", i, d.Description, d.VendorId, d.DeviceId, d.Flags);
        probe(ad);
        (ad)->Release();
    }
    return 0;
}
