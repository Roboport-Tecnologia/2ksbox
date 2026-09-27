/*
 * ddtest.c: DirectDraw 7 test for the d3dpt-vga driver (doc 15, M7b).
 *
 *   DDTEST.EXE [w h bpp] [frames] [-windowed]
 *
 * Prints the HAL caps DirectDraw reports for the adapter (is there a HAL
 * at all, video memory, flip support), then runs an exclusive full-screen
 * flip chain. Each frame Lock/Unlock draws a moving pattern into the back
 * buffer, a Blt colour fill paints a bar, and Flip presents. Reports
 * whether the surfaces landed in video memory and the frame rate, and
 * dumps the last back buffer to ddtest.bmp. Everything also goes to
 * ddtest.log. At 8 bpp a 256-entry palette (four ramps) goes on the
 * primary and SetEntries rotates it by one entry per frame, the palette
 * animation of 2D titles; the BMP is written through that palette.
 * Last, a system-memory surface goes to the back buffer by Blt, BltFast,
 * a stretched Blt and a colour-keyed BltFast, the sprite path of 2D
 * titles, each read back ("sysmem blt ... ok"): with DDCAPS_BLT in the
 * driver's system-to-video caps these must still work.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../guestlog.h"

static FILE *logf;
static void logp(const char *fmt, ...)
{
    va_list ap;
    char buf[512];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    if (logf) {
        fputs(buf, logf);
        fflush(logf);
    }
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

/* where a frame's time goes: the performance counter around each of the
 * four calls, summed over the run (Windows 7's DirectDraw runtime paced
 * the loop at 15.6 ms steps where XP's ran at the refresh, track M18) */
