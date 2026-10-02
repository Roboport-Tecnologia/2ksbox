/*
 * QEMU MESA GL Pass-Through: window-less context backend for the embed
 * library (2ksbox, M3; docs/12-m3-context-provider.md).
 *
 * Port of hw/mesa/mglcntx_linux.c (GLX on an X11 window) to run without a
 * window or display server. Everything the guest wrapper needs (WGL
 * pixel formats, extension string, context levels, pbuffer emulation,
 * gamma/swap-interval stubs) is shared; a small platform layer provides
 * the context and the drawable that stands in for the window:
 *
 *   Linux: EGL on the surfaceless platform, pbuffer surface = FBO 0.
 *   macOS: CGL context with no drawable; an FBO (renderbuffers) plays the
 *            default framebuffer and framebuffer binding 0 is redirected to
 *            it in the dispatch table (MesaGLSetFunc, patch 32), since CGL
 *            pbuffers are deprecated and no window exists.
 *
 * All GL runs on the vCPU thread under the BQL exactly as upstream. Each
 * swap hands the presented frame to the frontend (embedfx.c →
 * libqemu_embed.c on_3d_frame), zero-copy as a dma-buf / IOSurface where
 * the host has one and read back otherwise.
 *
 * The native backends in hw/mesa are linked weak (patch 31) so these
 * definitions take over in libqemu-embed while qemu-system keeps its own.
 *
 * Copyright (c) 2020 (upstream backend)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#ifdef CONFIG_OPENGL
/* before mesagl_impl.h's own khronos headers, as ui/console.h did up to
 * QEMU 10.2 (cc47123440 took epoxy out of ui/surface.h) */
#include <epoxy/gl.h>
#endif
#include "ui/console.h"
#include "hw/mesa/mesagl_impl.h"
#include "embedfx.h"

