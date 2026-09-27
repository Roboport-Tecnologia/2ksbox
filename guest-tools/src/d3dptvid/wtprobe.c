/*
 * WTPROBE8.EXE / WTPROBE9.EXE: why Wine's tests make no device on a machine
 * where DX8CAPS / DX9CAPS make one (track M16, the rig's Win98: every test
 * CreateDevice with an A8R8G8B8 back buffer failed D3DERR_NOTAVAILABLE,
 * while DX8CAPS made the same device). One windowed CreateDevice per line,
 * across the ways the tests and the caps tools differ:
 *   the window: the "static" class the tests use, or a class of our own;
 *               hidden (d3d8 / d3d9 stateblock) or visible
 *   the back buffer: 0 x 0 (the window's client size, as stateblock) or
 *               640 x 480; A8R8G8B8, X8R8G8B8 or D3DFMT_UNKNOWN
 *   depth: none, or D24S8; software or hardware vertex processing
 * The tag on the command line names the log (C:\2KSBOX\WTPROBE8_<tag>.LOG,
 * or under BOXLOG), so one batch can run it plainly, with stdout
 * redirected and under WTRUN, and compare. One source, built twice as
 * dx9caps.c is.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#if DXVER == 8
#include <d3d8.h>
#define D3D IDirect3D8
#define DEV IDirect3DDevice8
#define NAME "WTPROBE8"
#define D3D_(m) IDirect3D8_##m
#define DEV_(m) IDirect3DDevice8_##m
#else
#include <d3d9.h>
#define D3D IDirect3D9
#define DEV IDirect3DDevice9
#define NAME "WTPROBE9"
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
    fflush(out);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    return DefWindowProcA(w, m, wp, lp);
}

static const char *fmtname(D3DFORMAT f)
{
    return f == D3DFMT_A8R8G8B8 ? "A8R8G8B8" : f == D3DFMT_X8R8G8B8 ? "X8R8G8B8" : f == D3DFMT_UNKNOWN ? "UNKNOWN" : "?";
}

int main(int argc, char **argv)
{
    static const D3DFORMAT fmts[] = { D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, D3DFMT_UNKNOWN };
    D3D *(WINAPI *create)(UINT);
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    WNDCLASSA wc = { 0 };
    char name[64];
    HMODULE lib;
    HRESULT hr;
    D3D *d3d;
    DEV *dev;
    unsigned cls, vis, size, f, depth, vp;

    snprintf(name, sizeof name, NAME "_%s.LOG", argc > 1 ? argv[1] : "RUN");
    out = guest_log_open(name, "w");
    if (!out)
        out = stdout;
    say("%s %s: stdout is %s\n", NAME, argc > 1 ? argv[1] : "", GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR ? "a console" : "redirected");
    lib = LoadLibraryA(DXVER == 8 ? "d3d8.dll" : "d3d9.dll");
    create = lib ? (void *)GetProcAddress(lib, DXVER == 8 ? "Direct3DCreate8" : "Direct3DCreate9") : NULL;
    d3d = create ? create(D3D_SDK_VERSION) : NULL;
    if (!d3d) {
        say("no Direct3D %d\n", DXVER);
        return 1;
    }
    hr = D3D_(GetAdapterDisplayMode)(d3d, 0, &mode);
    say("display mode: 0x%08lx %ux%u format %u\n", hr, mode.Width, mode.Height, (unsigned)mode.Format);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = NAME;
    RegisterClassA(&wc);

    for (cls = 0; cls < 2; cls++)
        for (vis = 0; vis < 2; vis++)
            for (size = 0; size < 2; size++)
                for (f = 0; f < 3; f++)
                    for (depth = 0; depth < 2; depth++)
                        for (vp = 0; vp < 2; vp++) {
                            HWND w = CreateWindowA(cls ? NAME : "static", NAME,
                                                   WS_OVERLAPPEDWINDOW | (vis ? WS_VISIBLE : 0), 0, 0, 640, 480, NULL,
                                                   NULL, NULL, NULL);
                            memset(&pp, 0, sizeof pp);
                            pp.Windowed = TRUE;
                            pp.hDeviceWindow = w;
                            pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                            pp.BackBufferWidth = size ? 640 : 0;
                            pp.BackBufferHeight = size ? 480 : 0;
                            pp.BackBufferFormat = fmts[f];
                            pp.EnableAutoDepthStencil = depth;
                            pp.AutoDepthStencilFormat = depth ? D3DFMT_D24S8 : 0;
                            dev = NULL;
                            hr = D3D_(CreateDevice)(d3d, 0, D3DDEVTYPE_HAL, w,
                                                    vp ? D3DCREATE_HARDWARE_VERTEXPROCESSING
                                                       : D3DCREATE_SOFTWARE_VERTEXPROCESSING,
                                                    &pp, &dev);
                            say("%-6s %-7s %-7s %-8s %-5s %s: 0x%08lx\n", cls ? "own" : "static",
                                vis ? "visible" : "hidden", size ? "640x480" : "0x0", fmtname(fmts[f]),
                                depth ? "D24S8" : "none", vp ? "HWVP" : "SWVP", hr);
                            if (dev)
                                DEV_(Release)(dev);
                            DestroyWindow(w);
                        }
    D3D_(Release)(d3d);
    say("%s: done\n", NAME);
    return 0;
}
