/*
 * DX9CAPS.EXE / DX8CAPS.EXE: what Microsoft's runtime makes of the driver
 * (track M16). One source, built twice: -DDXVER=9 over d3d9.dll, -DDXVER=8
 * over d3d8.dll (their headers cannot share a unit).
 *
 * It asks the questions a game and Wine's conformance tests ask before
 * they draw anything, one line each, with the HRESULT:
 *   the adapter identifier and display mode,
 *   CheckDeviceType windowed and full screen for X8R8G8B8 / A8R8G8B8 /
 *   R5G6B5 back buffers, CheckDeviceFormat for the depth formats,
 *   CheckDepthStencilMatch, then CreateDevice with the tests' parameters
 *   (640x480 windowed, D3DFMT_A8R8G8B8, D24S8 then D16, hardware then
 *   software vertex processing),
 * and dumps the caps the runtime reports (the device's, after a device
 * exists, else the adapter's). Log: C:\2KSBOX\DX9CAPS.LOG (DX8CAPS.LOG).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#if DXVER == 8
#include <d3d8.h>
#define D3D IDirect3D8
#define DEV IDirect3DDevice8
#define CAPS D3DCAPS8
#define IDENT D3DADAPTER_IDENTIFIER8
#define NAME "DX8CAPS"
#define D3D_(m) IDirect3D8_##m
#define DEV_(m) IDirect3DDevice8_##m
#else
#include <d3d9.h>
#define D3D IDirect3D9
#define DEV IDirect3DDevice9
#define CAPS D3DCAPS9
#define IDENT D3DADAPTER_IDENTIFIER9
#define NAME "DX9CAPS"
#define D3D_(m) IDirect3D9_##m
#define DEV_(m) IDirect3DDevice9_##m
#endif
#include "../guestlog.h"

static FILE *out;

static void say(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(out);
}

static const char *fmtname(D3DFORMAT f)
{
    static char buf[16];

    switch (f) {
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_X1R5G5B5: return "X1R5G5B5";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D32: return "D32";
    default:
        snprintf(buf, sizeof buf, "%lu", (unsigned long)f);
        return buf;
    }
}

static void dump_caps(const CAPS *c, const char *whose)
{
    say("caps (%s):\n", whose);
    say("  DeviceType %u Caps 0x%08lx Caps2 0x%08lx Caps3 0x%08lx PresentationIntervals 0x%08lx\n",
        (unsigned)c->DeviceType, c->Caps, c->Caps2, c->Caps3, c->PresentationIntervals);
    say("  DevCaps 0x%08lx PrimitiveMiscCaps 0x%08lx RasterCaps 0x%08lx ZCmpCaps 0x%08lx\n",
        c->DevCaps, c->PrimitiveMiscCaps, c->RasterCaps, c->ZCmpCaps);
    say("  SrcBlendCaps 0x%08lx DestBlendCaps 0x%08lx AlphaCmpCaps 0x%08lx ShadeCaps 0x%08lx\n",
        c->SrcBlendCaps, c->DestBlendCaps, c->AlphaCmpCaps, c->ShadeCaps);
    say("  TextureCaps 0x%08lx TextureFilterCaps 0x%08lx CubeTextureFilterCaps 0x%08lx\n",
        c->TextureCaps, c->TextureFilterCaps, c->CubeTextureFilterCaps);
    say("  VolumeTextureFilterCaps 0x%08lx TextureAddressCaps 0x%08lx VolumeTextureAddressCaps 0x%08lx\n",
        c->VolumeTextureFilterCaps, c->TextureAddressCaps, c->VolumeTextureAddressCaps);
    say("  LineCaps 0x%08lx MaxTexture %lux%lu MaxVolumeExtent %lu MaxTextureRepeat %lu Aspect %lu MaxAnisotropy %lu\n",
        c->LineCaps, c->MaxTextureWidth, c->MaxTextureHeight, c->MaxVolumeExtent, c->MaxTextureRepeat,
        c->MaxTextureAspectRatio, c->MaxAnisotropy);
    say("  StencilCaps 0x%08lx FVFCaps 0x%08lx TextureOpCaps 0x%08lx Stages %lu Simultaneous %lu\n",
        c->StencilCaps, c->FVFCaps, c->TextureOpCaps, c->MaxTextureBlendStages, c->MaxSimultaneousTextures);
    say("  VertexProcessingCaps 0x%08lx Lights %lu ClipPlanes %lu BlendMatrices %lu Index %lu\n",
        c->VertexProcessingCaps, c->MaxActiveLights, c->MaxUserClipPlanes, c->MaxVertexBlendMatrices,
        c->MaxVertexBlendMatrixIndex);
    say("  MaxPointSize %g MaxPrimitiveCount %lu MaxVertexIndex %lu MaxStreams %lu MaxStreamStride %lu\n",
        c->MaxPointSize, c->MaxPrimitiveCount, c->MaxVertexIndex, c->MaxStreams, c->MaxStreamStride);
    say("  VertexShaderVersion 0x%08lx MaxVertexShaderConst %lu PixelShaderVersion 0x%08lx\n",
        c->VertexShaderVersion, c->MaxVertexShaderConst, c->PixelShaderVersion);
#if DXVER == 8
    say("  MaxPixelShaderValue %g\n", c->MaxPixelShaderValue);
#else
    say("  PixelShader1xMaxValue %g DevCaps2 0x%08lx MaxNpatchTessellationLevel %g\n",
        c->PixelShader1xMaxValue, c->DevCaps2, c->MaxNpatchTessellationLevel);
    say("  MasterAdapterOrdinal %u AdapterOrdinalInGroup %u NumberOfAdaptersInGroup %u\n",
        c->MasterAdapterOrdinal, c->AdapterOrdinalInGroup, c->NumberOfAdaptersInGroup);
    say("  DeclTypes 0x%08lx NumSimultaneousRTs %lu StretchRectFilterCaps 0x%08lx\n",
        c->DeclTypes, c->NumSimultaneousRTs, c->StretchRectFilterCaps);
    say("  VS20Caps: Caps 0x%08lx DynamicFlowControlDepth %d NumTemps %d StaticFlowControlDepth %d\n",
        c->VS20Caps.Caps, c->VS20Caps.DynamicFlowControlDepth, c->VS20Caps.NumTemps,
        c->VS20Caps.StaticFlowControlDepth);
    say("  PS20Caps: Caps 0x%08lx DynamicFlowControlDepth %d NumTemps %d StaticFlowControlDepth %d NumInstructionSlots %d\n",
        c->PS20Caps.Caps, c->PS20Caps.DynamicFlowControlDepth, c->PS20Caps.NumTemps,
        c->PS20Caps.StaticFlowControlDepth, c->PS20Caps.NumInstructionSlots);
    say("  VertexTextureFilterCaps 0x%08lx MaxVShaderInstructionsExecuted %lu MaxPShaderInstructionsExecuted %lu\n",
        c->VertexTextureFilterCaps, c->MaxVShaderInstructionsExecuted, c->MaxPShaderInstructionsExecuted);
    say("  MaxVertexShader30InstructionSlots %lu MaxPixelShader30InstructionSlots %lu\n",
        c->MaxVertexShader30InstructionSlots, c->MaxPixelShader30InstructionSlots);
#endif
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(w, m, wp, lp);
}

int main(void)
{
    static const D3DFORMAT bb[] = { D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, D3DFMT_R5G6B5 };
    static const D3DFORMAT ds[] = { D3DFMT_D24S8, D3DFMT_D16, D3DFMT_D24X8, D3DFMT_D32 };
    static const DWORD vp[] = { D3DCREATE_HARDWARE_VERTEXPROCESSING, D3DCREATE_SOFTWARE_VERTEXPROCESSING };
    D3D *(WINAPI *create)(UINT);
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    IDENT id;
    CAPS caps;
    WNDCLASSA wc = { 0 };
    HMODULE lib;
    HRESULT hr;
    HWND wnd;
    D3D *d3d;
    DEV *dev = NULL;
    unsigned i, j, k;

    out = guest_log_open(NAME ".LOG", "w");
    if (!out)
        out = stdout;
    lib = LoadLibraryA(DXVER == 8 ? "d3d8.dll" : "d3d9.dll");
    create = lib ? (void *)GetProcAddress(lib, DXVER == 8 ? "Direct3DCreate8" : "Direct3DCreate9") : NULL;
    if (!create) {
        say(NAME ": no d3d%d.dll here\n", DXVER);
        return 1;
    }
    d3d = create(DXVER == 8 ? D3D_SDK_VERSION : D3D_SDK_VERSION);
    if (!d3d) {
        say(NAME ": Direct3DCreate%d failed\n", DXVER);
        return 1;
    }

#if DXVER == 8
    hr = D3D_(GetAdapterIdentifier)(d3d, 0, D3DENUM_NO_WHQL_LEVEL, &id);
#else
    hr = D3D_(GetAdapterIdentifier)(d3d, 0, 0, &id);
#endif
    say("adapter: 0x%08lx \"%s\" \"%s\" %u.%u.%u.%u vendor 0x%04lx device 0x%04lx\n", hr, id.Driver, id.Description,
        HIWORD(id.DriverVersion.HighPart), LOWORD(id.DriverVersion.HighPart), HIWORD(id.DriverVersion.LowPart),
        LOWORD(id.DriverVersion.LowPart), id.VendorId, id.DeviceId);
    /* every adapter the runtime lists: a Voodoo 2's DirectDraw driver
     * (3DFX32V2.DLL) is a second one, listed even with no card */
    say("adapters: %u\n", D3D_(GetAdapterCount)(d3d));
    for (i = 1; i < D3D_(GetAdapterCount)(d3d); i++) {
#if DXVER == 8
        hr = D3D_(GetAdapterIdentifier)(d3d, i, D3DENUM_NO_WHQL_LEVEL, &id);
#else
        hr = D3D_(GetAdapterIdentifier)(d3d, i, 0, &id);
#endif
        say("adapter %u: 0x%08lx \"%s\" \"%s\"\n", i, hr, id.Driver, id.Description);
    }
