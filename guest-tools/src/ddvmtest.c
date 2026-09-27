/*
 * ddvmtest: what a DirectX 7-era launcher check sees. Creates a DirectDraw 7
 * object like GTA Vice City's psInitialize() does and prints total / free
 * video memory (GetAvailableVidMem, DDSCAPS_VIDEOMEMORY) plus the DDCAPS
 * figures, to the console and to DDVMTEST.LOG in C:\2KSBOX (guestlog.h). With the
 * system ddraw.dll on the Cirrus adapter this shows the card's 4 MB; on
 * d3dpt-vga, the display driver's video memory. Exit status 0 when free
 * memory is at least 12 MB (Vice City's threshold), 1 otherwise.
 *
 * Build (guest): i686-w64-mingw32-gcc -O2 -o ddvmtest.exe ddvmtest.c -lddraw -ldxguid
 */
#define COBJMACROS
#include <windows.h>
#include <ddraw.h>
#include <stdio.h>
#include <string.h>
#include "guestlog.h"

static FILE *g_log;
static void out(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("%s\n", line);
    if (g_log) { fprintf(g_log, "%s\n", line); fflush(g_log); }
}

int main(int argc, char **argv)
{
    char path[MAX_PATH];
    IDirectDraw7 *dd = NULL;
    DDSCAPS2 caps;
    DDCAPS hal, hel;
    DWORD total = 0, avail = 0;
    HRESULT hr;
    HMODULE m;

    g_log = guest_log_open("DDVMTEST.LOG", "wt");
    printf("log: %s\n", guest_log_path());

    hr = DirectDrawCreateEx(NULL, (void **)&dd, &IID_IDirectDraw7, NULL);
    out("ddvmtest: DirectDrawCreateEx -> 0x%08lx", (unsigned long)hr);
    if (FAILED(hr) || !dd) return 2;
    m = GetModuleHandleA("ddraw.dll");
    if (m && GetModuleFileNameA(m, path, sizeof path)) out("ddvmtest: ddraw.dll is %s", path);

    memset(&caps, 0, sizeof caps);
    caps.dwCaps = DDSCAPS_VIDEOMEMORY;
    hr = IDirectDraw7_GetAvailableVidMem(dd, &caps, &total, &avail);
    out("ddvmtest: GetAvailableVidMem(VIDEOMEMORY) -> 0x%08lx total %lu MB free %lu MB",
        (unsigned long)hr, (unsigned long)(total >> 20), (unsigned long)(avail >> 20));
    memset(&hal, 0, sizeof hal); hal.dwSize = sizeof hal;
    memset(&hel, 0, sizeof hel); hel.dwSize = sizeof hel;
    hr = IDirectDraw7_GetCaps(dd, &hal, &hel);
    out("ddvmtest: GetCaps -> 0x%08lx dwVidMemTotal %lu MB dwVidMemFree %lu MB",
        (unsigned long)hr, (unsigned long)(hal.dwVidMemTotal >> 20), (unsigned long)(hal.dwVidMemFree >> 20));
    {   /* a QueryInterface'd reference released again must leave the object alive (a
         * shim QI that takes no reference on the real object lets this Release free it) */
        IDirectDraw7 *dd2 = NULL;
        DWORD t2 = 0, a2 = 0;
        hr = IDirectDraw7_QueryInterface(dd, &IID_IDirectDraw7, (void **)&dd2);
        if (SUCCEEDED(hr) && dd2) IDirectDraw7_Release(dd2);
        hr = IDirectDraw7_GetAvailableVidMem(dd, &caps, &t2, &a2);
        out("ddvmtest: after QueryInterface + Release: GetAvailableVidMem -> 0x%08lx free %lu MB", (unsigned long)hr, (unsigned long)(a2 >> 20));
    }
    IDirectDraw7_Release(dd);
    out("ddvmtest: %s (Vice City needs 12 MB free)", avail >= (12u << 20) ? "enough" : "NOT ENOUGH");
    if (g_log) fclose(g_log);
    return avail >= (12u << 20) ? 0 : 1;
}