#define DPRINTF(fmt, ...) \
    do { fprintf(stderr, "glcntx: " fmt "\n" , ## __VA_ARGS__); } while(0)
#define DPRINTF_COND(cond,fmt, ...) \
    if (cond) { fprintf(stderr, "glcntx: " fmt "\n" , ## __VA_ARGS__); }

#if defined(CONFIG_LINUX) || defined(CONFIG_DARWIN) || defined(CONFIG_WIN32)

/* ----------------------------------------------------- shared WGL layer */

#define GL_CONTEXTALPHA 1

/* The guest speaks WGL, so the layer below is written in WGL's own types
 * and constants. Linux and macOS have no windows.h and get them here;
 * Windows has the real ones (identical by construction, this struct is
 * the ABI the guest passes) and must not see a second definition. */
#ifndef CONFIG_WIN32
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef uint8_t BYTE;
typedef struct tagPIXELFORMATDESCRIPTOR {
  WORD  nSize;
  WORD  nVersion;
  DWORD dwFlags;
  BYTE  iPixelType;
  BYTE  cColorBits;
  BYTE  cRedBits;
  BYTE  cRedShift;
  BYTE  cGreenBits;
  BYTE  cGreenShift;
  BYTE  cBlueBits;
  BYTE  cBlueShift;
  BYTE  cAlphaBits;
  BYTE  cAlphaShift;
  BYTE  cAccumBits;
  BYTE  cAccumRedBits;
  BYTE  cAccumGreenBits;
  BYTE  cAccumBlueBits;
  BYTE  cAccumAlphaBits;
  BYTE  cDepthBits;
  BYTE  cStencilBits;
  BYTE  cAuxBuffers;
  BYTE  iLayerType;
  BYTE  bReserved;
  DWORD dwLayerMask;
  DWORD dwVisibleMask;
  DWORD dwDamageMask;
} PIXELFORMATDESCRIPTOR;
#endif /* !CONFIG_WIN32 */

#define WGL_NUMBER_PIXEL_FORMATS_ARB            0x2000
#define WGL_DRAW_TO_WINDOW_ARB                  0x2001
#define WGL_ACCELERATION_ARB                    0x2003
#define WGL_SWAP_METHOD_ARB                     0x2007
#define WGL_SUPPORT_OPENGL_ARB                  0x2010
#define WGL_DOUBLE_BUFFER_ARB                   0x2011
#define WGL_PIXEL_TYPE_ARB                      0x2013
#define WGL_COLOR_BITS_ARB                      0x2014
#define WGL_RED_BITS_ARB                        0x2015
#define WGL_RED_SHIFT_ARB                       0x2016
#define WGL_GREEN_BITS_ARB                      0x2017
#define WGL_GREEN_SHIFT_ARB                     0x2018
#define WGL_BLUE_BITS_ARB                       0x2019
#define WGL_BLUE_SHIFT_ARB                      0x201A
#define WGL_ALPHA_BITS_ARB                      0x201B
#define WGL_ALPHA_SHIFT_ARB                     0x201C
#define WGL_DEPTH_BITS_ARB                      0x2022
#define WGL_STENCIL_BITS_ARB                    0x2023
#define WGL_AUX_BUFFERS_ARB                     0x2024
#define WGL_FULL_ACCELERATION_ARB               0x2027
#define WGL_SWAP_EXCHANGE_ARB                   0x2028
#define WGL_TYPE_RGBA_ARB                       0x202B
#define WGL_DRAW_TO_PBUFFER_ARB                 0x202D
#define WGL_PBUFFER_WIDTH_ARB                   0x2034
#define WGL_PBUFFER_HEIGHT_ARB                  0x2035
#define WGL_SAMPLE_BUFFERS_ARB                  0x2041
#define WGL_SAMPLES_ARB                         0x2042
/* WGL_ARB_create_context(_profile): same values as GLX_*_ARB */
#define WGL_CONTEXT_MAJOR_VERSION_ARB           0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB           0x2092
#define WGL_CONTEXT_FLAGS_ARB                   0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB            0x9126
#define WGL_CONTEXT_DEBUG_BIT_ARB               0x0001
#define WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB  0x0002
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB        0x0001
/* WGL_ARB_render_texture / WGL_NV_render_texture_rectangle */
#define WGL_TEXTURE_FORMAT_ARB                  0x2072
#define WGL_TEXTURE_TARGET_ARB                  0x2073
#define WGL_TEXTURE_RGB_ARB                     0x2075
#define WGL_TEXTURE_RGBA_ARB                    0x2076
#define WGL_TEXTURE_2D_ARB                      0x207A
#define WGL_MIPMAP_LEVEL_ARB                    0x207B
#define WGL_TEXTURE_RECTANGLE_NV                0x20A2

/* What the guest asked for in wglCreatePbufferARB, per slot. Not a
 * handle: the platform layer below keeps the real object. Named for what
 * it is, because on Windows HPBUFFERARB is wglext.h's own handle type. */
typedef struct tagFakePBuffer {
    int width;
    int height;
    int target, format, level;
} FakePBuffer;

static const PIXELFORMATDESCRIPTOR pfd = {
    .nSize = sizeof(PIXELFORMATDESCRIPTOR),
    .nVersion = 1,
    .dwFlags = 0x225,
    .cColorBits = 32,
    .cRedBits = 8, .cGreenBits = 8, .cBlueBits = 8, .cAlphaBits = 8,
    .cRedShift = 16, .cGreenShift = 8, .cBlueShift = 0, .cAlphaShift = 24,
    .cDepthBits = 24,
    .cStencilBits = 8,
    .cAuxBuffers = 0,
};
static const int iAttribs[] = {
    WGL_NUMBER_PIXEL_FORMATS_ARB, 1,
    WGL_DRAW_TO_WINDOW_ARB, 1,
    WGL_ACCELERATION_ARB, WGL_FULL_ACCELERATION_ARB,
    WGL_SWAP_METHOD_ARB, WGL_SWAP_EXCHANGE_ARB,
    WGL_SUPPORT_OPENGL_ARB, 1,
    WGL_DOUBLE_BUFFER_ARB, 1,
    WGL_PIXEL_TYPE_ARB, WGL_TYPE_RGBA_ARB,
    WGL_COLOR_BITS_ARB, 32,
    WGL_RED_BITS_ARB, 8,
    WGL_RED_SHIFT_ARB, 16,
    WGL_GREEN_BITS_ARB, 8,
    WGL_GREEN_SHIFT_ARB, 8,
    WGL_BLUE_BITS_ARB, 8,
    WGL_BLUE_SHIFT_ARB, 0,
    WGL_ALPHA_BITS_ARB, 8,
    WGL_ALPHA_SHIFT_ARB, 24,
    WGL_DEPTH_BITS_ARB, 24,
    WGL_STENCIL_BITS_ARB, 8,
    WGL_AUX_BUFFERS_ARB, 0,
    WGL_SAMPLE_BUFFERS_ARB, 0,
    WGL_SAMPLES_ARB, 0,
    0,0
};

/* platform-independent state */
static void        *ctx[MAX_LVLCNTX];      /* context levels (0 = main) */
static int          win_w, win_h;           /* drawable = guest 2D surface */
static int          win_ready;              /* provider said go */
static int          wnd_ready;              /* handshake flag for the guest */
static const char  *xstr, *xcstr;
static FakePBuffer  hPbuffer[MAX_PBUFFER];
static int          cAlphaBits, cDepthBits, cStencilBits;
static int          cAuxBuffers, cSampleBuf[2];
static int          swap_interval = 1;
static uint32_t    *readback;               /* win_w * win_h, bottom-up */
static struct wgamma { uint16_t r[0x100], g[0x100], b[0x100]; } gamma_ramp;

/* platform layer (one implementation per OS below) */
static int   plat_open(void);                          /* library / display */
static int   plat_choose(int msaa);                    /* fills c*Bits */
static void *plat_ctx_create(void *share, const int *wgl_attribs);
static void  plat_ctx_destroy(void *c);
static void  plat_unbind(void);
static int   plat_make_current(void *c);               /* + main drawable */
static void *plat_get_current(void);
static int   plat_drawable(int w, int h);              /* (re)size FBO 0 */
static void  plat_drawable_release(void);
static int   plat_readback(uint32_t *dst);             /* FBO 0, bottom-up; 0 = no frame */
static int   plat_present_zero_copy(void);             /* 1 = frame handed off as dma-buf */
static int   plat_pbuffer_create(int i, int w, int h);
static void  plat_pbuffer_destroy(int i);
static void  plat_pbuffer_current(int i);
static void  plat_pbuffer_bind_tex(int i, int gl_target, int gl_format);
static void *plat_get_proc(const char *name);
static void  plat_set_func_ptr(void *hdll);
/* is <binding> (a framebuffer-binding query) the drawable that plays the
 * guest's framebuffer 0, and what does a colour buffer of it become? */
static int   plat_default_fb(unsigned binding, unsigned *target);

/* guest 2D surface size = drawable size (the SDL window follows it too) */
static void guest_size(int *w, int *h)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);
    DisplaySurface *s = con ? qemu_console_surface(con) : NULL;
    *w = s ? surface_width(s) : 640;
    *h = s ? surface_height(s) : 480;
}

int embed_gl_available(void)
{
    return plat_open();
}

void embed_gl_drawable_size(int *w, int *h)
{
    *w = win_w ? win_w : 640;
    *h = win_h ? win_h : 480;
}

static int drawable_resize(int w, int h)
{
    if (win_ready && win_w == w && win_h == h) {
        return 1;
    }
    if (!plat_drawable(w, h)) {
        return 0;
    }
    win_w = w;
    win_h = h;
    readback = g_realloc(readback, (size_t)w * h * sizeof(uint32_t));
    DPRINTF("drawable %dx%d", w, h);
    return 1;
}

/* ------------------------------------------------------------ backend API */

int glwnd_ready(void) { return qatomic_read(&wnd_ready); }

int MGLExtIsAvail(const char *xstr_, const char *str)
{ return find_xstr(xstr_, str); }

static void cwnd_mesagl(void *swnd, void *nwnd, void *opaque)
{
    win_ready = nwnd ? 1 : 0;
    DPRINTF("MESAGL drawable %s", win_ready ? "ready" : "refused");
    qatomic_set(&wnd_ready, 1);
}

static void TmpContextPurge(void)
{
    int n;
    for (n = MAX_LVLCNTX; ((n > 1) && !ctx[--n]););
    if ((n == 1) && ctx[--n]) {
        plat_ctx_destroy(ctx[n]);
        DPRINTF("MESAGL curr %d cntx [%p] purge %d", n, ctx[n], 1);
        ctx[n] = 0;
    }
}

void SetMesaFuncPtr(void *p)
{
    plat_set_func_ptr(p);
}

void *MesaGLGetProc(const char *proc)
{
    return plat_get_proc(proc);
}

void MGLTmpContext(void)
{
    plat_open();
}

void MGLDeleteContext(int level)
{
    int n = (level)? ((level % MAX_LVLCNTX)? (level % MAX_LVLCNTX):1):level;
    plat_unbind();
    if (n) {
        plat_ctx_destroy(ctx[n]);
        ctx[n] = 0;
    }
    else {
        for (int i = MAX_LVLCNTX; i > 1;) {
            if (ctx[--i]) {
                plat_ctx_destroy(ctx[i]);
                ctx[i] = 0;
            }
        }
        MesaBlitFree();
        MGLActivateHandler(0, 0);
    }
}

void MGLWndRelease(void)
{
    if (win_ready) {
        plat_unbind();
        if (ctx[0]) {
            plat_ctx_destroy(ctx[0]);
        }
        plat_drawable_release();
        mesa_release_window();
        CompareAttribArray(NULL);
        ctx[0] = 0;
        win_w = win_h = 0;
        win_ready = 0;
    }
}

int MGLCreateContext(uint32_t gDC)
{
    int i, ret;
    if (!win_ready) {
        DPRINTF("no drawable: GL context refused");
        return 1;
    }
    i = gDC & (MAX_PBUFFER - 1);
    if (gDC == ((MESAGL_HPBDC & 0xFFFFFFF0U) | i)) {
        ret = 0;
    }
    else {
        plat_unbind();
        for (i = MAX_LVLCNTX; i > 1;) {
            if (ctx[--i]) {
                plat_ctx_destroy(ctx[i]);
                ctx[i] = 0;
            }
        }
        if (!ctx[0])
            ctx[0] = plat_ctx_create(NULL, NULL);
        ret = (ctx[0])? 0:1;
    }
    return ret;
}

int MGLMakeCurrent(uint32_t cntxRC, int level)
{
    if (!win_ready)
        return 0;
    int n = (level)? ((level % MAX_LVLCNTX)? (level % MAX_LVLCNTX):1):level;
    uint32_t i = cntxRC & (MAX_PBUFFER - 1);
    if (cntxRC == (MESAGL_MAGIC - n)) {
        int w, h;
        guest_size(&w, &h);
        drawable_resize(w, h);
        plat_make_current(ctx[n]);
        InitMesaGLExt();
        wrContextSRGB(ContextUseSRGB());
        if (!n)
            MGLActivateHandler(1, 0);
    }
    if (cntxRC == (((MESAGL_MAGIC & 0xFFFFFFFU) << 4) | i))
        plat_pbuffer_current(i);

    return 0;
}

/*
 * Hand the current FBO 0 to the frontend: zero-copy into a dma-buf /
 * IOSurface where available, else read it back (bottom-up). Called on every
 * swap and, for front-buffer rendering, on glFlush/glFinish (see the
 * glDrawBuffer hook): WineD3D's ddraw presents by drawing the primary
 * surface into GL_FRONT and flushing, it never calls SwapBuffers.
 */
static void publish_frame(void)
{
    if (win_ready && readback
        && !plat_present_zero_copy()
        && plat_readback(readback)) {
        embed_fx_frame(readback, win_w, win_h, win_w * 4, /* bottom_up */ 1);
    }
}

static void present_frame(void)
{
    int w, h;

    if (plat_get_current() == ctx[0]) {
        publish_frame();
    }
    /* size follows the guest's 2D mode for the next frame (a fullscreen
     * game switches to 640x480 while the stand-in was the desktop's size) */
    guest_size(&w, &h);
    if (w != win_w || h != win_h) {
        drawable_resize(w, h);
    }
}

int MGLSwapBuffers(void)
{
    MGLActivateHandler(1, 0);
    MesaBlitScale();
    present_frame();
    return 1;
}

/*
 * Front-buffer presentation.
 *
 * A window has front and back buffers; the drawable that stands in for one
 * here may have only the back (a pbuffer) or be an FBO with a single colour
 * attachment (macOS). A GL_FRONT or GL_BACK variant selected while the
 * guest's framebuffer 0 is bound therefore becomes whatever plays that
 * buffer here. Passing GL_FRONT through is GL_INVALID_OPERATION on both,
 * which is what WineD3D got. And while the front buffer is the selected draw
 * buffer, glFlush/glFinish present the frame, since that is the moment a real
 * window would show it: ddraw's primary-surface path draws into GL_FRONT and
 * flushes, it never swaps, so nothing else would ever publish that frame.
 *
 * The hooks sit in the guest's dispatch table (MesaGLSetFunc), so only the
 * guest's own calls take them; ours below go straight to GL. front_selected
 * shadows a per-context piece of GL state with one variable, which at worst
 * publishes a frame for a flush from another context: a cost, not a wrong
 * picture.
 */
static void (*real_draw_buffer)(unsigned);
static void (*real_read_buffer)(unsigned);
static void (*real_flush)(void);
static void (*real_finish)(void);
static int front_selected;

static unsigned buffer_target(unsigned buf, unsigned binding, int *front)
{
    unsigned target;

    switch (buf) {
    case GL_FRONT: case GL_FRONT_LEFT: case GL_FRONT_AND_BACK: case GL_LEFT:
    case GL_BACK: case GL_BACK_LEFT:
        if (plat_default_fb(binding, &target)) {
            if (front) {
                *front = (buf != GL_BACK && buf != GL_BACK_LEFT);
            }
            return target;
        }
        break;
    default:
        break;
    }
    return buf;
}
static void fx_glDrawBuffer(unsigned buf)
{
    real_draw_buffer(buffer_target(buf, GL_FRAMEBUFFER_BINDING, &front_selected));
}
static void fx_glReadBuffer(unsigned buf)
{
    real_read_buffer(buffer_target(buf, GL_READ_FRAMEBUFFER_BINDING, NULL));
}
static void fx_glFlush(void)
{
    real_flush();
    if (front_selected) {
        present_frame();
    }
}
static void fx_glFinish(void)
{
    real_finish();
    if (front_selected) {
        present_frame();
    }
}

/* Re-applied after every InitMesaGL(), which reloads the dispatch table. */
static void install_buffer_hooks(void)
{
#define HOOK(var, fenum, fn, name) do { \
        var = MesaGLSetFunc(fenum, (void *)fn); \
        if (!var || var == (void *)fn) var = plat_get_proc(name); \
    } while (0)
    HOOK(real_draw_buffer, FEnum_glDrawBuffer, fx_glDrawBuffer, "glDrawBuffer");
    HOOK(real_read_buffer, FEnum_glReadBuffer, fx_glReadBuffer, "glReadBuffer");
    HOOK(real_flush, FEnum_glFlush, fx_glFlush, "glFlush");
    HOOK(real_finish, FEnum_glFinish, fx_glFinish, "glFinish");
#undef HOOK
    front_selected = 0;
    DPRINTF("draw/read buffer + flush hooks installed (%p %p %p %p)", (void *)real_draw_buffer,
            (void *)real_read_buffer, (void *)real_flush, (void *)real_finish);
}


static int MGLPresetPixelFormat(void)
{
    qatomic_set(&wnd_ready, 0);
    ImplMesaGLReset();
    if (!plat_open() || !plat_choose(GetContextMSAA())) {
        if (GetContextMSAA() && plat_choose(0)) {
            DPRINTF("MSAA %d unavailable, using no MSAA", GetContextMSAA());
        } else {
            DPRINTF("no usable GL configuration");
            cwnd_mesagl(NULL, NULL, NULL);
            return 1;
        }
    }
    /* the provider decides whether a drawable may exist (it calls cwnd) */
    mesa_prepare_window(GetContextMSAA(), GL_CONTEXTALPHA, 0, &cwnd_mesagl);
    if (!win_ready) {
        return 1;
    }
    int w, h;
    guest_size(&w, &h);
    win_w = win_h = 0;
    drawable_resize(w, h);
    for (int i = 0; i < 0x100; i++) {
        gamma_ramp.r[i] = gamma_ramp.g[i] = gamma_ramp.b[i] = (i << 8) | i;
    }
    DPRINTF("config depth %d stencil %d nSamples %d %d %s",
        cDepthBits, cStencilBits, cSampleBuf[0], cSampleBuf[1],
        ContextUseSRGB()? "sRGB":"");
    return 1;
}

int MGLChoosePixelFormat(void)
{
    DPRINTF("ChoosePixelFormat()");
    if (!win_ready)
        return MGLPresetPixelFormat();
    return 1;
}

int MGLSetPixelFormat(int fmt, const void *p)
{
    int ret;
    ret = (!win_ready)? MGLPresetPixelFormat():1;
    TmpContextPurge();
    DPRINTF("SetPixelFormat() ret %d", ret);
    return ret;
}

int MGLDescribePixelFormat(int fmt, unsigned int sz, void *p)
{
    if (!win_ready)
        MGLPresetPixelFormat();
    memcpy(p, &pfd, sizeof(PIXELFORMATDESCRIPTOR));
    ((PIXELFORMATDESCRIPTOR *)p)->cDepthBits = cDepthBits;
    ((PIXELFORMATDESCRIPTOR *)p)->cStencilBits = cStencilBits;
    ((PIXELFORMATDESCRIPTOR *)p)->cAuxBuffers = cAuxBuffers;
    return 1;
}

int NumPbuffer(void)
{
    int i, c;
    for (i = 0, c = 0; i < MAX_PBUFFER;)
        if (hPbuffer[i++].width) c++;
    return c;
}

int DrawableContext(void)
{
    return (ctx[0] == plat_get_current());
}

static int PbufferGLBinding(const int target)
{
    switch (target) {
        case WGL_TEXTURE_2D_ARB:        return 0x8069; /* GL_TEXTURE_BINDING_2D */
        case WGL_TEXTURE_RECTANGLE_NV:  return 0x84F6; /* GL_TEXTURE_BINDING_RECTANGLE */
        default:                        return 0;
    }
}
static int PbufferGLAttrib(const int attr)
{
    switch (attr) {
        case WGL_TEXTURE_2D_ARB:        return 0x0DE1; /* GL_TEXTURE_2D */
        case WGL_TEXTURE_RECTANGLE_NV:  return 0x84F5; /* GL_TEXTURE_RECTANGLE */
        case WGL_TEXTURE_RGB_ARB:       return 0x1907; /* GL_RGB */
        case WGL_TEXTURE_RGBA_ARB:      return 0x1908; /* GL_RGBA */
        default:                        return 0;
    }
}
static int LookupAttribArray(const int *attrib, const int attr)
{
    int ret = 0;
    for (int i = 0; attrib[i]; i+=2) {
        if (attrib[i] == attr) {
            switch (attr) {
                case WGL_DEPTH_BITS_ARB:    ret = cDepthBits;    break;
                case WGL_STENCIL_BITS_ARB:  ret = cStencilBits;  break;
                case WGL_AUX_BUFFERS_ARB:   ret = cAuxBuffers;   break;
                case WGL_SAMPLE_BUFFERS_ARB: ret = cSampleBuf[0]; break;
                case WGL_SAMPLES_ARB:       ret = cSampleBuf[1]; break;
                default:                    ret = attrib[i+1];   break;
            }
            break;
        }
    }
    return ret;
}

void MGLFuncHandler(const char *name)
{
    char fname[64];
    uint32_t *argsp = (uint32_t *)(name + ALIGNED((strnlen(name, sizeof(fname))+1)));
    strncpy(fname, name, sizeof(fname)-1);

#define FUNCP_HANDLER(a) \
    if (!memcmp(fname, a, sizeof(a)))

    FUNCP_HANDLER("wglShareLists") {
        uint32_t i, ret = 0;
        i = argsp[1] & (MAX_PBUFFER - 1);
        if ((argsp[0] == MESAGL_MAGIC) && (argsp[1] == ((MESAGL_MAGIC & 0xFFFFFFFU) << 4 | i)))
            ret = 1;
        else {
            DPRINTF("  *WARN* ShareLists called with unknown contexts, %x %x", argsp[0], argsp[1]);
        }
        argsp[0] = ret;
        return;
    }
    FUNCP_HANDLER("wglUseFontBitmapsA") {
        /* no X font path without a display; the guest falls back */
        argsp[0] = 0;
        return;
    }
    FUNCP_HANDLER("wglSwapIntervalEXT") {
        /* the frontend paces presentation; remember the guest's wish */
        swap_interval = argsp[0];
        DPRINTF("wglSwapIntervalEXT(%u) ret %-24u", argsp[0], 1);
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglGetSwapIntervalEXT") {
        argsp[0] = swap_interval;
        return;
    }
    FUNCP_HANDLER("wglGetExtensionsStringARB") {
        const char wglext[] = "WGL_3DFX_gamma_control "
            "WGL_ARB_create_context "
            "WGL_ARB_create_context_profile "
            "WGL_ARB_extensions_string "
            "WGL_ARB_multisample "
            "WGL_ARB_pixel_format "
            "WGL_ARB_pbuffer WGL_ARB_render_texture WGL_NV_render_texture_rectangle "
            "WGL_EXT_extensions_string "
            "WGL_EXT_swap_control "
            ;
        memcpy((char *)name, wglext, sizeof(wglext));
        *((char *)name + sizeof(wglext) - 2) = '\0';
        return;
    }
    FUNCP_HANDLER("wglCreateContextAttribsARB") {
        uint32_t i, ret;
        const int *attribs = (const int *)&argsp[2];
        for (i = 0; ((i < MAX_LVLCNTX) && ctx[i]); i++);
        argsp[1] = (argsp[0])? i:0;
        if (argsp[1] == 0) {
            plat_unbind();
            if (CompareAttribArray(attribs)) {
                for (i = MAX_LVLCNTX; i > 0;) {
                    if (ctx[--i]) {
                        plat_ctx_destroy(ctx[i]);
                        ctx[i] = 0;
                    }
                }
                MGLActivateHandler(0, 0);
                ctx[0] = plat_ctx_create(NULL, attribs);
            }
            ret = (ctx[0])? 1:0;
        }
        else {
            if (i == MAX_LVLCNTX) {
                plat_ctx_destroy(ctx[1]);
                for (i = 1; i < (MAX_LVLCNTX - 1); i++)
                    ctx[i] = ctx[i + 1];
                argsp[1] = i;
            }
            ctx[i] = plat_ctx_create(ctx[i-1], attribs);
            ret = (ctx[i])? 1:0;
        }
        argsp[0] = ret;
        return;
    }
    FUNCP_HANDLER("wglGetPixelFormatAttribfvARB") {
        const int *ia = (const int *)&argsp[4], n = argsp[2];
        float pf[64];
        for (int i = 0; i < n; i++)
            pf[i] = (float)LookupAttribArray(iAttribs, ia[i]);
        memcpy(&argsp[2], pf, n*sizeof(float));
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglGetPixelFormatAttribivARB") {
        const int *ia = (const int *)&argsp[4], n = argsp[2];
        int pi[64];
        for (int i = 0; i < n; i++)
            pi[i] = LookupAttribArray(iAttribs, ia[i]);
        memcpy(&argsp[2], pi, n*sizeof(int));
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglChoosePixelFormatARB") {
        const int *ia = (const int *)argsp;
        if (LookupAttribArray(ia, WGL_DRAW_TO_PBUFFER_ARB)) {
            argsp[1] = 0x02;
        }
        else {
            DPRINTF("%-32s", "wglChoosePixelFormatARB()");
            argsp[1] = MGLChoosePixelFormat();
        }
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglBindTexImageARB") {
        uint32_t i = argsp[0] & (MAX_PBUFFER - 1);
        if (PbufferGLBinding(hPbuffer[i].target) && PbufferGLAttrib(hPbuffer[i].format)) {
            plat_pbuffer_bind_tex(i, hPbuffer[i].target, hPbuffer[i].format);
        }
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglReleaseTexImageARB") {
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglCreatePbufferARB") {
        uint32_t i;
        for (i = 0; ((i < MAX_PBUFFER) && hPbuffer[i].width); i++);
        if (MAX_PBUFFER == i) {
            DPRINTF("MAX_PBUFFER reached %-24u", i);
            argsp[0] = 0;
            return;
        }
        hPbuffer[i].width = argsp[1];
        hPbuffer[i].height = argsp[2];
        const int *pattr = (const int *)&argsp[4];
        hPbuffer[i].target = LookupAttribArray(pattr, WGL_TEXTURE_TARGET_ARB);
        hPbuffer[i].format = LookupAttribArray(pattr, WGL_TEXTURE_FORMAT_ARB);
        hPbuffer[i].level = LookupAttribArray(pattr, WGL_MIPMAP_LEVEL_ARB);
        if (plat_pbuffer_create(i, hPbuffer[i].width, hPbuffer[i].height)) {
            argsp[0] = 1;
        } else {
            memset(&hPbuffer[i], 0, sizeof(FakePBuffer));
            argsp[0] = 0;
        }
        argsp[1] = i;
        return;
    }
    FUNCP_HANDLER("wglDestroyPbufferARB") {
        uint32_t i;
        i = argsp[0] & (MAX_PBUFFER - 1);
        plat_pbuffer_destroy(i);
        argsp[0] = 1;
        memset(&hPbuffer[i], 0, sizeof(FakePBuffer));
        return;
    }
    FUNCP_HANDLER("wglQueryPbufferARB") {
        uint32_t i = argsp[0] & (MAX_PBUFFER - 1);
        switch(argsp[1]) {
            case WGL_PBUFFER_WIDTH_ARB:   argsp[2] = hPbuffer[i].width;  break;
            case WGL_PBUFFER_HEIGHT_ARB:  argsp[2] = hPbuffer[i].height; break;
            case WGL_TEXTURE_TARGET_ARB:  argsp[2] = hPbuffer[i].target; break;
            case WGL_TEXTURE_FORMAT_ARB:  argsp[2] = hPbuffer[i].format; break;
            case WGL_MIPMAP_LEVEL_ARB:    argsp[2] = hPbuffer[i].level;  break;
            default:
                argsp[0] = 0;
                return;
        }
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglGetDeviceGammaRamp3DFX") {
        /* no display to apply it to: hand back what the guest last set */
        struct wgamma *wRamp = (struct wgamma *)&argsp[2];
        memcpy(wRamp, &gamma_ramp, sizeof(gamma_ramp));
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglSetDeviceGammaRamp3DFX") {
        struct wgamma *wRamp = (struct wgamma *)&argsp[0];
        memcpy(&gamma_ramp, wRamp, sizeof(gamma_ramp));
        argsp[0] = 1;
        return;
    }
    FUNCP_HANDLER("wglSetDeviceCursor3DFX") {
        return;
    }

    DPRINTF("  *WARN* Unhandled GLFunc %s", name);
    argsp[0] = 0;
}

#endif /* CONFIG_LINUX || CONFIG_DARWIN */

/* ============================================================== Linux */
#if defined(CONFIG_LINUX)
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <linux/version.h>
#include <sys/utsname.h>
#include <gbm.h>
#include "qemu/drm.h"
#include "standard-headers/drm/drm_fourcc.h"
#include "system/kvm.h"

static int bufo_accel_en(void)
{
    struct utsname buf;
    if (!uname(&buf)) {
        int major, patch, sub, i = sscanf(buf.release, "%d.%d.%d", &major, &patch, &sub);
        if (i == 3) {
            return (KERNEL_VERSION(major, patch, sub) >=
                    KERNEL_VERSION(6, 13, 0))? 1:0;
        }
    }
    return 0;
}
int MGLUpdateGuestBufo(mapbufo_t *bufo, const int add)
{
    int ret = (GetBufOAccelEN()
            || (bufo_accel_en() &&
                (bufo && bufo->tgt == GL_PIXEL_UNPACK_BUFFER))
            )? kvm_enabled():0;
    if (ret && bufo) {
        bufo->lvl = (add)? MapBufObjGpa(bufo):0;
        kvm_update_guest_pa_range(MBUFO_BASE | (bufo->gpa & ((MBUFO_SIZE - 1) - (qemu_real_host_page_size() - 1))),
            bufo->mapsz + (bufo->hva & (qemu_real_host_page_size() - 1)),
            (void *)(bufo->hva & qemu_real_host_page_mask()),
            (bufo->acc & GL_MAP_WRITE_BIT)? 0:1, add);
    }
    return ret;
}

static EGLDisplay   dpy = EGL_NO_DISPLAY;
static EGLConfig    cfg;
static EGLSurface   win = EGL_NO_SURFACE;     /* the pbuffer = FBO 0 */
static EGLSurface   PBDC[MAX_PBUFFER];
static EGLContext   PBRC[MAX_PBUFFER];

static const EGLint ctx_compat[] = {
    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT,
    EGL_NONE
};

/*
 * Zero-copy presentation (doc 12 §4): a ring of GBM buffers (linear,
 * ARGB8888) imported into GL as EGLImage-backed textures; each swap blits
 * FBO 0 into the next one (Y-flipped, so the buffer is top-down) and the
 * frontend, which imported the same dma-bufs into its own GPU API, samples
 * it. zc_state: -1 undecided, 0 off (readback), 1 on.
 */
#define ZC_SLOTS 3
static struct gbm_device *gbm_dev;
static int drm_fd = -1;
static int zc_state = -1;
static int zc_have_modifiers;
static struct {
    struct gbm_bo *bo;
    int fd;
    EGLImageKHR img;
    GLuint tex, fbo;
    int w, h;
    uint32_t stride;
    uint64_t modifier;
} zc[ZC_SLOTS];
static int zc_next;

static void zc_slot_free(int i)
{
    /*
     * epoxy resolves GL/EGL extension entry points through the current
     * context; teardown runs after MGLDeleteContext unbound it, so bind the
     * main context for the duration when nothing is current.
     */
    bool rebound = false;
    if (eglGetCurrentContext() == EGL_NO_CONTEXT && ctx[0] && win != EGL_NO_SURFACE) {
        rebound = eglMakeCurrent(dpy, win, win, (EGLContext)ctx[0]);
    }
    bool have_ctx = eglGetCurrentContext() != EGL_NO_CONTEXT;
    if (zc[i].fbo && have_ctx) {
        glDeleteFramebuffers(1, &zc[i].fbo);
    }
    if (zc[i].tex && have_ctx) {
        glDeleteTextures(1, &zc[i].tex);
    }
    if (zc[i].img && have_ctx) {
        eglDestroyImageKHR(dpy, zc[i].img);
    }
    if (rebound) {
        eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
    if (zc[i].fd >= 0) {
        close(zc[i].fd);
    }
    if (zc[i].bo) {
        gbm_bo_destroy(zc[i].bo);
    }
    memset(&zc[i], 0, sizeof(zc[i]));
    zc[i].fd = -1;
}

static void zc_off(const char *why)
{
    if (zc_state != 0) {
        DPRINTF("zero-copy off: %s (readback path)", why);
    }
    zc_state = 0;
    for (int i = 0; i < ZC_SLOTS; i++) {
        zc_slot_free(i);
    }
}

static int zc_init(void)
{
    if (zc_state >= 0) {
        return zc_state;
    }
    for (int i = 0; i < ZC_SLOTS; i++) {
        zc[i].fd = -1;
    }
    if (!epoxy_has_egl_extension(dpy, "EGL_EXT_image_dma_buf_import")
        || !epoxy_has_gl_extension("GL_OES_EGL_image")) {
        zc_off("EGL_EXT_image_dma_buf_import / GL_OES_EGL_image missing");
        return 0;
    }
    zc_have_modifiers = epoxy_has_egl_extension(dpy, "EGL_EXT_image_dma_buf_import_modifiers");
    drm_fd = qemu_drm_rendernode_open(NULL);
    if (drm_fd < 0) {
        zc_off("no DRM render node");
        return 0;
    }
    gbm_dev = gbm_create_device(drm_fd);
    if (!gbm_dev) {
        zc_off("gbm_create_device failed");
        return 0;
    }
    zc_state = 1;
    DPRINTF("zero-copy: GBM on %s, %s modifiers", gbm_device_get_backend_name(gbm_dev),
            zc_have_modifiers ? "with" : "without");
    return 1;
}

/* An integer knob from the environment, or `dflt`. */
static int zc_env(const char *name, int dflt)
{
    const char *e = getenv(name);
    int v = e ? atoi(e) : dflt;
    return v < 0 ? dflt : v;
}

/* (re)allocate slot i at w x h and offer it to the frontend */
static int zc_slot_ensure(int i, int w, int h)
{
    if (zc[i].bo && zc[i].w == w && zc[i].h == h) {
        return 1;
    }
    zc_slot_free(i);
    zc[i].bo = gbm_bo_create(gbm_dev, w, h, GBM_FORMAT_ARGB8888,
                             GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
    if (!zc[i].bo) {
        zc_off("gbm_bo_create failed");
        return 0;
    }
    zc[i].fd = gbm_bo_get_fd(zc[i].bo);
    zc[i].stride = gbm_bo_get_stride(zc[i].bo);
    zc[i].modifier = gbm_bo_get_modifier(zc[i].bo);
    if (zc[i].modifier == DRM_FORMAT_MOD_INVALID) {
        zc[i].modifier = DRM_FORMAT_MOD_LINEAR;   /* GBM_BO_USE_LINEAR */
    }
    zc[i].w = w;
    zc[i].h = h;
    if (zc[i].fd < 0) {
        zc_off("gbm_bo_get_fd failed");
        return 0;
    }
    EGLint attrs[32];
    int n = 0;
    attrs[n++] = EGL_WIDTH;                     attrs[n++] = w;
    attrs[n++] = EGL_HEIGHT;                    attrs[n++] = h;
    attrs[n++] = EGL_LINUX_DRM_FOURCC_EXT;      attrs[n++] = DRM_FORMAT_ARGB8888;
    attrs[n++] = EGL_DMA_BUF_PLANE0_FD_EXT;     attrs[n++] = zc[i].fd;
    attrs[n++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT; attrs[n++] = 0;
    attrs[n++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;  attrs[n++] = zc[i].stride;
    if (zc_have_modifiers && zc[i].modifier != DRM_FORMAT_MOD_INVALID) {
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT; attrs[n++] = (EGLint)(zc[i].modifier & 0xffffffffu);
        attrs[n++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT; attrs[n++] = (EGLint)(zc[i].modifier >> 32);
    }
    attrs[n] = EGL_NONE;
    zc[i].img = eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if (!zc[i].img) {
        DPRINTF("eglCreateImageKHR(dma-buf) failed 0x%x", eglGetError());
        zc_off("dma-buf import into GL failed");
        return 0;
    }
    GLint prev_tex = 0, prev_fbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_fbo);
    glGenTextures(1, &zc[i].tex);
    glBindTexture(GL_TEXTURE_2D, zc[i].tex);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, zc[i].img);
    /* What the driver actually made of the dma-buf. The frontend imports
     * every slot as one format from the fourcc we sent, so a slot whose
     * texture came back a different shape is a slot the two sides disagree
     * about, which shows up as one frozen picture in a turning ring. */
    {
        GLint ifmt = 0, a = -1, r = -1;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &ifmt);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_ALPHA_SIZE, &a);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_RED_SIZE, &r);
        DPRINTF("zc slot %d texture: internal 0x%x red %d alpha %d; bo format 0x%x bpp %u "
                "offset %u planes %d size %zu",
                i, ifmt, r, a, gbm_bo_get_format(zc[i].bo), gbm_bo_get_bpp(zc[i].bo),
                gbm_bo_get_offset(zc[i].bo, 0), gbm_bo_get_plane_count(zc[i].bo),
                (size_t)gbm_bo_get_stride(zc[i].bo) * h);
    }
    glBindTexture(GL_TEXTURE_2D, prev_tex);
    glGenFramebuffers(1, &zc[i].fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, zc[i].fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, zc[i].tex, 0);
    GLenum st = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_fbo);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        DPRINTF("dma-buf FBO incomplete 0x%x", st);
        zc_off("dma-buf FBO incomplete");
        return 0;
    }
    /*
     * The frontend gets an fd of its own rather than the one this side keeps.
     * Importing a dma-buf into Vulkan passes ownership: the frontend's
     * vkAllocateMemory closes it with the memory, while zc_slot_free closes
     * zc[i].fd too. One description, two owners, and the number goes back
     * to the process to hand to the next file it opens. A declined offer is
     * still ours to close.
     */
    int share_fd = gbm_bo_get_fd(zc[i].bo);
    if (share_fd < 0) {
        zc_off("gbm_bo_get_fd for the frontend failed");
        return 0;
    }
    if (!embed_fx_dmabuf(i, share_fd, w, h, zc[i].stride, DRM_FORMAT_ARGB8888, zc[i].modifier)) {
        close(share_fd);
        zc_off("frontend declined the dma-buf");
        return 0;
    }
    DPRINTF("zero-copy slot %d: %dx%d stride %u modifier 0x%llx fd %d", i, w, h,
            zc[i].stride, (unsigned long long)zc[i].modifier, zc[i].fd);
    /*
     * With EMBED_ZC_CHECK on, prove at once that this texture and this
     * buffer are the same memory: clear the one through GL and read the
     * other with the CPU. A slot that fails here was never written through
     * at all, and everything later blamed on the guest is the moment its
     * picture stopped being constant, which is the only moment the two
     * readings can be told apart.
     */
    if (zc_env("EMBED_ZC_CHECK", 0)) {
        GLint prev_draw = 0;
        uint32_t want = 0xff000000u | (uint32_t)(0x10 + i * 0x40) << 8;
        void *mapd = NULL;
        uint32_t mstride = 0, *cpu;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, zc[i].fbo);
        glClearColor(0.f, (float)(0x10 + i * 0x40) / 255.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
        glFinish();
        cpu = gbm_bo_map(zc[i].bo, 0, 0, w, h, GBM_BO_TRANSFER_READ, &mstride, &mapd);
        if (cpu) {
            uint32_t got = *(uint32_t *)((char *)cpu + (size_t)(h / 2) * mstride + (w / 2) * 4);
            DPRINTF("zc slot %d alias check: wrote %08x through GL, the buffer holds %08x -- %s",
                    i, want, got, (got == want) ? "same memory" : "DIFFERENT MEMORY");
            gbm_bo_unmap(zc[i].bo, mapd);
        }
    }
    /*
     * `EMBED_ZC_SETTLE=<ms>`: wait before using a slot just offered. Accepting
     * an offer only queues it (the frontend imports on its own thread, later),
     * so as it stands the first blits into a slot race whatever importing
     * it does. This says whether that race is what leaves a slot diverged.
     */
    {
        int ms = zc_env("EMBED_ZC_SETTLE", 0);
        if (ms) {
            g_usleep((gulong)ms * 1000);
        }
    }
    return 1;
}