#if DXVER == 8
    D3D_(GetAdapterIdentifier)(d3d, 0, D3DENUM_NO_WHQL_LEVEL, &id);
#else
    D3D_(GetAdapterIdentifier)(d3d, 0, 0, &id);
#endif
    hr = D3D_(GetAdapterDisplayMode)(d3d, 0, &mode);
    say("display mode: 0x%08lx %ux%u %s %u Hz\n", hr, mode.Width, mode.Height, fmtname(mode.Format), mode.RefreshRate);

    for (i = 0; i < 2; i++)
        for (j = 0; j < sizeof bb / sizeof bb[0]; j++) {
            D3DFORMAT disp = i ? (bb[j] == D3DFMT_A8R8G8B8 ? D3DFMT_X8R8G8B8 : bb[j]) : mode.Format;
            hr = D3D_(CheckDeviceType)(d3d, 0, D3DDEVTYPE_HAL, disp, bb[j], !i);
            say("CheckDeviceType %s display %s back buffer %s: 0x%08lx\n", i ? "fullscreen" : "windowed",
                fmtname(disp), fmtname(bb[j]), hr);
        }
    for (k = 0; k < sizeof ds / sizeof ds[0]; k++) {
        hr = D3D_(CheckDeviceFormat)(d3d, 0, D3DDEVTYPE_HAL, mode.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_SURFACE, ds[k]);
        say("CheckDeviceFormat depth %s: 0x%08lx", fmtname(ds[k]), hr);
        hr = D3D_(CheckDepthStencilMatch)(d3d, 0, D3DDEVTYPE_HAL, mode.Format, D3DFMT_A8R8G8B8, ds[k]);
        say(", match with A8R8G8B8: 0x%08lx\n", hr);
    }
    hr = D3D_(GetDeviceCaps)(d3d, 0, D3DDEVTYPE_HAL, &caps);
    say("GetDeviceCaps: 0x%08lx\n", hr);
    if (SUCCEEDED(hr))
        dump_caps(&caps, "adapter");

    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = NAME;
    RegisterClassA(&wc);
    wnd = CreateWindowA(NAME, NAME, WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 480, NULL, NULL, NULL, NULL);

    for (i = 0; i < 2; i++)
        for (k = 0; k < 2; k++) {
            memset(&pp, 0, sizeof pp);
            pp.BackBufferWidth = 640;
            pp.BackBufferHeight = 480;
            pp.BackBufferFormat = D3DFMT_A8R8G8B8;
            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
            pp.hDeviceWindow = wnd;
            pp.Windowed = TRUE;
            pp.EnableAutoDepthStencil = TRUE;
            pp.AutoDepthStencilFormat = ds[k];
            hr = D3D_(CreateDevice)(d3d, 0, D3DDEVTYPE_HAL, wnd, vp[i], &pp, &dev);
            say("CreateDevice windowed A8R8G8B8 %s %s: 0x%08lx\n", fmtname(ds[k]), i ? "SWVP" : "HWVP", hr);
            if (SUCCEEDED(hr)) {
                if (DEV_(GetDeviceCaps)(dev, &caps) == D3D_OK)
                    dump_caps(&caps, "device");
                DEV_(Release)(dev);
                dev = NULL;
            }
        }
    memset(&pp, 0, sizeof pp);
    pp.BackBufferFormat = mode.Format;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Windowed = TRUE;
    hr = D3D_(CreateDevice)(d3d, 0, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    say("CreateDevice windowed desktop format, no depth, SWVP: 0x%08lx\n", hr);
    if (dev)
        DEV_(Release)(dev);
    D3D_(Release)(d3d);
    DestroyWindow(wnd);
    say(NAME ": done\n");
    return 0;
}