static LARGE_INTEGER qpf;
static double call_ms[5];
static LONGLONG tick(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static void spent(int k, LONGLONG t0) { call_ms[k] += (double)(tick() - t0) * 1000.0 / (double)qpf.QuadPart; }

static void pump(void)
{
    MSG msg;
    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

static unsigned px_get(const DDSURFACEDESC2 *sd, unsigned x, unsigned y)
{
    const unsigned char *row = (const unsigned char *)sd->lpSurface + y * sd->lPitch;
    switch (sd->ddpfPixelFormat.dwRGBBitCount) {
    case 8: return row[x];
    case 16: return ((const unsigned short *)row)[x];
    default: return ((const unsigned *)row)[x] & 0xffffff;
    }
}

static void px_set(const DDSURFACEDESC2 *sd, unsigned x, unsigned y, unsigned v)
{
    unsigned char *row = (unsigned char *)sd->lpSurface + y * sd->lPitch;
    switch (sd->ddpfPixelFormat.dwRGBBitCount) {
    case 8: row[x] = (unsigned char)v; break;
    case 16: ((unsigned short *)row)[x] = (unsigned short)v; break;
    default: ((unsigned *)row)[x] = v & 0xffffff; break;
    }
}

/* the source pattern: never 0, which is the colour key */
static unsigned sys_px(unsigned x, unsigned y, unsigned bpp)
{
    unsigned v = x * 7 + y * 13 + 1;
    return bpp == 8 ? (v % 255) + 1 : bpp == 16 ? (v * 0x0841) % 0xffff + 1 : ((v * 0x010203) & 0xffffff) | 1;
}

/* A 32x32 system-memory surface to the video-memory back buffer by the
 * four calls a 2D title makes, each read back: Blt and BltFast 1:1 at
 * (0,0) and (40,0), Blt stretched 2x at (0,40), and BltFast with the
 * source colour key (the left half 0, keyed out) at (80,0) over a filled
 * background. */
static void sysmem_blts(IDirectDraw7 *dd, IDirectDrawSurface7 *back)
{
    IDirectDrawSurface7 *sys = NULL;
    DDSURFACEDESC2 sd, bd;
    DDCOLORKEY ck;
    DDBLTFX fx;
    RECT r, dr;
    HRESULT hr, h[4];
    unsigned x, y, bpp, bad[4] = {0, 0, 0, 0};

    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
    sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_SYSTEMMEMORY;
    sd.dwWidth = 32; sd.dwHeight = 32;
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &sys, NULL);
    if (FAILED(hr)) { logp("sysmem blt: CreateSurface %08lx  FAIL\n", hr); return; }
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    hr = sys->lpVtbl->Lock(sys, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
    if (FAILED(hr)) { logp("sysmem blt: Lock %08lx  FAIL\n", hr); sys->lpVtbl->Release(sys); return; }
    bpp = sd.ddpfPixelFormat.dwRGBBitCount;
    for (y = 0; y < 32; y++)
        for (x = 0; x < 32; x++) px_set(&sd, x, y, sys_px(x, y, bpp));
    sys->lpVtbl->Unlock(sys, NULL);

    memset(&fx, 0, sizeof(fx)); fx.dwSize = sizeof(fx);
    fx.dwFillColor = 0;
    back->lpVtbl->Blt(back, NULL, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
    r.left = 0; r.top = 0; r.right = 32; r.bottom = 32;
    dr = r;
    h[0] = back->lpVtbl->Blt(back, &dr, sys, &r, DDBLT_WAIT, NULL);
    h[1] = back->lpVtbl->BltFast(back, 40, 0, sys, &r, DDBLTFAST_WAIT);
    dr.left = 0; dr.top = 40; dr.right = 64; dr.bottom = 104;
    h[2] = back->lpVtbl->Blt(back, &dr, sys, &r, DDBLT_WAIT, NULL);
    /* the keyed one: the source's left half becomes the key colour 0,
     * over the target's fill of 0, which the pattern never has */
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    if (SUCCEEDED(sys->lpVtbl->Lock(sys, NULL, &sd, DDLOCK_WAIT, NULL))) {
        for (y = 0; y < 32; y++)
            for (x = 0; x < 16; x++) px_set(&sd, x, y, 0);
        sys->lpVtbl->Unlock(sys, NULL);
    }
    ck.dwColorSpaceLowValue = ck.dwColorSpaceHighValue = 0;
    sys->lpVtbl->SetColorKey(sys, DDCKEY_SRCBLT, &ck);
    h[3] = back->lpVtbl->BltFast(back, 80, 0, sys, &r, DDBLTFAST_WAIT | DDBLTFAST_SRCCOLORKEY);

    memset(&bd, 0, sizeof(bd)); bd.dwSize = sizeof(bd);
    hr = back->lpVtbl->Lock(back, NULL, &bd, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (FAILED(hr)) { logp("sysmem blt: Lock(back) %08lx  FAIL\n", hr); sys->lpVtbl->Release(sys); return; }
    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            unsigned want = sys_px(x, y, bpp);
            if (px_get(&bd, x, y) != want) bad[0]++;
            if (px_get(&bd, 40 + x, y) != want) bad[1]++;
            if (px_get(&bd, x * 2, 40 + y * 2) != want) bad[2]++;
            if (px_get(&bd, 80 + x, y) != (x < 16 ? 0u : want)) bad[3]++;
        }
    }
    back->lpVtbl->Unlock(back, NULL);
    sys->lpVtbl->Release(sys);
    logp("sysmem blt: Blt %08lx %s, BltFast %08lx %s, stretched Blt %08lx %s, keyed BltFast %08lx %s%s\n",
         h[0], bad[0] ? "WRONG" : "ok", h[1], bad[1] ? "WRONG" : "ok",
         h[2], bad[2] ? "WRONG" : "ok", h[3], bad[3] ? "WRONG" : "ok",
         (bad[0] | bad[1] | bad[2] | bad[3] | FAILED(h[0]) | FAILED(h[1]) | FAILED(h[2]) | FAILED(h[3])) ?
         "  FAIL" : "");
}

static PALETTEENTRY cur_pal[256];       /* what the primary's palette holds now */

static void dump_bmp(const char *path, const DDSURFACEDESC2 *sd)
{
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    FILE *f = fopen(path, "wb");
    unsigned w = sd->dwWidth, h = sd->dwHeight, bpp = sd->ddpfPixelFormat.dwRGBBitCount, y, x;
    unsigned char *row = malloc(w * 3);
    if (!f || !row) return;
    memset(&fh, 0, sizeof(fh)); memset(&ih, 0, sizeof(ih));
    fh.bfType = 0x4d42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + w * 3 * h;
    ih.biSize = sizeof(ih); ih.biWidth = w; ih.biHeight = h; ih.biPlanes = 1; ih.biBitCount = 24;
    fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
    for (y = h; y-- > 0;) {
        const unsigned char *src = (const unsigned char *)sd->lpSurface + y * sd->lPitch;
        for (x = 0; x < w; x++) {
            if (bpp == 32) {
                row[x * 3 + 0] = src[x * 4 + 0]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4 + 2];
            } else if (bpp == 8) {
                const PALETTEENTRY *e = &cur_pal[src[x]];
                row[x * 3 + 0] = e->peBlue; row[x * 3 + 1] = e->peGreen; row[x * 3 + 2] = e->peRed;
            } else {
                unsigned v = ((const unsigned short *)src)[x];
                row[x * 3 + 0] = (v & 0x1f) << 3; row[x * 3 + 1] = ((v >> 5) & 0x3f) << 2; row[x * 3 + 2] = ((v >> 11) & 0x1f) << 3;
            }
        }
        fwrite(row, 1, w * 3, f);
    }
    fclose(f);
    free(row);
}

static const char *caps_str(DWORD caps)
{
    static char buf[256];
    buf[0] = 0;
    if (caps & DDSCAPS_VIDEOMEMORY) strcat(buf, "VIDEOMEMORY ");
    if (caps & DDSCAPS_LOCALVIDMEM) strcat(buf, "LOCALVIDMEM ");
    if (caps & DDSCAPS_SYSTEMMEMORY) strcat(buf, "SYSTEMMEMORY ");
    if (caps & DDSCAPS_PRIMARYSURFACE) strcat(buf, "PRIMARY ");
    if (caps & DDSCAPS_BACKBUFFER) strcat(buf, "BACKBUFFER ");
    if (caps & DDSCAPS_FLIP) strcat(buf, "FLIP ");
    return buf;
}

int main(int argc, char **argv)
{
    int w = 640, h = 480, bpp = 16, frames = 300, windowed = 0, i, argn = 0;
    LPDIRECTDRAW7 dd = NULL;
    LPDIRECTDRAWSURFACE7 prim = NULL, back = NULL;
    LPDIRECTDRAWPALETTE pal = NULL;
    DDSURFACEDESC2 sd;
    DDSCAPS2 caps;
    DDCAPS hal, hel;
    HRESULT hr;
    WNDCLASSA wc;
    HWND hwnd;
    DWORD t0, t1;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-windowed")) windowed = 1;
        else if (argn == 0) { w = atoi(argv[i]); argn++; }
        else if (argn == 1) { h = atoi(argv[i]); argn++; }
        else if (argn == 2) { bpp = atoi(argv[i]); argn++; }
        else if (argn == 3) { frames = atoi(argv[i]); argn++; }
    }
    logf = guest_log_open("DDTEST.LOG", "w");
    logp("ddtest: %dx%d %d bpp, %d frames%s\n", w, h, bpp, frames, windowed ? ", windowed" : "");

    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "ddtest";
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    hwnd = CreateWindowExA(0, "ddtest", "ddtest", windowed ? WS_OVERLAPPEDWINDOW : WS_POPUP,
                           0, 0, w, h, NULL, NULL, wc.hInstance, NULL);
    ShowWindow(hwnd, SW_SHOW);
    pump();

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    if (FAILED(hr)) { logp("DirectDrawCreateEx failed %08lx\n", hr); return 1; }

    memset(&hal, 0, sizeof(hal)); hal.dwSize = sizeof(hal);
    memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(hel);
    hr = dd->lpVtbl->GetCaps(dd, &hal, &hel);
    logp("GetCaps %08lx: HAL dwCaps %08lx dwCaps2 %08lx ddsCaps %08lx palcaps %08lx vidmem %lu/%lu KiB\n", hr,
         hal.dwCaps, hal.dwCaps2, hal.ddsCaps.dwCaps, hal.dwPalCaps, hal.dwVidMemFree / 1024, hal.dwVidMemTotal / 1024);
    logp("  HAL: %s%s%s%s%s\n",
         hal.dwCaps & DDCAPS_NOHARDWARE ? "NOHARDWARE " : "HAL-present ",
         hal.dwCaps & DDCAPS_BLT ? "BLT " : "no-blt ",
         hal.dwCaps & DDCAPS_GDI ? "GDI " : "",
         hal.ddsCaps.dwCaps & DDSCAPS_FLIP ? "FLIP " : "no-flip ",
         hal.dwCaps & DDCAPS_3D ? "3D " : "no-3D ");

    hr = dd->lpVtbl->SetCooperativeLevel(dd, hwnd, windowed ? DDSCL_NORMAL :
                                         (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWREBOOT));
    logp("SetCooperativeLevel %08lx\n", hr);
    if (!windowed) {
        HDC hdc;
        hr = dd->lpVtbl->SetDisplayMode(dd, w, h, bpp, 0, 0);
        logp("SetDisplayMode %dx%dx%d %08lx\n", w, h, bpp, hr);
        if (FAILED(hr)) goto out;
        /* what the runtime rebuilt for the new mode: caps again, the mode's
         * pixel format, and GDI's view of the palette (8 bpp) */
        memset(&hal, 0, sizeof(hal)); hal.dwSize = sizeof(hal);
        memset(&hel, 0, sizeof(hel)); hel.dwSize = sizeof(hel);
        hr = dd->lpVtbl->GetCaps(dd, &hal, &hel);
        logp("after mode set: GetCaps %08lx HAL dwCaps %08lx palcaps %08lx ddsCaps %08lx\n", hr,
             hal.dwCaps, hal.dwPalCaps, hal.ddsCaps.dwCaps);
        memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
        hr = dd->lpVtbl->GetDisplayMode(dd, &sd);
        logp("GetDisplayMode %08lx: %lux%lu %lu bpp pf flags %08lx pitch %ld\n", hr, sd.dwWidth, sd.dwHeight,
             sd.ddpfPixelFormat.dwRGBBitCount, sd.ddpfPixelFormat.dwFlags, sd.lPitch);
        hdc = GetDC(NULL);
        logp("GDI: BITSPIXEL %d RASTERCAPS %08x SIZEPALETTE %d NUMRESERVED %d COLORRES %d\n",
             GetDeviceCaps(hdc, BITSPIXEL), GetDeviceCaps(hdc, RASTERCAPS), GetDeviceCaps(hdc, SIZEPALETTE),
             GetDeviceCaps(hdc, NUMRESERVED), GetDeviceCaps(hdc, COLORRES));
        ReleaseDC(NULL, hdc);
    }

    memset(&sd, 0, sizeof(sd));
    sd.dwSize = sizeof(sd);
    if (windowed) {
        sd.dwFlags = DDSD_CAPS;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
    } else {
        sd.dwFlags = DDSD_CAPS | DDSD_BACKBUFFERCOUNT;
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_FLIP | DDSCAPS_COMPLEX;
        sd.dwBackBufferCount = 1;
    }
    hr = dd->lpVtbl->CreateSurface(dd, &sd, &prim, NULL);
    logp("CreateSurface(primary%s) %08lx\n", windowed ? "" : " + 1 back buffer", hr);
    if (FAILED(hr) && !windowed) {
        /* a plain primary, then a system-memory one: which of them the mode allows */
        memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS; sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE;
        hr = dd->lpVtbl->CreateSurface(dd, &sd, &prim, NULL);
        logp("CreateSurface(plain primary) %08lx\n", hr);
        if (prim) { prim->lpVtbl->Release(prim); prim = NULL; }
        sd.ddsCaps.dwCaps = DDSCAPS_PRIMARYSURFACE | DDSCAPS_SYSTEMMEMORY;
        hr = dd->lpVtbl->CreateSurface(dd, &sd, &prim, NULL);
        logp("CreateSurface(sysmem primary) %08lx\n", hr);
        if (prim) { prim->lpVtbl->Release(prim); prim = NULL; }
        goto out;
    }
    if (FAILED(hr)) goto out;
    if (bpp == 8 && !windowed) {
        /* four ramps: red, green, blue, grey */
        for (i = 0; i < 256; i++) {
            unsigned v = (i & 63) * 255 / 63;
            cur_pal[i].peRed = (i >> 6) == 0 || (i >> 6) == 3 ? v : 0;
            cur_pal[i].peGreen = (i >> 6) == 1 || (i >> 6) == 3 ? v : 0;
            cur_pal[i].peBlue = (i >> 6) == 2 || (i >> 6) == 3 ? v : 0;
            cur_pal[i].peFlags = 0;
        }
        hr = dd->lpVtbl->CreatePalette(dd, DDPCAPS_8BIT | DDPCAPS_ALLOW256, cur_pal, &pal, NULL);
        logp("CreatePalette(8BIT|ALLOW256) %08lx\n", hr);
        if (FAILED(hr)) goto out;
        hr = prim->lpVtbl->SetPalette(prim, pal);
        logp("SetPalette(primary) %08lx\n", hr);
        if (FAILED(hr)) goto out;
    }
    if (windowed) {
        memset(&sd, 0, sizeof(sd));
        sd.dwSize = sizeof(sd);
        sd.dwFlags = DDSD_CAPS | DDSD_WIDTH | DDSD_HEIGHT;
        sd.ddsCaps.dwCaps = DDSCAPS_OFFSCREENPLAIN | DDSCAPS_VIDEOMEMORY;
        sd.dwWidth = w; sd.dwHeight = h;
        hr = dd->lpVtbl->CreateSurface(dd, &sd, &back, NULL);
        logp("CreateSurface(offscreen vidmem) %08lx\n", hr);
        if (FAILED(hr)) goto out;
    } else {
        memset(&caps, 0, sizeof(caps));
        caps.dwCaps = DDSCAPS_BACKBUFFER;
        hr = prim->lpVtbl->GetAttachedSurface(prim, &caps, &back);
        logp("GetAttachedSurface(back) %08lx\n", hr);
        if (FAILED(hr)) goto out;
    }
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    prim->lpVtbl->GetSurfaceDesc(prim, &sd);
    logp("primary: %lux%lu %lu bpp pitch %ld caps %08lx %s\n", sd.dwWidth, sd.dwHeight,
         sd.ddpfPixelFormat.dwRGBBitCount, sd.lPitch, sd.ddsCaps.dwCaps, caps_str(sd.ddsCaps.dwCaps));
    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    back->lpVtbl->GetSurfaceDesc(back, &sd);
    logp("back:    %lux%lu %lu bpp pitch %ld caps %08lx %s\n", sd.dwWidth, sd.dwHeight,
         sd.ddpfPixelFormat.dwRGBBitCount, sd.lPitch, sd.ddsCaps.dwCaps, caps_str(sd.ddsCaps.dwCaps));

    QueryPerformanceFrequency(&qpf);
    t0 = GetTickCount();
    for (i = 0; i < frames; i++) {
        DDBLTFX fx;
        RECT r;
        unsigned y, x;
        LONGLONG c0;

        c0 = tick();
        pump();
        spent(4, c0);
        memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
        c0 = tick();
        hr = back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, NULL);
        spent(0, c0);
        if (FAILED(hr)) { logp("Lock failed %08lx at frame %d\n", hr, i); goto out; }
        for (y = 0; y < sd.dwHeight; y++) {
            unsigned char *row = (unsigned char *)sd.lpSurface + y * sd.lPitch;
            unsigned c = ((y + i * 2) / 16) & 1;
            if (sd.ddpfPixelFormat.dwRGBBitCount == 32) {
                unsigned *p = (unsigned *)row;
                unsigned v = c ? 0x00204080 : 0x00c0a040;
                for (x = 0; x < sd.dwWidth; x++) p[x] = ((x + i) / 16 & 1) ? v : v ^ 0x00ffffff;
            } else if (sd.ddpfPixelFormat.dwRGBBitCount == 8) {
                /* diagonal bands through the whole palette; the checker
                 * flips the ramp (bit 7) */
                for (x = 0; x < sd.dwWidth; x++) row[x] = (unsigned char)(((x + y) / 2) ^ (c ? 0x80 : 0));
            } else {
                unsigned short *p = (unsigned short *)row;
                unsigned short v = c ? 0x2210 : 0xc528;
                for (x = 0; x < sd.dwWidth; x++) p[x] = ((x + i) / 16 & 1) ? v : v ^ 0xffff;
            }
        }
        c0 = tick();
        hr = back->lpVtbl->Unlock(back, NULL);
        spent(1, c0);
        if (FAILED(hr)) { logp("Unlock failed %08lx\n", hr); goto out; }

        /* a moving bar through Blt (HEL or HAL, whichever DirectDraw picks) */
        memset(&fx, 0, sizeof(fx)); fx.dwSize = sizeof(fx);
        fx.dwFillColor = sd.ddpfPixelFormat.dwRGBBitCount == 32 ? 0x00ff2020 :
                         sd.ddpfPixelFormat.dwRGBBitCount == 8 ? 255 : 0xf800;
        r.left = (i * 3) % (w - 40); r.right = r.left + 40; r.top = h / 4; r.bottom = h * 3 / 4;
        c0 = tick();
        hr = back->lpVtbl->Blt(back, &r, NULL, NULL, DDBLT_COLORFILL | DDBLT_WAIT, &fx);
        spent(2, c0);
        if (FAILED(hr)) { logp("Blt(COLORFILL) failed %08lx at frame %d\n", hr, i); goto out; }

        c0 = tick();
        if (windowed) {
            hr = prim->lpVtbl->Blt(prim, NULL, back, NULL, DDBLT_WAIT, NULL);
        } else {
            hr = prim->lpVtbl->Flip(prim, NULL, DDFLIP_WAIT);
        }
        spent(3, c0);
        if (FAILED(hr)) { logp("%s failed %08lx at frame %d\n", windowed ? "Blt(primary)" : "Flip", hr, i); goto out; }
        if (pal) {
            /* palette animation: rotate the 256 entries by one per frame */
            PALETTEENTRY first = cur_pal[0];
            memmove(&cur_pal[0], &cur_pal[1], 255 * sizeof(PALETTEENTRY));
            cur_pal[255] = first;
            hr = pal->lpVtbl->SetEntries(pal, 0, 0, 256, cur_pal);
            if (FAILED(hr)) { logp("SetEntries failed %08lx at frame %d\n", hr, i); goto out; }
        }
        if (i == 60) {
            t1 = GetTickCount();
            logp("first 60 frames: %lu ms\n", t1 - t0);
        }
    }
    t1 = GetTickCount();
    logp("%d frames in %lu ms = %.1f fps\n", frames, t1 - t0, t1 > t0 ? frames * 1000.0 / (t1 - t0) : 0.0);
    if (frames > 0)
        logp("per frame: Lock %.2f ms, Unlock %.2f, colour fill %.2f, %s %.2f, messages %.2f\n",
             call_ms[0] / frames, call_ms[1] / frames, call_ms[2] / frames, windowed ? "Blt(primary)" : "Flip",
             call_ms[3] / frames, call_ms[4] / frames);

    hr = dd->lpVtbl->WaitForVerticalBlank(dd, DDWAITVB_BLOCKBEGIN, NULL);
    logp("WaitForVerticalBlank %08lx\n", hr);

    /* GetVerticalBlankStatus in the loop a title of the era writes around
     * it, counting "in blank" answers over 500 ms. The adapter has no beam
     * position, so the driver says yes once a frame (about 30 at 60 Hz). A
     * driver that always says no hangs `while (!in_vb)` forever. */
    {
        DWORD t0 = GetTickCount(), polls = 0, yes = 0;
        BOOL in_vb;

        while (GetTickCount() - t0 < 500) {
            in_vb = FALSE;
            hr = dd->lpVtbl->GetVerticalBlankStatus(dd, &in_vb);
            polls++;
            if (hr == DD_OK && in_vb) yes++;
        }
        logp("GetVerticalBlankStatus: %lu of %lu polls in blank in 500 ms (last hr %08lx)%s\n",
             (unsigned long)yes, (unsigned long)polls, (unsigned long)hr,
             yes ? "" : "  FAIL: never in blank");
    }

    memset(&sd, 0, sizeof(sd)); sd.dwSize = sizeof(sd);
    hr = back->lpVtbl->Lock(back, NULL, &sd, DDLOCK_WAIT | DDLOCK_READONLY, NULL);
    if (SUCCEEDED(hr)) {
        char bmp[GUEST_PATHBUF];
        dump_bmp(guest_path(bmp, sizeof bmp, "DDTEST.BMP"), &sd);
        back->lpVtbl->Unlock(back, NULL);
        logp("%s written (last back buffer)\n", bmp);
    }
    sysmem_blts(dd, back);
out:
    if (back && !windowed) back = NULL;      /* attached: released with the primary */
    else if (back) back->lpVtbl->Release(back);
    if (pal) pal->lpVtbl->Release(pal);
    if (prim) prim->lpVtbl->Release(prim);
    if (dd) {
        if (!windowed) dd->lpVtbl->RestoreDisplayMode(dd);
        dd->lpVtbl->SetCooperativeLevel(dd, hwnd, DDSCL_NORMAL);
        dd->lpVtbl->Release(dd);
    }
    DestroyWindow(hwnd);
    logp("ddtest: done\n");
    if (logf) fclose(logf);
    return 0;
}