/*
 * `EMBED_ZC_SLOTS=<n>`: rotate over the first n slots of the ring only. It
 * defaults to ZC_SLOTS_DEFAULT, which is 1 while the ring is stood down.
 * `EMBED_ZC_CHECK=<n>`: every n-th present, read four pixels back out of the
 * slot just blitted into and log them beside the slot number.
 *
 * Both exist for one failure the frame counters cannot show: a ring slot the
 * guest's frame stops reaching still gets published, so the picture flicks
 * between the live frame and a frozen one at a third of the frame rate while
 * every log says the ring is turning. The sample says whether the blit
 * landed on *this* side of the hand-off; a slot whose sample keeps changing
 * while the frontend shows it frozen puts the fault in the import instead.
 */
/*
 * The ring is stood down to one buffer, which is not what it is for.
 *
 * On this host the second buffer the ring allocates stops being written
 * through: GL reads back everything it blitted into it, while the dma-buf's
 * own memory (what the frontend imported and samples) keeps the frame it
 * held first. One publish in three is then a frozen picture, which on screen
 * is a fast flicker (GLQuake on the Win98 machine, and any other title that
 * presents through the GL pass-through). It needs the frontend to import the
 * buffers into Vulkan: the backend on its own writes through to all three,
 * which `embed-3d` now checks. Ring size, fd ownership and a settling delay
 * before first use all make no difference (doc 12 §4 has the measurements).
 *
 * With one buffer there is no second slot to go bad. What it gives up is the
 * margin the ring exists for: the frontend samples the buffer the next blit
 * is about to overwrite, and nothing but timing keeps them apart, since the
 * hand-off has no fence coming back. Measured on the reference workload that
 * costs nothing: 72 fps either way, and a tear detector that catches every
 * synthetic one-frame tear finds no more torn frames than the tear-free
 * readback path does. It is still a smaller machine, a heavier shader chain
 * or a faster guest away from mattering, so this is a stopgap: put it back to
 * ZC_SLOTS once a slot's write-through survives the import.
 */
