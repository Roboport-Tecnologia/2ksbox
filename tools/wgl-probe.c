/*
 * The exact WGL sequence embed/mglcntx_embed.c's Windows
 * backend performs, without QEMU: an invisible window for a device
 * handle, a bootstrap context to reach the ARB entry points, an
 * ARB-chosen pixel format, a WGL_ARB_pbuffer standing in for the window,
 * a real context on it, a frame drawn and read back.
 *
 * It exists because that sequence is the one part of the Windows port
 * that cannot be checked by building it: whether a given driver will
 * hand out an offscreen pbuffer and render into it. Run it first on any
 * Windows machine that is going to run a Win98 guest with 3D. If this
 * fails there, `mesapt: no GL on this host` is what the guest will see,
 * and the reason is in this program's output rather than in a VM.
 *
 * The frame is the same one tools/embed-3d-test.c draws: green clear,
 * red quad over the top-left quadrant (GL's y is up, so the quad is
 * y > 0 and lands in the *top* rows after the bottom-up flip).
 *
 * Build: scripts/build-windows.sh exec, with MSVC (cl, static C runtime)
 *   in MSYS2's MINGW64 shell, against opengl32, gdi32 and user32
 * Run:  wgl-probe.exe [width height]
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <GL/gl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WGL_ARB_pixel_format / WGL_ARB_pbuffer / WGL_ARB_create_context, as
 * mglcntx_embed.c's shared layer spells them (they are the guest's own
 * constants, so they are copied here rather than pulled from a header
 * the mingw sysroot may or may not have). */
#define WGL_DRAW_TO_WINDOW_ARB      0x2001
#define WGL_ACCELERATION_ARB        0x2003
#define WGL_SUPPORT_OPENGL_ARB      0x2010
#define WGL_DOUBLE_BUFFER_ARB       0x2011
#define WGL_PIXEL_TYPE_ARB          0x2013
#define WGL_COLOR_BITS_ARB          0x2014
#define WGL_ALPHA_BITS_ARB          0x201B
#define WGL_DEPTH_BITS_ARB          0x2022
#define WGL_STENCIL_BITS_ARB        0x2023
#define WGL_FULL_ACCELERATION_ARB   0x2027
#define WGL_TYPE_RGBA_ARB           0x202B
#define WGL_DRAW_TO_PBUFFER_ARB     0x202D

/* mingw's GL/gl.h is OpenGL 1.1; these two are 1.2 (and are what the
 * readback in embed/mglcntx_embed.c uses). */
#ifndef GL_BGRA
#define GL_BGRA                     0x80E1
#endif
#ifndef GL_UNSIGNED_INT_8_8_8_8_REV
#define GL_UNSIGNED_INT_8_8_8_8_REV 0x8367
#endif

DECLARE_HANDLE(HPBUFFERARB);
typedef BOOL (WINAPI *PFNCHOOSEPF)(HDC, const int *, const FLOAT *, UINT, int *, UINT *);
typedef BOOL (WINAPI *PFNGETPFATTR)(HDC, int, int, UINT, const int *, int *);
typedef HPBUFFERARB (WINAPI *PFNCREATEPB)(HDC, int, int, int, const int *);
typedef HDC (WINAPI *PFNGETPBDC)(HPBUFFERARB);
typedef int (WINAPI *PFNRELPBDC)(HPBUFFERARB, HDC);
typedef BOOL (WINAPI *PFNDESTROYPB)(HPBUFFERARB);
typedef const char *(WINAPI *PFNEXTSTR)(HDC);

static PFNCHOOSEPF  ChoosePixelFormatARB;
static PFNGETPFATTR GetPixelFormatAttribivARB;
static PFNCREATEPB  CreatePbufferARB;
static PFNGETPBDC   GetPbufferDCARB;
static PFNRELPBDC   ReleasePbufferDCARB;
static PFNDESTROYPB DestroyPbufferARB;
static PFNEXTSTR    GetExtensionsStringARB;

#define FAIL(...) do { printf("wgl-probe: " __VA_ARGS__); \
                       printf(" (error 0x%08lx)\n", GetLastError()); return 1; } while (0)

