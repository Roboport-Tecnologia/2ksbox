/*
 * dxgi-modes: the display modes a Windows 10 / 11 guest offers games
 * (track M25). For each adapter and output, IDXGIOutput::GetDisplayModeList
 * for the formats a game's swap chain uses (B8G8R8A8 and R8G8B8A8, their
 * sRGB forms, R10G10B10A2, R16G16B16A16_FLOAT), plain and with
 * DXGI_ENUM_MODES_SCALING; the count and each size once with its rates;
 * FindClosestMatchingMode for the desktop's size in R8G8B8A8; then
 * EnumDisplaySettings' list, which the GDI side (and a game's own list,
 * when it reads that one) sees.
 *
 * With "fullscreen" as argv[2]: an R8G8B8A8 swap chain (a game's, The
 * Witcher 3's) made full screen at 1280x720 on our adapter, cleared to
 * pure red and presented for argv[3] milliseconds (5000), the mode
 * Windows set meanwhile reported; a screendump in that time shows red,
 * not blue, when the device shows the mode in its channel order.
 *
 * Output into the file argv[1] (stdout without one); the last line says
 * "dxgi-modes: done".
 *
 * Built on the PC in MSYS2's MINGW64 shell, x64 MSVC:
 *   . scripts/msvc-env.sh && MSYS2_ARG_CONV_EXCL='*' cl -nologo -O1 -MT -EHsc tools/dxgi-modes.cpp
 * Run in the guest's logged-on session (a scheduled task with an
 * interactive principal, as d3d11-probe).
 */
#include <windows.h>
#include <dxgi1_2.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "user32.lib")

static FILE *out;

static void current_mode(const char *when)
{
    DEVMODEA dm;
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        fprintf(out, "mode %s: %lux%lu %lu bpp\n", when, dm.dmPelsWidth, dm.dmPelsHeight, dm.dmBitsPerPel);
    }
}

/* the R8G8B8A8 full-screen swap chain, red */
static void fullscreen(IDXGIAdapter1 *ad, DWORD hold_ms)
{
    WNDCLASSA wc = {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "dxgimodes";
    RegisterClassA(&wc);
    HWND w = CreateWindowA("dxgimodes", "dxgi-modes", WS_POPUP | WS_VISIBLE, 0, 0, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(w);
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferDesc.Width = 1280;
    sd.BufferDesc.Height = 720;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.OutputWindow = w;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev,
                                               NULL, &ctx);
    fprintf(out, "fullscreen: device and swap chain hr 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) {
        return;
    }
    current_mode("before");
    hr = sc->SetFullscreenState(TRUE, NULL);
    fprintf(out, "fullscreen: SetFullscreenState hr 0x%08lx\n", (unsigned long)hr);
    if (SUCCEEDED(hr)) {
        hr = sc->ResizeBuffers(2, 1280, 720, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
        fprintf(out, "fullscreen: ResizeBuffers hr 0x%08lx\n", (unsigned long)hr);
    }
    fflush(out);
    DWORD t0 = GetTickCount();
    bool said = false;
    while (GetTickCount() - t0 < hold_ms) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            DispatchMessageA(&msg);
        }
        ID3D11Texture2D *bb = NULL;
        ID3D11RenderTargetView *rtv = NULL;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) &&
            SUCCEEDED(dev->CreateRenderTargetView(bb, NULL, &rtv))) {
            const float red[4] = { 1, 0, 0, 1 };
            ctx->ClearRenderTargetView(rtv, red);
        }
        if (rtv) rtv->Release();
        if (bb) bb->Release();
        hr = sc->Present(1, 0);
        if (!said && GetTickCount() - t0 > 1000) {
            BOOL fs = FALSE;
            sc->GetFullscreenState(&fs, NULL);
            fprintf(out, "fullscreen: after 1 s, full screen %d, present hr 0x%08lx\n", fs, (unsigned long)hr);
            current_mode("during");
            fprintf(out, "fullscreen: red now\n");
            fflush(out);
            said = true;
        }
    }
    sc->SetFullscreenState(FALSE, NULL);
    current_mode("after");
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(w);
}