#define ZC_SLOTS_DEFAULT ZC_SLOTS

static int zc_slots(void)
{
    static int n;
    if (!n) {
        n = zc_env("EMBED_ZC_SLOTS", ZC_SLOTS_DEFAULT);
        if (n < 1 || n > ZC_SLOTS) {
            n = ZC_SLOTS_DEFAULT;
        }
        DPRINTF("zero-copy ring: %d of %d slot(s)%s", n, ZC_SLOTS,
                (n == 1) ? " (one buffer: the ring is stood down)" : "");
    }
    return n;
}

/* Read four pixels out of the middle of the slot just written. */
static void zc_sample(int i)
{
    static int every = -1;
    static unsigned long long nth;
    uint32_t px[4] = { 0, 0, 0, 0 };
    GLint prev_read = 0;

    if (every < 0) {
        every = zc_env("EMBED_ZC_CHECK", 0);
    }
    if (!every || (nth++ % (unsigned)every)) {
        return;
    }
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, zc[i].fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(win_w / 2, win_h / 2, 2, 2, GL_BGRA, GL_UNSIGNED_BYTE, px);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    /* The same four pixels straight out of the buffer's own memory. GL
     * reading back what GL wrote proves only that GL is self-consistent: if
     * the driver kept a private copy behind the EGLImage, the dma-buf the
     * frontend imported never moves and the two lines disagree. That is the
     * whole question a frozen ring slot asks. */
    {
        uint32_t *cpu;
        void *mapd = NULL;
        uint32_t mstride = 0;
        cpu = gbm_bo_map(zc[i].bo, 0, 0, zc[i].w, zc[i].h, GBM_BO_TRANSFER_READ,
                         &mstride, &mapd);
        if (cpu) {
            uint32_t *row = (uint32_t *)((char *)cpu + (size_t)(win_h / 2) * mstride);
            DPRINTF("zc slot %d centre gl %08x %08x cpu %08x %08x", i, px[0], px[1],
                    row[win_w / 2], row[win_w / 2 + 1]);
            gbm_bo_unmap(zc[i].bo, mapd);
        } else {
            DPRINTF("zc slot %d centre gl %08x %08x cpu <map failed>", i, px[0], px[1]);
        }
    }
}

/*
 * `EMBED_ZC_PROBE=<n>`: every n-th present, before the blit, write a known
 * colour into the slot through GL and read the buffer's own memory back.
 * Unlike comparing the two readings of a frame, this cannot be fooled by a
 * picture that did not change, so it says exactly which present a slot stops
 * being the memory it was made over. The blit then paints over the probe, so
 * nothing shows on screen.
 */
/*
 * When to probe. A buffer that goes bad does so early (on the first frame
 * the guest renders in earnest, which is present 13 of GLQuake's every time)
 * and never recovers, so the schedule is dense while the ring is young
 * and occasional afterwards. `EMBED_ZC_PROBE=<n>` forces a rate, `=0` turns
 * it off.
 */
static int zc_probe_due(unsigned long long n)
{
    static int every = -2;

    if (every == -2) {
        every = zc_env("EMBED_ZC_PROBE", -1);
    }
    if (every == 0) {
        return 0;
    }
    if (every > 0) {
        return (n % (unsigned)every) == 0;
    }
    return (n < 64) || ((n % 512) == 0);
}

static void zc_probe(int i)
{
    static unsigned long long nth;
    static int broken[ZC_SLOTS];
    uint32_t want, got = 0;
    void *mapd = NULL;
    uint32_t mstride = 0, *cpu;
    GLint prev_draw = 0;
    unsigned long long n;

    n = nth++;
    if (!zc_probe_due(n)) {
        return;
    }
    DPRINTF_COND(zc_env("EMBED_ZC_MARK", 0), "zc present %llu slot %d",
                 (unsigned long long)n, i);
    /*
     * ARGB8888 in memory: A R G B. Red names the slot, green the present.
     *
     * Every piece of state this touches is put back, and the two that would
     * quietly make the clear do nothing (a scissor the guest left on, a
     * colour mask it left closed) are taken out of the way first. Leaving
     * the clear colour behind cost the `embed-3d` check a frame the moment
     * this was written: a guest that clears without setting the colour each
     * time would have got ours.
     */
    {
        uint32_t r = (uint32_t)(0x20 + i * 0x40), g = (uint32_t)((n / 16) & 0xff);
        GLfloat cc[4];
        GLboolean mask[4];
        GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
        want = 0xff000000u | (r << 16) | (g << 8);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, cc);
        glGetBooleanv(GL_COLOR_WRITEMASK, mask);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, zc[i].fbo);
        if (scissor) {
            glDisable(GL_SCISSOR_TEST);
        }
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glClearColor((float)r / 255.f, (float)g / 255.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(cc[0], cc[1], cc[2], cc[3]);
        glColorMask(mask[0], mask[1], mask[2], mask[3]);
        if (scissor) {
            glEnable(GL_SCISSOR_TEST);
        }
    }
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
    glFinish();
    /* one pixel, not the buffer: this runs beside a game */
    cpu = gbm_bo_map(zc[i].bo, zc[i].w / 2, zc[i].h / 2, 2, 2, GBM_BO_TRANSFER_READ,
                     &mstride, &mapd);
    if (!cpu) {
        return;
    }
    got = *cpu;
    gbm_bo_unmap(zc[i].bo, mapd);
    if (got != want && !broken[i]) {
        broken[i] = 1;
        DPRINTF("zc slot %d STOPPED ALIASING at present %llu: wrote %08x, buffer holds %08x",
                i, (unsigned long long)n, want, got);
        /* `EMBED_ZC_HEAL=1`: throw the buffer away and make the slot again.
         * If the new one holds, a slot going bad is something the ring can
         * recover from by itself rather than a reason to stand it down. */
        if (zc_env("EMBED_ZC_HEAL", 1)) {
            int w = zc[i].w, h = zc[i].h;
            zc_slot_free(i);
            DPRINTF("zc slot %d: remaking it", i);
            if (!zc_slot_ensure(i, w, h)) {
                DPRINTF("zc slot %d: remaking it failed", i);
            }
            broken[i] = 0;
        }
    } else if (got == want && broken[i]) {
        broken[i] = 0;
        DPRINTF("zc slot %d aliases its buffer again at present %llu", i, (unsigned long long)n);
    }
}

/* present FBO 0 into the next ring slot; 1 = handed off, 0 = use readback */
static int zc_present(void)
{
    if (zc_state < 0) {
        zc_init();
    }
    if (zc_state != 1) {
        return 0;
    }
    int i = zc_next;
    if (!zc_slot_ensure(i, win_w, win_h)) {
        return 0;
    }
    zc_probe(i);
    GLint prev_read = 0, prev_draw = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prev_draw);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, zc[i].fbo);
    glReadBuffer(GL_BACK);
    /* flip: GL's bottom-up FBO 0 becomes a top-down buffer */
    glBlitFramebuffer(0, 0, win_w, win_h, 0, win_h, win_w, 0,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw);
    /* bring-up sync: wait for the blit before publishing (fence fd later) */
    glFinish();
    zc_sample(i);
    embed_fx_frame_ready(i);
    zc_next = (i + 1) % zc_slots();
    return 1;
}

static int plat_open(void)
{
    if (dpy != EGL_NO_DISPLAY) {
        return 1;
    }
    if (!epoxy_has_egl_extension(EGL_NO_DISPLAY, "EGL_MESA_platform_surfaceless")) {
        DPRINTF("EGL_MESA_platform_surfaceless missing");
        return 0;
    }
    dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA,
                                   EGL_DEFAULT_DISPLAY, NULL);
    if (dpy == EGL_NO_DISPLAY) {
        DPRINTF("no surfaceless EGL display");
        return 0;
    }
    EGLint major, minor;
    if (!eglInitialize(dpy, &major, &minor)) {
        DPRINTF("eglInitialize failed 0x%x", eglGetError());
        dpy = EGL_NO_DISPLAY;
        return 0;
    }
    if (!eglBindAPI(EGL_OPENGL_API)) {
        DPRINTF("EGL_OPENGL_API not supported");
        eglTerminate(dpy);
        dpy = EGL_NO_DISPLAY;
        return 0;
    }
    xcstr = eglQueryString(dpy, EGL_VENDOR);
    xstr = eglQueryString(dpy, EGL_EXTENSIONS);
    DPRINTF("EGL %d.%d %s (surfaceless)", major, minor, xcstr);
    return 1;
}

static int plat_choose(int msaa)
{
    EGLint ia[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE,   24,
        EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, msaa ? 1 : 0,
        EGL_SAMPLES,        msaa ? msaa : 0,
        EGL_NONE
    };
    EGLint n = 0;
    if (!eglChooseConfig(dpy, ia, &cfg, 1, &n) || n < 1) {
        return 0;
    }
    eglGetConfigAttrib(dpy, cfg, EGL_ALPHA_SIZE, &cAlphaBits);
    eglGetConfigAttrib(dpy, cfg, EGL_DEPTH_SIZE, &cDepthBits);
    eglGetConfigAttrib(dpy, cfg, EGL_STENCIL_SIZE, &cStencilBits);
    cAuxBuffers = 0;
    eglGetConfigAttrib(dpy, cfg, EGL_SAMPLE_BUFFERS, &cSampleBuf[0]);
    eglGetConfigAttrib(dpy, cfg, EGL_SAMPLES, &cSampleBuf[1]);
    return 1;
}