int main(int argc, char **argv)
{
    int w = argc > 2 ? atoi(argv[1]) : 640;
    int h = argc > 2 ? atoi(argv[2]) : 480;

    /* 1. an invisible window, only ever a device handle */
    static const char cls[] = "2ksbox-wgl-probe";
    WNDCLASSEX wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = cls;
    RegisterClassEx(&wc);
    HWND wnd = CreateWindowEx(0, cls, cls, WS_POPUP, 0, 0, 1, 1,
                              NULL, NULL, wc.hInstance, NULL);
    if (!wnd) FAIL("CreateWindowEx");
    HDC dc = GetDC(wnd);
    if (!dc) FAIL("GetDC");

    PIXELFORMATDESCRIPTOR boot;
    memset(&boot, 0, sizeof(boot));
    boot.nSize = sizeof(boot);
    boot.nVersion = 1;
    boot.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    boot.iPixelType = PFD_TYPE_RGBA;
    boot.iLayerType = PFD_MAIN_PLANE;
    boot.cColorBits = 32;
    boot.cAlphaBits = 8;
    boot.cDepthBits = 24;
    boot.cStencilBits = 8;
    int bf = ChoosePixelFormat(dc, &boot);
    if (!bf || !SetPixelFormat(dc, bf, &boot)) FAIL("SetPixelFormat(%d)", bf);

    /* 2. a bootstrap context: WGL has no other way to reach the ARB
     *    entry points, and opengl32.dll exports none of them */
    HGLRC boot_rc = wglCreateContext(dc);
    if (!boot_rc) FAIL("wglCreateContext");
    if (!wglMakeCurrent(dc, boot_rc)) FAIL("wglMakeCurrent(bootstrap)");
    printf("GL %s / %s / %s\n", (const char *)glGetString(GL_VERSION),
           (const char *)glGetString(GL_VENDOR), (const char *)glGetString(GL_RENDERER));

#define GETPROC(v, t, n) do { v = (t)wglGetProcAddress(n); \
        if (!v) { printf("wgl-probe: %s missing: no offscreen GL here\n", n); return 1; } } while (0)
    GETPROC(ChoosePixelFormatARB, PFNCHOOSEPF, "wglChoosePixelFormatARB");
    GETPROC(GetPixelFormatAttribivARB, PFNGETPFATTR, "wglGetPixelFormatAttribivARB");
    GETPROC(CreatePbufferARB, PFNCREATEPB, "wglCreatePbufferARB");
    GETPROC(GetPbufferDCARB, PFNGETPBDC, "wglGetPbufferDCARB");
    GETPROC(ReleasePbufferDCARB, PFNRELPBDC, "wglReleasePbufferDCARB");
    GETPROC(DestroyPbufferARB, PFNDESTROYPB, "wglDestroyPbufferARB");
#undef GETPROC
    GetExtensionsStringARB = (PFNEXTSTR)wglGetProcAddress("wglGetExtensionsStringARB");
    if (GetExtensionsStringARB) {
        const char *e = GetExtensionsStringARB(dc);
        printf("WGL_ARB_pbuffer %s, WGL_ARB_create_context %s\n",
               e && strstr(e, "WGL_ARB_pbuffer") ? "yes" : "NO",
               e && strstr(e, "WGL_ARB_create_context") ? "yes" : "no");
    }

    /* 3. a pixel format that can be drawn to as a pbuffer */
    const int ia[] = {
        WGL_DRAW_TO_WINDOW_ARB, 1,
        WGL_DRAW_TO_PBUFFER_ARB, 1,
        WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
        WGL_SUPPORT_OPENGL_ARB, 1,
        WGL_DOUBLE_BUFFER_ARB, 1,
        WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
        WGL_COLOR_BITS_ARB, 32,
        WGL_ALPHA_BITS_ARB, 8,
        WGL_DEPTH_BITS_ARB, 24,
        WGL_STENCIL_BITS_ARB, 8,
        0, 0
    };
    int fmt = 0;
    UINT count = 0;
    if (!ChoosePixelFormatARB(dc, ia, NULL, 1, &fmt, &count) || count < 1)
        FAIL("wglChoosePixelFormatARB found no pbuffer-capable format");
    const int q[] = { WGL_ALPHA_BITS_ARB, WGL_DEPTH_BITS_ARB, WGL_STENCIL_BITS_ARB };
    int v[3] = { 0 };
    GetPixelFormatAttribivARB(dc, fmt, 0, 3, q, v);
    printf("pixel format %d: alpha %d depth %d stencil %d\n", fmt, v[0], v[1], v[2]);

    /* 4. the pbuffer that stands in for a window, and a context on it */
    const int none[] = { 0 };
    HPBUFFERARB pb = CreatePbufferARB(dc, fmt, w, h, none);
    if (!pb) FAIL("wglCreatePbufferARB(%dx%d)", w, h);
    HDC pbdc = GetPbufferDCARB(pb);
    if (!pbdc) FAIL("wglGetPbufferDCARB");
    HGLRC rc = wglCreateContext(pbdc);
    if (!rc) FAIL("wglCreateContext(pbuffer)");
    if (!wglMakeCurrent(pbdc, rc)) FAIL("wglMakeCurrent(pbuffer)");

    /* 5. the frame embed-3d-test draws, and the same check */
    glViewport(0, 0, w, h);
    glClearColor(0.f, 1.f, 0.f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT);
    glColor3f(1.f, 0.f, 0.f);
    glBegin(GL_QUADS);
    glVertex2f(-1.f, 0.f);
    glVertex2f(0.f, 0.f);
    glVertex2f(0.f, 1.f);
    glVertex2f(-1.f, 1.f);
    glEnd();
    glFinish();

    uint32_t *px = malloc((size_t)w * h * 4);
    if (!px) FAIL("malloc");
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        printf("wgl-probe: glReadPixels: 0x%x\n", e);
        return 1;
    }
    /* bottom-up: row 0 is the bottom of the image, so the red quad (top
     * half in GL) is at high row numbers here */
    uint32_t tl = px[(size_t)(h - 10) * w + 10];
    uint32_t br = px[(size_t)10 * w + (w - 10)];
    uint32_t c  = px[(size_t)(h / 2) * w + (w / 2)];
    int ok = (tl & 0xffffff) == 0xff0000
          && (br & 0xffffff) == 0x00ff00
          && (c & 0xffffff) == 0x00ff00;
    printf("frame %dx%d  top-left %08x  bottom-right %08x  centre %08x -> %s\n",
           w, h, tl, br, c, ok ? "OK" : "FAIL");

    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleasePbufferDCARB(pb, pbdc);
    DestroyPbufferARB(pb);
    wglDeleteContext(boot_rc);
    ReleaseDC(wnd, dc);
    DestroyWindow(wnd);
    return ok ? 0 : 1;
}