static const struct { DXGI_FORMAT f; const char *name; } fmts[] = {
    { DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM" },
    { DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, "B8G8R8A8_UNORM_SRGB" },
    { DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM" },
    { DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, "R8G8B8A8_UNORM_SRGB" },
    { DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM" },
    { DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT" },
};

static void list(IDXGIOutput *o, DXGI_FORMAT f, const char *name, UINT flags)
{
    UINT n = 0;
    HRESULT hr = o->GetDisplayModeList(f, flags, &n, NULL);
    fprintf(out, "  %-20s %-7s hr 0x%08lx, %u modes", name, flags ? "scaling" : "plain", (unsigned long)hr, n);
    if (FAILED(hr) || !n) {
        fprintf(out, "\n");
        return;
    }
    DXGI_MODE_DESC *m = new DXGI_MODE_DESC[n];
    hr = o->GetDisplayModeList(f, flags, &n, m);
    fprintf(out, ":");
    UINT lw = 0, lh = 0;
    for (UINT i = 0; i < n; i++) {
        if (m[i].Width != lw || m[i].Height != lh) {
            fprintf(out, "\n    %ux%u", m[i].Width, m[i].Height);
            lw = m[i].Width;
            lh = m[i].Height;
        }
        fprintf(out, " %u/%u(s%u,o%u)", m[i].RefreshRate.Numerator, m[i].RefreshRate.Denominator,
                (unsigned)m[i].Scaling, (unsigned)m[i].ScanlineOrdering);
    }
    fprintf(out, "\n");
    delete[] m;
}

int main(int argc, char **argv)
{
    out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out) {
        return 1;
    }
    SetProcessDPIAware();
    IDXGIFactory1 *fac = NULL;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&fac))) {
        fprintf(out, "CreateDXGIFactory1 failed\ndxgi-modes: done\n");
        return 1;
    }
    IDXGIAdapter1 *ad;
    if (argc > 2 && !strcmp(argv[2], "fullscreen")) {
        if (fac->EnumAdapters1(0, &ad) == S_OK) {
            fullscreen(ad, argc > 3 ? (DWORD)atoi(argv[3]) : 5000);
            ad->Release();
        }
        fac->Release();
        fprintf(out, "dxgi-modes: done\n");
        if (out != stdout) fclose(out);
        return 0;
    }
    for (UINT a = 0; fac->EnumAdapters1(a, &ad) == S_OK; a++) {
        DXGI_ADAPTER_DESC1 ad_desc;
        ad->GetDesc1(&ad_desc);
        fprintf(out, "adapter %u: %ls (vendor 0x%04x device 0x%04x)\n", a, ad_desc.Description, ad_desc.VendorId,
                ad_desc.DeviceId);
        IDXGIOutput *o;
        for (UINT k = 0; ad->EnumOutputs(k, &o) == S_OK; k++) {
            DXGI_OUTPUT_DESC od;
            o->GetDesc(&od);
            RECT r = od.DesktopCoordinates;
            fprintf(out, " output %u: %ls, desktop %ldx%ld, attached %d, rotation %d\n", k, od.DeviceName,
                    r.right - r.left, r.bottom - r.top, od.AttachedToDesktop, (int)od.Rotation);
            for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++) {
                list(o, fmts[i].f, fmts[i].name, 0);
                list(o, fmts[i].f, fmts[i].name, DXGI_ENUM_MODES_SCALING);
            }
            DXGI_MODE_DESC want = {}, got = {};
            want.Width = r.right - r.left;
            want.Height = r.bottom - r.top;
            want.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            HRESULT hr = o->FindClosestMatchingMode(&want, &got, NULL);
            fprintf(out, "  FindClosestMatchingMode %ux%u R8G8B8A8: hr 0x%08lx -> %ux%u %u/%u format %u\n", want.Width,
                    want.Height, (unsigned long)hr, got.Width, got.Height, got.RefreshRate.Numerator,
                    got.RefreshRate.Denominator, (unsigned)got.Format);
            o->Release();
        }
        ad->Release();
    }
    fac->Release();

    DEVMODEA dm;
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm)) {
        fprintf(out, "EnumDisplaySettings current: %lux%lu %lu bpp %lu Hz\n", dm.dmPelsWidth, dm.dmPelsHeight,
                dm.dmBitsPerPel, dm.dmDisplayFrequency);
    }
    fprintf(out, "EnumDisplaySettings list:");
    DWORD lw = 0, lh = 0;
    for (DWORD i = 0; EnumDisplaySettingsA(NULL, i, &dm); i++) {
        if (dm.dmPelsWidth != lw || dm.dmPelsHeight != lh) {
            fprintf(out, "\n  %lux%lu", dm.dmPelsWidth, dm.dmPelsHeight);
            lw = dm.dmPelsWidth;
            lh = dm.dmPelsHeight;
        }
        fprintf(out, " %lubpp@%lu", dm.dmBitsPerPel, dm.dmDisplayFrequency);
    }
    fprintf(out, "\ndxgi-modes: done\n");
    if (out != stdout) {
        fclose(out);
    }
    return 0;
}