/* WGL_ARB_create_context attribs (== GLX values) -> EGL attribs */
static void *plat_ctx_create(void *share, const int *wgl)
{
    EGLint ea[16];
    int n = 0;
    if (wgl) {
        for (int i = 0; wgl[i] && n + 2 < 16; i += 2) {
            switch (wgl[i]) {
            case WGL_CONTEXT_MAJOR_VERSION_ARB:
                ea[n++] = EGL_CONTEXT_MAJOR_VERSION; ea[n++] = wgl[i + 1];
                break;
            case WGL_CONTEXT_MINOR_VERSION_ARB:
                ea[n++] = EGL_CONTEXT_MINOR_VERSION; ea[n++] = wgl[i + 1];
                break;
            case WGL_CONTEXT_PROFILE_MASK_ARB:
                /* core = 1, compatibility = 2 in both APIs */
                ea[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK; ea[n++] = wgl[i + 1];
                break;
            case WGL_CONTEXT_FLAGS_ARB:
                if (wgl[i + 1] & WGL_CONTEXT_DEBUG_BIT_ARB) {
                    ea[n++] = EGL_CONTEXT_OPENGL_DEBUG; ea[n++] = EGL_TRUE;
                }
                if (wgl[i + 1] & WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB) {
                    ea[n++] = EGL_CONTEXT_OPENGL_FORWARD_COMPATIBLE; ea[n++] = EGL_TRUE;
                }
                break;
            default:
                break;
            }
        }
        ea[n] = EGL_NONE;
    }
    EGLContext c = eglCreateContext(dpy, cfg, share ? (EGLContext)share : EGL_NO_CONTEXT,
                                    wgl ? ea : ctx_compat);
    if (c == EGL_NO_CONTEXT) {
        DPRINTF("eglCreateContext failed 0x%x", eglGetError());
        return NULL;
    }
    return c;
}

static void plat_ctx_destroy(void *c)
{
    if (c == ctx[0]) {
        /* the ring's GL objects belong to this context */
        for (int i = 0; i < ZC_SLOTS; i++) {
            zc_slot_free(i);
        }
    }
    eglDestroyContext(dpy, (EGLContext)c);
}

static void plat_unbind(void)
{
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

static int plat_make_current(void *c)
{
    if (!eglMakeCurrent(dpy, win, win, (EGLContext)c)) {
        DPRINTF("eglMakeCurrent failed 0x%x", eglGetError());
        return 0;
    }
    return 1;
}

static void *plat_get_current(void)
{
    return eglGetCurrentContext();
}

/* (re)create the pbuffer that plays the window; keeps the context */
static int plat_drawable(int w, int h)
{
    EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(dpy, cfg, pa);
    if (s == EGL_NO_SURFACE) {
        DPRINTF("pbuffer %dx%d failed 0x%x", w, h, eglGetError());
        return 0;
    }
    EGLContext cur = eglGetCurrentContext();
    if (win != EGL_NO_SURFACE) {
        if (cur != EGL_NO_CONTEXT && eglGetCurrentSurface(EGL_DRAW) == win) {
            eglMakeCurrent(dpy, s, s, cur);
        }
        eglDestroySurface(dpy, win);
    }
    win = s;
    return 1;
}

static void plat_drawable_release(void)
{
    for (int i = 0; i < ZC_SLOTS; i++) {
        zc_slot_free(i);
    }
    if (win != EGL_NO_SURFACE) {
        eglDestroySurface(dpy, win);
        win = EGL_NO_SURFACE;
    }
}

static int plat_present_zero_copy(void)
{
    return zc_present();
}

static int plat_readback(uint32_t *dst)
{
    GLint prev_fbo = 0, prev_pack = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, win_w, win_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, prev_pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_fbo);
    return 1;
}

static int plat_pbuffer_create(int i, int w, int h)
{
    const EGLint ia[] = {
        EGL_SURFACE_TYPE,    EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, cAlphaBits,
        EGL_DEPTH_SIZE, cDepthBits,
        EGL_NONE,
    };
    EGLint pa[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    EGLConfig pbcfg;
    EGLint n = 0;
    if (!eglChooseConfig(dpy, ia, &pbcfg, 1, &n) || n < 1) {
        return 0;
    }
    PBDC[i] = eglCreatePbufferSurface(dpy, pbcfg, pa);
    PBRC[i] = eglCreateContext(dpy, pbcfg, eglGetCurrentContext(), ctx_compat);
    return PBDC[i] != EGL_NO_SURFACE && PBRC[i] != EGL_NO_CONTEXT;
}

static void plat_pbuffer_destroy(int i)
{
    if (PBRC[i])
        eglDestroyContext(dpy, PBRC[i]);
    if (PBDC[i])
        eglDestroySurface(dpy, PBDC[i]);
    PBRC[i] = 0; PBDC[i] = 0;
}

static void plat_pbuffer_current(int i)
{
    eglMakeCurrent(dpy, PBDC[i], PBDC[i], PBRC[i]);
}

static void plat_pbuffer_bind_tex(int i, int wgl_target, int wgl_format)
{
    int prev_binded_texture = 0;
    EGLContext prev_context = eglGetCurrentContext();
    EGLSurface prev_draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface prev_read = eglGetCurrentSurface(EGL_READ);
    glGetIntegerv(PbufferGLBinding(wgl_target), &prev_binded_texture);
    eglMakeCurrent(dpy, PBDC[i], PBDC[i], PBRC[i]);
    glBindTexture(PbufferGLAttrib(wgl_target), prev_binded_texture);
    glCopyTexImage2D(PbufferGLAttrib(wgl_target), hPbuffer[i].level,
        PbufferGLAttrib(wgl_format), 0, 0, hPbuffer[i].width, hPbuffer[i].height, 0);
    eglMakeCurrent(dpy, prev_draw, prev_read, prev_context);
}

static void *plat_get_proc(const char *name)
{
    return (void *)eglGetProcAddress(name);
}

/*
 * The pbuffer is framebuffer 0 and has one colour buffer, the back one,
 * unless the config came out single-buffered, in which case it is the front
 * (see the shared hooks above).
 */
static int plat_default_fb(unsigned binding, unsigned *target)
{
    GLint fb = 0;
    GLboolean dbl = GL_FALSE;

    glGetIntegerv(binding, &fb);
    if (fb) {
        return 0;               /* an FBO of the guest's own */
    }
    glGetBooleanv(GL_DOUBLEBUFFER, &dbl);
    *target = dbl ? GL_BACK : GL_FRONT;
    return 1;
}

static void plat_set_func_ptr(void *hdll)
{
    install_buffer_hooks();
}

/* ============================================================== macOS */
#elif defined(CONFIG_DARWIN)
#define GL_SILENCE_DEPRECATION 1
#include <dlfcn.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurface.h>

/*
 * mesagl_impl.c dlopens this for the dispatch table; the framework umbrella
 * re-exports libGL, so CGL and GL come from one handle. Every call this
 * backend makes goes through that same handle: the QEMU macOS build also
 * links XQuartz's Mesa libGL, and a plainly linked gl* symbol binds to it
 * (a GLX library that sees no CGL context and returns nothing).
 */
const char dllname[] = "/System/Library/Frameworks/OpenGL.framework/OpenGL";

int MGLUpdateGuestBufo(mapbufo_t *bufo, int add) { return 0; }

static void *hdll;

static struct {
    /* CGL */
    CGLError (*ChoosePixelFormat)(const CGLPixelFormatAttribute *, CGLPixelFormatObj *, GLint *);
    CGLError (*DestroyPixelFormat)(CGLPixelFormatObj);
    CGLError (*DescribePixelFormat)(CGLPixelFormatObj, GLint, CGLPixelFormatAttribute, GLint *);
    CGLError (*CreateContext)(CGLPixelFormatObj, CGLContextObj, CGLContextObj *);
    CGLError (*DestroyContext)(CGLContextObj);
    CGLError (*SetCurrentContext)(CGLContextObj);
    CGLContextObj (*GetCurrentContext)(void);
    const char *(*ErrorString)(CGLError);
    /* GL */
    GLenum (*GetError)(void);
    const GLubyte *(*GetString)(GLenum);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*ReadBuffer)(GLenum);
    void (*PixelStorei)(GLenum, GLint);
    void (*BindBuffer)(GLenum, GLuint);
    void (*GenTextures)(GLsizei, GLuint *);
    void (*BindTexture)(GLenum, GLuint);
    void (*DeleteTextures)(GLsizei, const GLuint *);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*CopyTexImage2D)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint);
    void (*GenFramebuffersEXT)(GLsizei, GLuint *);
    void (*BindFramebufferEXT)(GLenum, GLuint);
    void (*DeleteFramebuffersEXT)(GLsizei, const GLuint *);
    void (*GenRenderbuffersEXT)(GLsizei, GLuint *);
    void (*BindRenderbufferEXT)(GLenum, GLuint);
    void (*RenderbufferStorageEXT)(GLenum, GLenum, GLsizei, GLsizei);
    void (*DeleteRenderbuffersEXT)(GLsizei, const GLuint *);
    void (*FramebufferRenderbufferEXT)(GLenum, GLenum, GLenum, GLuint);
    void (*FramebufferTexture2DEXT)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (*CheckFramebufferStatusEXT)(GLenum);
    void (*BlitFramebufferEXT)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
    void (*Flush)(void);
    void (*Finish)(void);
    CGLError (*TexImageIOSurface2D)(CGLContextObj, GLenum, GLenum, GLsizei, GLsizei,
                                    GLenum, GLenum, IOSurfaceRef, GLuint);
} gl;

/* IOSurface + CoreFoundation, resolved the same way (no link changes) */
static struct {
    IOSurfaceRef (*Create)(CFDictionaryRef);
    CFDictionaryRef (*DictionaryCreate)(CFAllocatorRef, const void **, const void **, CFIndex,
                                        const CFDictionaryKeyCallBacks *,
                                        const CFDictionaryValueCallBacks *);
    CFNumberRef (*NumberCreate)(CFAllocatorRef, CFNumberType, const void *);
    void (*Release)(CFTypeRef);
    CFStringRef kWidth, kHeight, kBytesPerElement, kPixelFormat;
    const CFDictionaryKeyCallBacks *keycb;
    const CFDictionaryValueCallBacks *valcb;
    int loaded;
} ios;

static int ios_load(void)
{
    if (ios.loaded) {
        return ios.loaded > 0;
    }
    void *hi = dlopen("/System/Library/Frameworks/IOSurface.framework/IOSurface", RTLD_NOW);
    void *hc = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", RTLD_NOW);
    if (!hi || !hc) {
        DPRINTF("IOSurface/CoreFoundation: %s", dlerror());
        ios.loaded = -1;
        return 0;
    }
    ios.Create = dlsym(hi, "IOSurfaceCreate");
    ios.DictionaryCreate = dlsym(hc, "CFDictionaryCreate");
    ios.NumberCreate = dlsym(hc, "CFNumberCreate");
    ios.Release = dlsym(hc, "CFRelease");
    CFStringRef *k;
    ios.kWidth = (k = dlsym(hi, "kIOSurfaceWidth")) ? *k : NULL;
    ios.kHeight = (k = dlsym(hi, "kIOSurfaceHeight")) ? *k : NULL;
    ios.kBytesPerElement = (k = dlsym(hi, "kIOSurfaceBytesPerElement")) ? *k : NULL;
    ios.kPixelFormat = (k = dlsym(hi, "kIOSurfacePixelFormat")) ? *k : NULL;
    ios.keycb = dlsym(hc, "kCFTypeDictionaryKeyCallBacks");
    ios.valcb = dlsym(hc, "kCFTypeDictionaryValueCallBacks");
    if (!ios.Create || !ios.DictionaryCreate || !ios.NumberCreate || !ios.Release
        || !ios.kWidth || !ios.kHeight || !ios.kBytesPerElement || !ios.kPixelFormat
        || !ios.keycb || !ios.valcb) {
        DPRINTF("IOSurface symbols missing");
        ios.loaded = -1;
        return 0;
    }
    ios.loaded = 1;
    return 1;
}

static int gl_load(void)
{
    if (gl.CheckFramebufferStatusEXT) {
        return 1;
    }
    if (!hdll) {
        hdll = dlopen(dllname, RTLD_NOW);
        if (!hdll) {
            DPRINTF("dlopen %s: %s", dllname, dlerror());
            return 0;
        }
    }
    int missing = 0;
#define SYM(field, name) do { gl.field = dlsym(hdll, name); \
    if (!gl.field) { DPRINTF("missing %s", name); missing++; } } while (0)
    SYM(ChoosePixelFormat, "CGLChoosePixelFormat");
    SYM(DestroyPixelFormat, "CGLDestroyPixelFormat");
    SYM(DescribePixelFormat, "CGLDescribePixelFormat");
    SYM(CreateContext, "CGLCreateContext");
    SYM(DestroyContext, "CGLDestroyContext");
    SYM(SetCurrentContext, "CGLSetCurrentContext");
    SYM(GetCurrentContext, "CGLGetCurrentContext");
    SYM(ErrorString, "CGLErrorString");
    SYM(GetError, "glGetError");
    SYM(GetString, "glGetString");
    SYM(GetIntegerv, "glGetIntegerv");
    SYM(Viewport, "glViewport");
    SYM(ReadPixels, "glReadPixels");
    SYM(ReadBuffer, "glReadBuffer");
    SYM(PixelStorei, "glPixelStorei");
    SYM(BindBuffer, "glBindBuffer");
    SYM(GenTextures, "glGenTextures");
    SYM(BindTexture, "glBindTexture");
    SYM(DeleteTextures, "glDeleteTextures");
    SYM(TexImage2D, "glTexImage2D");
    SYM(TexParameteri, "glTexParameteri");
    SYM(CopyTexImage2D, "glCopyTexImage2D");
    SYM(GenFramebuffersEXT, "glGenFramebuffersEXT");
    SYM(BindFramebufferEXT, "glBindFramebufferEXT");
    SYM(DeleteFramebuffersEXT, "glDeleteFramebuffersEXT");
    SYM(GenRenderbuffersEXT, "glGenRenderbuffersEXT");
    SYM(BindRenderbufferEXT, "glBindRenderbufferEXT");
    SYM(RenderbufferStorageEXT, "glRenderbufferStorageEXT");
    SYM(DeleteRenderbuffersEXT, "glDeleteRenderbuffersEXT");
    SYM(FramebufferRenderbufferEXT, "glFramebufferRenderbufferEXT");
    SYM(FramebufferTexture2DEXT, "glFramebufferTexture2DEXT");
    SYM(CheckFramebufferStatusEXT, "glCheckFramebufferStatusEXT");
    SYM(BlitFramebufferEXT, "glBlitFramebufferEXT");
    SYM(Flush, "glFlush");
    SYM(Finish, "glFinish");
    SYM(TexImageIOSurface2D, "CGLTexImageIOSurface2D");
#undef SYM
    if (missing) {
        memset(&gl, 0, sizeof(gl));
        return 0;
    }
    return 1;
}

static CGLPixelFormatObj pix;
static int      cgl_open;
/*
 * The default framebuffer stand-in: an FBO per context level (FBO names
 * are not shared between contexts) over shared renderbuffers. Binding 0
 * through the guest's dispatch is redirected here (MesaGLSetFunc).
 */
static GLuint   dfbo[MAX_LVLCNTX];
static void    *dfbo_ctx[MAX_LVLCNTX];     /* the context each FBO belongs to */
static GLuint   rb_color, rb_depth;
static GLuint   cur_dfbo;                   /* FBO of the current context */
static int      rb_w, rb_h;
static int      dfbo_ok;                    /* current stand-in is complete */
/* WGL pbuffer emulation: FBO + texture in the main context */
static GLuint   pb_fbo[MAX_PBUFFER], pb_tex[MAX_PBUFFER], pb_depth[MAX_PBUFFER];
static void (*real_bind_fb)(GLenum, GLuint);
static void (*real_bind_fb_ext)(GLenum, GLuint);

static void fx_glBindFramebuffer(GLenum target, GLuint fb)
{
    (real_bind_fb ? real_bind_fb : gl.BindFramebufferEXT)(target, fb ? fb : cur_dfbo);
}
static void fx_glBindFramebufferEXT(GLenum target, GLuint fb)
{
    (real_bind_fb_ext ? real_bind_fb_ext : gl.BindFramebufferEXT)(target, fb ? fb : cur_dfbo);
}

/*
 * The stand-in has one colour attachment, so both of a window's buffers are
 * it (see the shared hooks above).
 */
static int plat_default_fb(unsigned binding, unsigned *target)
{
    GLint fb = 0;

    gl.GetIntegerv(binding, &fb);
    if (!cur_dfbo || (GLuint)fb != cur_dfbo) {
        return 0;
    }
    *target = GL_COLOR_ATTACHMENT0_EXT;
    return 1;
}

static const char *gl_err_str(GLenum e)
{
    switch (e) {
    case GL_NO_ERROR: return "ok";
    case GL_INVALID_ENUM: return "INVALID_ENUM";
    case GL_INVALID_VALUE: return "INVALID_VALUE";
    case GL_INVALID_OPERATION: return "INVALID_OPERATION";
    case GL_INVALID_FRAMEBUFFER_OPERATION_EXT: return "INVALID_FRAMEBUFFER_OPERATION";
    case GL_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    default: return "?";
    }
}
#define GLCHK(what) do { GLenum e_ = gl.GetError(); \
    if (e_ != GL_NO_ERROR) DPRINTF("%s: GL error %s (0x%x)", what, gl_err_str(e_), e_); } while (0)

/*
 * Zero-copy presentation (doc 12 §4, macOS): a ring of IOSurfaces bound to
 * GL_TEXTURE_RECTANGLE textures (CGLTexImageIOSurface2D); each swap blits
 * the stand-in FBO into the next one (Y-flipped) and the frontend wraps the
 * same IOSurface in a Metal texture. zc_state: -1 undecided, 0 off, 1 on.
 */
#define ZC_SLOTS 3
#define kIOSurfacePixelFormatBGRA 0x42475241u   /* 'BGRA' */
static int zc_state = -1;
static struct {
    IOSurfaceRef surf;
    GLuint tex, fbo;
    int w, h;
} zc[ZC_SLOTS];
static int zc_next;

static void zc_slot_free(int i)
{
    /* GL names die with their context; delete only while one is current */
    if (gl.GetCurrentContext()) {
        if (zc[i].fbo) {
            gl.DeleteFramebuffersEXT(1, &zc[i].fbo);
        }
        if (zc[i].tex) {
            gl.DeleteTextures(1, &zc[i].tex);
        }
    }
    if (zc[i].surf) {
        ios.Release(zc[i].surf);
    }
    memset(&zc[i], 0, sizeof(zc[i]));
}

static void zc_off(const char *why)
{
    if (zc_state != 0) {
        DPRINTF("zero-copy off: %s (readback path)", why);
    }
    zc_state = 0;
    for (int i = 0; i < ZC_SLOTS; i++) {
        zc_slot_free(i);
    }
}

static int zc_slot_ensure(int i, int w, int h)
{
    if (zc[i].surf && zc[i].w == w && zc[i].h == h) {
        return 1;
    }
    zc_slot_free(i);
    int bpe = 4;
    uint32_t fmt = kIOSurfacePixelFormatBGRA;
    CFNumberRef vw = ios.NumberCreate(NULL, kCFNumberIntType, &w);
    CFNumberRef vh = ios.NumberCreate(NULL, kCFNumberIntType, &h);
    CFNumberRef vb = ios.NumberCreate(NULL, kCFNumberIntType, &bpe);
    CFNumberRef vf = ios.NumberCreate(NULL, kCFNumberSInt32Type, &fmt);
    const void *keys[] = { ios.kWidth, ios.kHeight, ios.kBytesPerElement, ios.kPixelFormat };
    const void *vals[] = { vw, vh, vb, vf };
    CFDictionaryRef d = ios.DictionaryCreate(NULL, keys, vals, 4, ios.keycb, ios.valcb);
    zc[i].surf = ios.Create(d);
    ios.Release(d);
    ios.Release(vw); ios.Release(vh); ios.Release(vb); ios.Release(vf);
    if (!zc[i].surf) {
        zc_off("IOSurfaceCreate failed");
        return 0;
    }
    zc[i].w = w;
    zc[i].h = h;
    GLint prev_tex = 0, prev_fb = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_RECTANGLE_ARB, &prev_tex);
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev_fb);
    while (gl.GetError() != GL_NO_ERROR);
    gl.GenTextures(1, &zc[i].tex);
    gl.BindTexture(GL_TEXTURE_RECTANGLE_ARB, zc[i].tex);
    CGLError err = gl.TexImageIOSurface2D(gl.GetCurrentContext(), GL_TEXTURE_RECTANGLE_ARB,
                                          GL_RGBA, w, h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV,
                                          zc[i].surf, 0);
    gl.BindTexture(GL_TEXTURE_RECTANGLE_ARB, prev_tex);
    if (err != kCGLNoError) {
        DPRINTF("CGLTexImageIOSurface2D: %s", gl.ErrorString(err));
        zc_off("IOSurface texture binding failed");
        return 0;
    }
    gl.GenFramebuffersEXT(1, &zc[i].fbo);
    gl.BindFramebufferEXT(GL_FRAMEBUFFER_EXT, zc[i].fbo);
    gl.FramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                               GL_TEXTURE_RECTANGLE_ARB, zc[i].tex, 0);
    GLenum st = gl.CheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    gl.BindFramebufferEXT(GL_FRAMEBUFFER_EXT, prev_fb ? prev_fb : cur_dfbo);
    if (st != GL_FRAMEBUFFER_COMPLETE_EXT) {
        DPRINTF("IOSurface FBO incomplete 0x%x", st);
        zc_off("IOSurface FBO incomplete");
        return 0;
    }
    if (!embed_fx_iosurface(i, zc[i].surf, w, h)) {
        zc_off("frontend declined the IOSurface");
        return 0;
    }
    DPRINTF("zero-copy slot %d: %dx%d IOSurface %p", i, w, h, (void *)zc[i].surf);
    return 1;
}

static int zc_present(void)
{
    if (zc_state < 0) {
        zc_state = ios_load() && gl.TexImageIOSurface2D && gl.BlitFramebufferEXT ? 1 : 0;
        if (!zc_state) {
            zc_off("IOSurface / blit support missing");
        } else {
            DPRINTF("zero-copy: IOSurface ring");
        }
    }
    if (zc_state != 1 || !dfbo_ok) {
        return 0;
    }
    int i = zc_next;
    if (!zc_slot_ensure(i, win_w, win_h)) {
        return 0;
    }
    GLint prev_read = 0, prev_draw = 0;
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &prev_read);
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_EXT, &prev_draw);
    gl.BindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, cur_dfbo);
    gl.BindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, zc[i].fbo);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    /* flip: the stand-in FBO is bottom-up, the IOSurface is read top-down */
    gl.BlitFramebufferEXT(0, 0, win_w, win_h, 0, win_h, win_w, 0,
                          GL_COLOR_BUFFER_BIT, GL_NEAREST);
    gl.BindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, prev_read ? prev_read : cur_dfbo);
    gl.BindFramebufferEXT(GL_DRAW_FRAMEBUFFER_EXT, prev_draw ? prev_draw : cur_dfbo);
    /* bring-up sync: the GPU must be done before Metal samples the surface */
    gl.Flush();
    gl.Finish();
    embed_fx_frame_ready(i);
    zc_next = (i + 1) % ZC_SLOTS;
    return 1;
}

static int plat_open(void)
{
    if (cgl_open) {
        return 1;
    }
    if (!gl_load()) {
        return 0;
    }
    /* any accelerated renderer will do; probing needs no context */
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        (CGLPixelFormatAttribute)0
    };
    CGLPixelFormatObj p = NULL;
    GLint npix = 0;
    CGLError err = gl.ChoosePixelFormat(attrs, &p, &npix);
    if (err != kCGLNoError || !p) {
        DPRINTF("CGLChoosePixelFormat: %s", gl.ErrorString(err));
        return 0;
    }
    gl.DestroyPixelFormat(p);
    cgl_open = 1;
    xcstr = "Apple";
    xstr = "";
    DPRINTF("CGL (window-less)");
    return 1;
}

static CGLPixelFormatObj choose_pix(int msaa, int profile)
{
    CGLPixelFormatAttribute attrs[24];
    int n = 0;
#define A(x) ((CGLPixelFormatAttribute)(x))
    attrs[n++] = kCGLPFAAccelerated;
    attrs[n++] = kCGLPFAColorSize;   attrs[n++] = A(24);
    attrs[n++] = kCGLPFAAlphaSize;   attrs[n++] = A(8);
    attrs[n++] = kCGLPFADepthSize;   attrs[n++] = A(24);
    attrs[n++] = kCGLPFAStencilSize; attrs[n++] = A(8);
    attrs[n++] = kCGLPFAOpenGLProfile; attrs[n++] = A(profile);
    if (msaa) {
        attrs[n++] = kCGLPFASampleBuffers; attrs[n++] = A(1);
        attrs[n++] = kCGLPFASamples;       attrs[n++] = A(msaa);
        attrs[n++] = kCGLPFAMultisample;
    }
    attrs[n] = A(0);
#undef A
    CGLPixelFormatObj p = NULL;
    GLint npix = 0;
    CGLError err = gl.ChoosePixelFormat(attrs, &p, &npix);
    if (err != kCGLNoError || !p) {
        DPRINTF("CGLChoosePixelFormat(profile 0x%x msaa %d): %s", profile, msaa,
                gl.ErrorString(err));
        return NULL;
    }
    return p;
}

static int plat_choose(int msaa)
{
    /* legacy = compatibility 2.1: the guests are fixed-function GL */
    CGLPixelFormatObj p = choose_pix(msaa, kCGLOGLPVersion_Legacy);
    if (!p) {
        return 0;
    }
    if (pix) {
        gl.DestroyPixelFormat(pix);
    }
    pix = p;
    GLint v = 0;
    gl.DescribePixelFormat(pix, 0, kCGLPFAAlphaSize, &v);   cAlphaBits = v;
    gl.DescribePixelFormat(pix, 0, kCGLPFADepthSize, &v);   cDepthBits = v;
    gl.DescribePixelFormat(pix, 0, kCGLPFAStencilSize, &v); cStencilBits = v;
    cAuxBuffers = 0;
    /* the FBO stand-in is single-sampled for now */
    cSampleBuf[0] = 0;
    cSampleBuf[1] = 0;
    return 1;
}

static void *plat_ctx_create(void *share, const int *wgl)
{
    int profile = kCGLOGLPVersion_Legacy;
    if (wgl) {
        int major = 0, mask = 0;
        for (int i = 0; wgl[i]; i += 2) {
            if (wgl[i] == WGL_CONTEXT_MAJOR_VERSION_ARB) major = wgl[i + 1];
            if (wgl[i] == WGL_CONTEXT_PROFILE_MASK_ARB)  mask = wgl[i + 1];
        }
        if (major >= 4 && (mask & WGL_CONTEXT_CORE_PROFILE_BIT_ARB)) {
            profile = kCGLOGLPVersion_GL4_Core;
        } else if (major >= 3 && (mask & WGL_CONTEXT_CORE_PROFILE_BIT_ARB)) {
            profile = kCGLOGLPVersion_3_2_Core;
        }
    }
    CGLPixelFormatObj p = pix;
    int own = 0;
    if (profile != kCGLOGLPVersion_Legacy) {
        p = choose_pix(GetContextMSAA(), profile);
        own = p != NULL;
        if (!p) {
            p = pix;    /* fall back to legacy rather than fail */
        }
    }
    CGLContextObj c = NULL;
    CGLError err = gl.CreateContext(p, (CGLContextObj)share, &c);
    if (own) {
        gl.DestroyPixelFormat(p);
    }
    if (err != kCGLNoError) {
        DPRINTF("CGLCreateContext: %s", gl.ErrorString(err));
        return NULL;
    }
    return c;
}

static void ctx_fbo_forget(void *c)
{
    for (int i = 0; i < MAX_LVLCNTX; i++) {
        if (dfbo_ctx[i] == c) {
            dfbo[i] = 0;
            dfbo_ctx[i] = NULL;
        }
    }
}

static void plat_ctx_destroy(void *c)
{
    ctx_fbo_forget(c);
    if (c == ctx[0]) {
        /* the ring's GL objects belong to this context: drop them while it
         * can still be made current, then forget the shared names */
        CGLContextObj cur = gl.GetCurrentContext();
        if (!cur) {
            gl.SetCurrentContext((CGLContextObj)c);
        }
        for (int i = 0; i < ZC_SLOTS; i++) {
            zc_slot_free(i);
        }
        if (!cur) {
            gl.SetCurrentContext(NULL);
        }
        /* renderbuffers live in the share group rooted at ctx[0] */
        rb_color = rb_depth = 0;
        rb_w = rb_h = 0;
        for (int i = 0; i < MAX_PBUFFER; i++) {
            pb_fbo[i] = pb_tex[i] = pb_depth[i] = 0;
        }
    }
    if (gl.GetCurrentContext() == (CGLContextObj)c) {
        gl.SetCurrentContext(NULL);
        cur_dfbo = 0;
        dfbo_ok = 0;
    }
    gl.DestroyContext((CGLContextObj)c);
}

static void plat_unbind(void)
{
    gl.SetCurrentContext(NULL);
    cur_dfbo = 0;
    dfbo_ok = 0;
}

static void *plat_get_current(void)
{
    return gl.GetCurrentContext();
}

/* renderbuffers at the drawable size (shared objects, created once) */
static int rb_ensure(int w, int h)
{
    if (!rb_color) {
        gl.GenRenderbuffersEXT(1, &rb_color);
        gl.GenRenderbuffersEXT(1, &rb_depth);
        rb_w = rb_h = 0;
    }
    if (rb_w != w || rb_h != h) {
        while (gl.GetError() != GL_NO_ERROR);
        gl.BindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb_color);
        gl.RenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, w, h);
        GLCHK("color renderbuffer storage");
        gl.BindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb_depth);
        gl.RenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
        GLCHK("depth/stencil renderbuffer storage");
        gl.BindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
        DPRINTF("renderbuffers %u/%u %dx%d", rb_color, rb_depth, w, h);
        rb_w = w;
        rb_h = h;
    }
    return 1;
}

/* the current context's FBO 0 stand-in, created on first use */
static int dfbo_ensure(void)
{
    CGLContextObj c = gl.GetCurrentContext();
    int slot = -1;
    if (!c) {
        return 0;
    }
    rb_ensure(win_w ? win_w : 640, win_h ? win_h : 480);
    for (int i = 0; i < MAX_LVLCNTX; i++) {
        if (dfbo_ctx[i] == c && dfbo[i]) {
            cur_dfbo = dfbo[i];
            gl.BindFramebufferEXT(GL_FRAMEBUFFER_EXT, cur_dfbo);
            dfbo_ok = gl.CheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) == GL_FRAMEBUFFER_COMPLETE_EXT;
            return 1;
        }
        if (slot < 0 && !dfbo[i]) {
            slot = i;
        }
    }
    if (slot < 0) {
        return 0;
    }
    {
        static int once;
        if (!once) {
            once = 1;
            DPRINTF("GL %s / %s", (const char *)gl.GetString(GL_VERSION),
                    (const char *)gl.GetString(GL_RENDERER));
        }
    }
    while (gl.GetError() != GL_NO_ERROR);
    gl.GenFramebuffersEXT(1, &dfbo[slot]);
    GLCHK("glGenFramebuffersEXT");
    dfbo_ctx[slot] = c;
    gl.BindFramebufferEXT(GL_FRAMEBUFFER_EXT, dfbo[slot]);
    GLCHK("glBindFramebufferEXT");
    GLint bound = -1;
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &bound);
    gl.FramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT,
                                  GL_RENDERBUFFER_EXT, rb_color);
    GLCHK("attach color");
    gl.FramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT,
                                  GL_RENDERBUFFER_EXT, rb_depth);
    gl.FramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT,
                                  GL_RENDERBUFFER_EXT, rb_depth);
    GLCHK("attach depth/stencil");
    GLenum st = gl.CheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    GLCHK("glCheckFramebufferStatusEXT");
    dfbo_ok = (st == GL_FRAMEBUFFER_COMPLETE_EXT);
    DPRINTF("default FBO %u (bound %d) rb %u/%u: %s (0x%x)", dfbo[slot], bound,
            rb_color, rb_depth, dfbo_ok ? "complete" : "INCOMPLETE", st);
    cur_dfbo = dfbo[slot];
    gl.Viewport(0, 0, rb_w, rb_h);
    return 1;
}

static int plat_make_current(void *c)
{
    CGLError err = gl.SetCurrentContext((CGLContextObj)c);
    if (err != kCGLNoError) {
        DPRINTF("CGLSetCurrentContext: %s", gl.ErrorString(err));
        return 0;
    }
    return dfbo_ensure();
}

static int plat_drawable(int w, int h)
{
    /* storage follows lazily in dfbo_ensure()/rb_ensure() on the vCPU thread */
    if (gl.GetCurrentContext()) {
        rb_ensure(w, h);
    }
    return 1;
}

static void plat_drawable_release(void)
{
    /* objects die with the share group in plat_ctx_destroy(ctx[0]) */
}

static int plat_present_zero_copy(void)
{
    return zc_present();
}

static int readback_warned;
static int plat_readback(uint32_t *dst)
{
    GLint prev_read = 0, prev_pack = 0;
    if (!dfbo_ok || !cur_dfbo) {
        if (!readback_warned++) {
            DPRINTF("no complete default FBO: frames not published");
        }
        return 0;
    }
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &prev_read);
    gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
    gl.BindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, cur_dfbo);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl.ReadBuffer(GL_COLOR_ATTACHMENT0_EXT);
    while (gl.GetError() != GL_NO_ERROR);
    gl.ReadPixels(0, 0, win_w, win_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    GLenum e = gl.GetError();
    if (e != GL_NO_ERROR && !readback_warned++) {
        DPRINTF("glReadPixels: %s (0x%x)", gl_err_str(e), e);
    }
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, prev_pack);
    gl.BindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, prev_read ? prev_read : cur_dfbo);
    return e == GL_NO_ERROR;
}

/* WGL pbuffers: FBO + texture in the current (main) context */
static int plat_pbuffer_create(int i, int w, int h)
{
    if (!gl.GetCurrentContext()) {
        return 0;
    }
    GLint prev_fb = 0, prev_tex = 0;
    gl.GetIntegerv(GL_FRAMEBUFFER_BINDING_EXT, &prev_fb);
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    gl.GenTextures(1, &pb_tex[i]);
    gl.BindTexture(GL_TEXTURE_2D, pb_tex[i]);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, NULL);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    gl.GenRenderbuffersEXT(1, &pb_depth[i]);
    gl.BindRenderbufferEXT(GL_RENDERBUFFER_EXT, pb_depth[i]);
    gl.RenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, w, h);
    gl.BindRenderbufferEXT(GL_RENDERBUFFER_EXT, 0);
    gl.GenFramebuffersEXT(1, &pb_fbo[i]);
    gl.BindFramebufferEXT(GL_FRAMEBUFFER_EXT, pb_fbo[i]);
    gl.FramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_2D, pb_tex[i], 0);
    gl.FramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, pb_depth[i]);
    gl.FramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT, pb_depth[i]);
    GLenum st = gl.CheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT);
    gl.BindFramebufferEXT(GL_FRAMEBUFFER_EXT, prev_fb ? prev_fb : cur_dfbo);
    gl.BindTexture(GL_TEXTURE_2D, prev_tex);
    if (st != GL_FRAMEBUFFER_COMPLETE_EXT) {
        DPRINTF("pbuffer FBO incomplete 0x%x", st);
        plat_pbuffer_destroy(i);
        return 0;
    }
    return 1;
}

static void plat_pbuffer_destroy(int i)
{
    if (pb_fbo[i]) {
        gl.DeleteFramebuffersEXT(1, &pb_fbo[i]);
    }
    if (pb_depth[i]) {
        gl.DeleteRenderbuffersEXT(1, &pb_depth[i]);
    }
    if (pb_tex[i]) {
        gl.DeleteTextures(1, &pb_tex[i]);
    }
    pb_fbo[i] = pb_depth[i] = pb_tex[i] = 0;
}

static void plat_pbuffer_current(int i)
{
    /* same context: "making the pbuffer current" = drawing into its FBO */
    if (pb_fbo[i]) {
        gl.BindFramebufferEXT(GL_FRAMEBUFFER_EXT, pb_fbo[i]);
        gl.Viewport(0, 0, hPbuffer[i].width, hPbuffer[i].height);
    }
}

static void plat_pbuffer_bind_tex(int i, int wgl_target, int wgl_format)
{
    /* copy the pbuffer's colour into the texture the guest has bound */
    GLint prev_read = 0, prev_tex = 0;
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING_EXT, &prev_read);
    gl.GetIntegerv(PbufferGLBinding(wgl_target), &prev_tex);
    gl.BindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, pb_fbo[i]);
    gl.BindTexture(PbufferGLAttrib(wgl_target), prev_tex);
    gl.CopyTexImage2D(PbufferGLAttrib(wgl_target), hPbuffer[i].level,
        PbufferGLAttrib(wgl_format), 0, 0, hPbuffer[i].width, hPbuffer[i].height, 0);
    gl.BindFramebufferEXT(GL_READ_FRAMEBUFFER_EXT, prev_read ? prev_read : cur_dfbo);
}

static void *plat_get_proc(const char *name)
{
    if (!hdll) {
        hdll = dlopen(dllname, RTLD_NOW);
    }
    return hdll ? dlsym(hdll, name) : NULL;
}

static void plat_set_func_ptr(void *h)
{
    hdll = h;
    gl_load();
    /* redirect framebuffer 0 to the stand-in FBO in the guest's dispatch */
    real_bind_fb = MesaGLSetFunc(FEnum_glBindFramebuffer, (void *)fx_glBindFramebuffer);
    real_bind_fb_ext = MesaGLSetFunc(FEnum_glBindFramebufferEXT, (void *)fx_glBindFramebufferEXT);
    DPRINTF("FBO 0 redirect installed (table had %p / %p; framework %p)",
            (void *)real_bind_fb, (void *)real_bind_fb_ext, (void *)gl.BindFramebufferEXT);
    if (real_bind_fb == (void *)fx_glBindFramebuffer) {
        real_bind_fb = NULL;      /* re-entered after a DLL reload */
    }
    if (real_bind_fb_ext == (void *)fx_glBindFramebufferEXT) {
        real_bind_fb_ext = NULL;
    }
    install_buffer_hooks();
}

/* ============================================================ Windows */
#elif defined(CONFIG_WIN32)
#include <epoxy/wgl.h>
#include <epoxy/gl.h>

/*
 * WGL without a visible window. Windows, unlike macOS, has a first-class
 * offscreen drawable, a WGL_ARB_pbuffer, so this backend is shaped like
 * the Linux one (the pbuffer is FBO 0 and the swap is a readback of its
 * back buffer) rather than like the macOS one, which has to fake a
 * default framebuffer with an FBO because CGL pbuffers are gone.
 *
 * There is still one window: a 1x1 popup that is created and never
 * shown. WGL has no way to get at a device's pixel formats or its
 * extension entry points without a window and a context on it, and
 * wglCreatePbufferARB itself takes a DC to say which device to create the
 * pbuffer on. So the window exists to be a device handle, and nothing is
 * ever drawn to it.
 *
 * Every WGL and GL call here goes through epoxy, which the embed library
 * already links: it resolves the ARB entry points through the current
 * context, which is exactly the dance WGL requires (opengl32.dll itself
 * exports GL 1.1 and nothing newer).
 */

/* the guest's own buffer objects are a Linux/GBM path; nothing here */
int MGLUpdateGuestBufo(mapbufo_t *bufo, const int add) { return 0; }

static HWND        gl_wnd;          /* the invisible device handle */
static HDC         wnd_dc;          /* its DC: pbuffers are created on it */
static HGLRC       boot_rc;         /* the bootstrap context, kept alive */
static int         pixfmt;          /* the ARB-chosen format */
static HPBUFFERARB drawable;        /* the pbuffer standing in for a window */
static HDC         drawable_dc;
static HPBUFFERARB pb[MAX_PBUFFER]; /* the guest's own pbuffers */
static HDC         PBDC[MAX_PBUFFER];
static HGLRC       PBRC[MAX_PBUFFER];
static void       *gl_dll;          /* opengl32.dll, from plat_set_func_ptr */
static char        wgl_exts[4096];

#define WERR(what) DPRINTF("%s failed, error 0x%08lx", what, GetLastError())

static int make_window(void)
{
    static const char cls[] = "2ksbox-embed-gl";
    HINSTANCE inst = GetModuleHandle(NULL);
    WNDCLASSEX wc;

    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC;            /* the DC must outlive each call */
    wc.lpfnWndProc = DefWindowProc;
    wc.hInstance = inst;
    wc.lpszClassName = cls;
    RegisterClassEx(&wc);           /* already-registered is fine */

    gl_wnd = CreateWindowEx(0, cls, cls, WS_POPUP, 0, 0, 1, 1,
                            NULL, NULL, inst, NULL);
    if (!gl_wnd) {
        WERR("CreateWindowEx");
        return 0;
    }
    wnd_dc = GetDC(gl_wnd);
    if (!wnd_dc) {
        WERR("GetDC");
        return 0;
    }
    /* A window's pixel format can be set exactly once, and this one is
     * only ever a device handle, so it gets the plainest format that
     * supports OpenGL. The ARB-chosen format below is for the pbuffer. */
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
    int f = ChoosePixelFormat(wnd_dc, &boot);
    if (!f || !SetPixelFormat(wnd_dc, f, &boot)) {
        WERR("SetPixelFormat");
        return 0;
    }
    boot_rc = wglCreateContext(wnd_dc);
    if (!boot_rc) {
        WERR("wglCreateContext");
        return 0;
    }
    return 1;
}

static int plat_open(void)
{
    if (boot_rc) {
        return 1;
    }
    if (!make_window()) {
        return 0;
    }
    /* WGL extensions are only visible with a context current, so the
     * bootstrap context is made current once, here, and epoxy resolves
     * everything the rest of this file calls through it. */
    if (!wglMakeCurrent(wnd_dc, boot_rc)) {
        WERR("wglMakeCurrent (bootstrap)");
        return 0;
    }
    int ok = epoxy_has_wgl_extension(wnd_dc, "WGL_ARB_pixel_format")
          && epoxy_has_wgl_extension(wnd_dc, "WGL_ARB_pbuffer");
    if (ok && epoxy_has_wgl_extension(wnd_dc, "WGL_ARB_extensions_string")) {
        const char *e = wglGetExtensionsStringARB(wnd_dc);
        if (e) {
            snprintf(wgl_exts, sizeof(wgl_exts), "%s", e);
        }
    }
    xstr = wgl_exts;
    xcstr = (const char *)glGetString(GL_VENDOR);
    DPRINTF("WGL %s / %s (window-less)", xcstr ? xcstr : "?",
            (const char *)glGetString(GL_RENDERER));
    wglMakeCurrent(NULL, NULL);
    if (!ok) {
        DPRINTF("WGL_ARB_pixel_format / WGL_ARB_pbuffer missing: no offscreen GL");
        return 0;
    }
    if (!xcstr) {
        xcstr = "";
    }
    return 1;
}

/*
 * WGL answers `wglGetProcAddress` only while a context is current on the
 * calling thread, and libepoxy resolves an entry point lazily at its
 * first call. So an ARB call made with nothing current does not fail.
 * It *faults*: ACCESS_VIOLATION, the whole process, and the log ends at
 * whatever was printed before it. That is how GLQuake took the player
 * down on the user's PC: `glcntx: ChoosePixelFormat()` was
 * the last line, and the first `wglChoosePixelFormatARB` below was the
 * faulting call. A 40-line repro of the same three steps (bootstrap
 * context, `wglMakeCurrent(NULL, NULL)`, one ARB call through epoxy)
 * dies the same way on this driver.
 *
 * `plat_open` leaves nothing current on purpose: a WGL context may be
 * current on one thread at a time, and the thread that opened the
 * backend is not always the one that draws. So instead of holding the
 * bootstrap context, every ARB call below *borrows* it for the duration
 * when the caller has no context of its own, and puts back exactly what
 * it found, including "nothing", which is a state the rest of this file
 * relies on.
 */
struct wgl_borrow { HDC dc; HGLRC rc; int taken; };

static void wgl_borrow_ctx(struct wgl_borrow *b)
{
    b->rc = wglGetCurrentContext();
    b->dc = wglGetCurrentDC();
    b->taken = 0;
    if (!b->rc && boot_rc && wglMakeCurrent(wnd_dc, boot_rc)) {
        b->taken = 1;
    }
}

static void wgl_return_ctx(struct wgl_borrow *b)
{
    if (b->taken) {
        wglMakeCurrent(b->dc, b->rc);   /* both NULL: unbound, as it was */
    }
}

static int plat_choose(int msaa)
{
    /* the shared iAttribs, plus the pbuffer this backend draws into and
     * whatever multisampling was asked for */
    int a[ARRAY_SIZE(iAttribs) + 4];
    int n = 0;
    for (int i = 0; iAttribs[i]; i += 2) {
        int v = iAttribs[i + 1];
        if (iAttribs[i] == WGL_SAMPLE_BUFFERS_ARB) v = msaa ? 1 : 0;
        if (iAttribs[i] == WGL_SAMPLES_ARB)        v = msaa;
        a[n++] = iAttribs[i];
        a[n++] = v;
    }
    a[n++] = WGL_DRAW_TO_PBUFFER_ARB;
    a[n++] = 1;
    a[n++] = 0;
    a[n] = 0;

    UINT count = 0;
    int fmt = 0;
    struct wgl_borrow b;
    wgl_borrow_ctx(&b);
    int chosen = wglChoosePixelFormatARB(wnd_dc, a, NULL, 1, &fmt, &count);
    if (!chosen || count < 1) {
        wgl_return_ctx(&b);
        DPRINTF("wglChoosePixelFormatARB(msaa %d) found nothing", msaa);
        return 0;
    }
    pixfmt = fmt;
    static const int q[] = {
        WGL_ALPHA_BITS_ARB, WGL_DEPTH_BITS_ARB, WGL_STENCIL_BITS_ARB,
        WGL_AUX_BUFFERS_ARB, WGL_SAMPLE_BUFFERS_ARB, WGL_SAMPLES_ARB,
    };
    int v[ARRAY_SIZE(q)] = { 0 };
    wglGetPixelFormatAttribivARB(wnd_dc, pixfmt, 0, ARRAY_SIZE(q), q, v);
    wgl_return_ctx(&b);
    cAlphaBits = v[0];
    cDepthBits = v[1];
    cStencilBits = v[2];
    cAuxBuffers = v[3];
    cSampleBuf[0] = v[4];
    cSampleBuf[1] = v[5];
    DPRINTF("pixel format %d: alpha %d depth %d stencil %d samples %d/%d",
            pixfmt, cAlphaBits, cDepthBits, cStencilBits, cSampleBuf[0], cSampleBuf[1]);
    return 1;
}

/* the DC a context is created on and made current with: the drawable
 * while there is one, the window before the first plat_drawable() */
static HDC draw_dc(void)
{
    return drawable_dc ? drawable_dc : wnd_dc;
}

static void *plat_ctx_create(void *share, const int *wgl)
{
    HGLRC c;
    /* The attribute list is already WGL's own (the guest sent it), so on
     * the one platform whose API it belongs to it needs no translation. */
    struct wgl_borrow b;
    wgl_borrow_ctx(&b);
    if (wgl && epoxy_has_wgl_extension(wnd_dc, "WGL_ARB_create_context")) {
        c = wglCreateContextAttribsARB(draw_dc(), (HGLRC)share, wgl);
        wgl_return_ctx(&b);
        if (!c) {
            WERR("wglCreateContextAttribsARB");
        }
        return c;
    }
    wgl_return_ctx(&b);
    c = wglCreateContext(draw_dc());
    if (!c) {
        WERR("wglCreateContext");
        return NULL;
    }
    if (share && !wglShareLists((HGLRC)share, c)) {
        /* a context that cannot share is worse than none: the guest's
         * textures live in the first one */
        WERR("wglShareLists");
        wglDeleteContext(c);
        return NULL;
    }
    return c;
}

static void plat_ctx_destroy(void *c)
{
    wglDeleteContext((HGLRC)c);
}

static void plat_unbind(void)
{
    wglMakeCurrent(NULL, NULL);
}

static int plat_make_current(void *c)
{
    if (!wglMakeCurrent(draw_dc(), (HGLRC)c)) {
        WERR("wglMakeCurrent");
        return 0;
    }
    return 1;
}

static void *plat_get_current(void)
{
    return wglGetCurrentContext();
}

/* (re)create the pbuffer that plays the window; keeps the context */
static int plat_drawable(int w, int h)
{
    const int a[] = { 0 };
    struct wgl_borrow b;
    wgl_borrow_ctx(&b);
    HPBUFFERARB p = wglCreatePbufferARB(wnd_dc, pixfmt, w, h, a);
    if (!p) {
        wgl_return_ctx(&b);
        WERR("wglCreatePbufferARB");
        return 0;
    }
    HDC dc = wglGetPbufferDCARB(p);
    if (!dc) {
        WERR("wglGetPbufferDCARB");
        wglDestroyPbufferARB(p);
        wgl_return_ctx(&b);
        return 0;
    }
    HPBUFFERARB old = drawable;
    HDC old_dc = drawable_dc;
    HGLRC cur = wglGetCurrentContext();
    drawable = p;
    drawable_dc = dc;
    if (cur && !wglMakeCurrent(dc, cur)) {
        WERR("wglMakeCurrent (new drawable)");
    }
    if (old) {
        wglReleasePbufferDCARB(old, old_dc);
        wglDestroyPbufferARB(old);
    }
    wgl_return_ctx(&b);
    return 1;
}

static void plat_drawable_release(void)
{
    if (drawable) {
        struct wgl_borrow b;
        wglMakeCurrent(NULL, NULL);
        wgl_borrow_ctx(&b);
        wglReleasePbufferDCARB(drawable, drawable_dc);
        wglDestroyPbufferARB(drawable);
        wgl_return_ctx(&b);
        drawable = NULL;
        drawable_dc = NULL;
    }
}

static int plat_present_zero_copy(void)
{
    /* No shared-surface path yet: a Windows frame is read back (the
     * dma-buf ring is Linux, IOSurface is macOS; DXGI is the one to
     * write here, docs/tracks/m11-windows-host.md). */
    return 0;
}

static int plat_readback(uint32_t *dst)
{
    GLint prev_fbo = 0, prev_pack = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prev_fbo);
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prev_pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, win_w, win_h, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, dst);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, prev_pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_fbo);
    return 1;
}

/* the guest's own WGL pbuffers, which here are what they say they are */
static int plat_pbuffer_create(int i, int w, int h)
{
    int a[8], n = 0;
    if (hPbuffer[i].format) {
        a[n++] = WGL_TEXTURE_FORMAT_ARB; a[n++] = hPbuffer[i].format;
    }
    if (hPbuffer[i].target) {
        a[n++] = WGL_TEXTURE_TARGET_ARB; a[n++] = hPbuffer[i].target;
    }
    a[n] = 0;
    struct wgl_borrow b;
    wgl_borrow_ctx(&b);
    pb[i] = wglCreatePbufferARB(wnd_dc, pixfmt, w, h, a);
    if (!pb[i]) {
        wgl_return_ctx(&b);
        WERR("wglCreatePbufferARB (guest)");
        return 0;
    }
    PBDC[i] = wglGetPbufferDCARB(pb[i]);
    wgl_return_ctx(&b);
    PBRC[i] = PBDC[i] ? wglCreateContext(PBDC[i]) : NULL;
    if (PBRC[i]) {
        HGLRC cur = wglGetCurrentContext();
        if (cur) {
            wglShareLists(cur, PBRC[i]);
        }
        return 1;
    }
    plat_pbuffer_destroy(i);
    return 0;
}

static void plat_pbuffer_destroy(int i)
{
    struct wgl_borrow b;
    if (PBRC[i]) {
        wglDeleteContext(PBRC[i]);
        PBRC[i] = NULL;
    }
    wgl_borrow_ctx(&b);
    if (PBDC[i]) {
        wglReleasePbufferDCARB(pb[i], PBDC[i]);
        PBDC[i] = NULL;
    }
    if (pb[i]) {
        wglDestroyPbufferARB(pb[i]);
        pb[i] = NULL;
    }
    wgl_return_ctx(&b);
}

static void plat_pbuffer_current(int i)
{
    if (PBDC[i] && PBRC[i]) {
        wglMakeCurrent(PBDC[i], PBRC[i]);
    }
}

static void plat_pbuffer_bind_tex(int i, int wgl_target, int wgl_format)
{
    int prev_binded_texture = 0;
    HGLRC prev_rc = wglGetCurrentContext();
    HDC prev_dc = wglGetCurrentDC();
    if (!PBDC[i] || !PBRC[i]) {
        return;
    }
    glGetIntegerv(PbufferGLBinding(wgl_target), &prev_binded_texture);
    wglMakeCurrent(PBDC[i], PBRC[i]);
    glBindTexture(PbufferGLAttrib(wgl_target), prev_binded_texture);
    glCopyTexImage2D(PbufferGLAttrib(wgl_target), hPbuffer[i].level,
        PbufferGLAttrib(wgl_format), 0, 0, hPbuffer[i].width, hPbuffer[i].height, 0);
    wglMakeCurrent(prev_dc, prev_rc);
}

static void *plat_get_proc(const char *name)
{
    /* wglGetProcAddress answers for everything above GL 1.1 and nothing
     * at or below it, which is what opengl32.dll's own exports are for. */
    void *p = (void *)wglGetProcAddress(name);
    if (!p) {
        HMODULE h = gl_dll ? (HMODULE)gl_dll : GetModuleHandle("opengl32.dll");
        p = h ? (void *)GetProcAddress(h, name) : NULL;
    }
    return p;
}

/*
 * The pbuffer is framebuffer 0 and has one colour buffer, the back one,
 * unless the config came out single-buffered, in which case it is the front
 * (see the shared hooks above).
 */
static int plat_default_fb(unsigned binding, unsigned *target)
{
    GLint fb = 0;
    GLboolean dbl = GL_FALSE;

    glGetIntegerv(binding, &fb);
    if (fb) {
        return 0;               /* an FBO of the guest's own */
    }
    glGetBooleanv(GL_DOUBLEBUFFER, &dbl);
    *target = dbl ? GL_BACK : GL_FRONT;
    return 1;
}

static void plat_set_func_ptr(void *hdll)
{
    gl_dll = hdll;
    install_buffer_hooks();
}

#else /* neither: no embed backend, the native (weak) one stays */

int embed_gl_available(void)
{
    return 0;
}

void embed_gl_drawable_size(int *w, int *h)
{
    *w = 640;
    *h = 480;
}

#endif
