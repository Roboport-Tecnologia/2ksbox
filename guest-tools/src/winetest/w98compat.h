/*
 * w98compat.h: the Unicode display calls of Wine's Direct3D tests on
 * Windows 98 (track M16). guest-tools/build-winetests.sh force-includes
 * it into every test file.
 *
 * Win98's user32 exports EnumDisplaySettingsW, ChangeDisplaySettingsW,
 * GetMonitorInfoW and the rest as stubs that fail with
 * ERROR_CALL_NOT_IMPLEMENTED, so the tests' mode checks failed and the
 * device tests skipped nearly everything on the rig's Win98. Each wrapper
 * calls the real function first and falls back to the ANSI one only on
 * that error: on XP nothing changes. What a test checks never changes.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef W98COMPAT_H
#define W98COMPAT_H
/* the tests define it before their first include, and windows.h comes
 * first now */
#ifndef COBJMACROS
#define COBJMACROS
#endif
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define W98_API static __attribute__((unused))

W98_API BOOL w98_nocall(void)
{
    return GetLastError() == ERROR_CALL_NOT_IMPLEMENTED;
}

/* a device name: NULL stays NULL, the rest is ASCII (\\.\DISPLAY1) */
W98_API const char *w98_name(LPCWSTR w, char *a, size_t n)
{
    size_t i;

    if (!w) return NULL;
    for (i = 0; i + 1 < n && w[i]; i++) a[i] = (char)w[i];
    a[i] = 0;
    return a;
}

W98_API void w98_wname(WCHAR *w, const char *a, size_t n)
{
    size_t i;

    for (i = 0; i + 1 < n && a[i]; i++) w[i] = (unsigned char)a[i];
    w[i] = 0;
}

/* DEVMODEW <-> DEVMODEA: the fields between the two names and after the
 * form name are the same in both */
W98_API void w98_dm_a2w(DEVMODEW *w, const DEVMODEA *a)
{
    WORD size = w->dmSize, extra = w->dmDriverExtra;

    memcpy(&w->dmSpecVersion, &a->dmSpecVersion, offsetof(DEVMODEA, dmFormName) - offsetof(DEVMODEA, dmSpecVersion));
    memcpy(&w->dmLogPixels, &a->dmLogPixels, sizeof(DEVMODEA) - offsetof(DEVMODEA, dmLogPixels));
    w98_wname(w->dmDeviceName, (const char *)a->dmDeviceName, CCHDEVICENAME);
    w98_wname(w->dmFormName, (const char *)a->dmFormName, CCHFORMNAME);
    w->dmSize = size;
    w->dmDriverExtra = extra;
}

W98_API void w98_dm_w2a(DEVMODEA *a, const DEVMODEW *w)
{
    memset(a, 0, sizeof(*a));
    memcpy(&a->dmSpecVersion, &w->dmSpecVersion, offsetof(DEVMODEA, dmFormName) - offsetof(DEVMODEA, dmSpecVersion));
    memcpy(&a->dmLogPixels, &w->dmLogPixels, sizeof(DEVMODEA) - offsetof(DEVMODEA, dmLogPixels));
    w98_name(w->dmDeviceName, (char *)a->dmDeviceName, CCHDEVICENAME);
    w98_name(w->dmFormName, (char *)a->dmFormName, CCHFORMNAME);
    a->dmSize = sizeof(*a);
}

W98_API BOOL WINAPI w98_EnumDisplaySettingsExW(LPCWSTR dev, DWORD mode, DEVMODEW *dm, DWORD flags)
{
    DEVMODEA a;
    char n[CCHDEVICENAME];
    BOOL r = EnumDisplaySettingsExW(dev, mode, dm, flags);

    if (r || !w98_nocall()) return r;
    memset(&a, 0, sizeof(a));
    a.dmSize = sizeof(a);
    if (!EnumDisplaySettingsExA(w98_name(dev, n, sizeof(n)), mode, &a, flags)) return FALSE;
    w98_dm_a2w(dm, &a);
    return TRUE;
}

W98_API BOOL WINAPI w98_EnumDisplaySettingsW(LPCWSTR dev, DWORD mode, DEVMODEW *dm)
{
    DEVMODEA a;
    char n[CCHDEVICENAME];
    BOOL r = EnumDisplaySettingsW(dev, mode, dm);

    if (r || !w98_nocall()) return r;
    memset(&a, 0, sizeof(a));
    a.dmSize = sizeof(a);
    if (!EnumDisplaySettingsA(w98_name(dev, n, sizeof(n)), mode, &a)) return FALSE;
    w98_dm_a2w(dm, &a);
    return TRUE;
}

W98_API LONG WINAPI w98_ChangeDisplaySettingsExW(LPCWSTR dev, DEVMODEW *dm, HWND hwnd, DWORD flags, void *param)
{
    DEVMODEA a;
    char n[CCHDEVICENAME];
    LONG r;

    /* the stub's return value is not to be trusted (0 is success here) */
    SetLastError(0);
    r = ChangeDisplaySettingsExW(dev, dm, hwnd, flags, param);
    if (!w98_nocall()) return r;
    if (dm) w98_dm_w2a(&a, dm);
    return ChangeDisplaySettingsExA(w98_name(dev, n, sizeof(n)), dm ? &a : NULL, hwnd, flags, param);
}

W98_API LONG WINAPI w98_ChangeDisplaySettingsW(DEVMODEW *dm, DWORD flags)
{
    DEVMODEA a;
    LONG r;

    /* the stub's return value is not to be trusted (0 is success here) */
    SetLastError(0);
    r = ChangeDisplaySettingsW(dm, flags);
    if (!w98_nocall()) return r;
    if (dm) w98_dm_w2a(&a, dm);
    return ChangeDisplaySettingsA(dm ? &a : NULL, flags);
}

W98_API BOOL WINAPI w98_EnumDisplayDevicesW(LPCWSTR dev, DWORD i, DISPLAY_DEVICEW *dd, DWORD flags)
{
    DISPLAY_DEVICEA a;
    char n[CCHDEVICENAME];
    BOOL r = EnumDisplayDevicesW(dev, i, dd, flags);

    if (r || !w98_nocall()) return r;
    memset(&a, 0, sizeof(a));
    a.cb = sizeof(a);
    if (!EnumDisplayDevicesA(w98_name(dev, n, sizeof(n)), i, &a, flags)) return FALSE;
    w98_wname(dd->DeviceName, a.DeviceName, ARRAYSIZE(dd->DeviceName));
    w98_wname(dd->DeviceString, a.DeviceString, ARRAYSIZE(dd->DeviceString));
    dd->StateFlags = a.StateFlags;
    if (dd->cb >= sizeof(*dd)) {
        w98_wname(dd->DeviceID, a.DeviceID, ARRAYSIZE(dd->DeviceID));
        w98_wname(dd->DeviceKey, a.DeviceKey, ARRAYSIZE(dd->DeviceKey));
    }
    return TRUE;
}

W98_API BOOL WINAPI w98_GetMonitorInfoW(HMONITOR mon, MONITORINFO *mi)
{
    MONITORINFOEXA a;
    BOOL r = GetMonitorInfoW(mon, mi);

    if (r || !w98_nocall()) return r;
    memset(&a, 0, sizeof(a));
    a.cbSize = mi->cbSize >= sizeof(MONITORINFOEXW) ? sizeof(MONITORINFOEXA) : sizeof(MONITORINFO);
    if (!GetMonitorInfoA(mon, (MONITORINFO *)&a)) return FALSE;
    mi->rcMonitor = a.rcMonitor;
    mi->rcWork = a.rcWork;
    mi->dwFlags = a.dwFlags;
    if (mi->cbSize >= sizeof(MONITORINFOEXW)) w98_wname(((MONITORINFOEXW *)mi)->szDevice, a.szDevice, CCHDEVICENAME);
    return TRUE;
}

W98_API LONG WINAPI w98_GetWindowLongW(HWND hwnd, int i)
{
    LONG r;

    SetLastError(0);
    r = GetWindowLongW(hwnd, i);
    if (r || !w98_nocall()) return r;
    return GetWindowLongA(hwnd, i);
}

#define EnumDisplaySettingsExW w98_EnumDisplaySettingsExW
#define EnumDisplaySettingsW w98_EnumDisplaySettingsW
#define ChangeDisplaySettingsExW w98_ChangeDisplaySettingsExW
#define ChangeDisplaySettingsW w98_ChangeDisplaySettingsW
#define EnumDisplayDevicesW w98_EnumDisplayDevicesW
#define GetMonitorInfoW w98_GetMonitorInfoW
#define GetWindowLongW w98_GetWindowLongW

/* patches/winetest/04: a failed CreateDevice of the tests' create_device
 * is traced with what was asked (the rig's Win98 made no d3d8 device at
 * all and the tests only say they skipped). A macro, so the patch adds no
 * line: the baselines are keyed by source line */
#define WINETEST_TRACE_CREATE_DEVICE(hr, adapter, flags, pp) \
    trace("CreateDevice failed: hr %#lx, adapter %u, %ux%u format %#x, windowed %d, depth %d format %#x, " \
          "flags %#lx.\n", (hr), (unsigned)(adapter), (pp).BackBufferWidth, (pp).BackBufferHeight, \
          (pp).BackBufferFormat, (pp).Windowed, (pp).EnableAutoDepthStencil, (pp).AutoDepthStencilFormat, \
          (unsigned long)(flags))

/* patches/winetest/09: running the files on a machine where one test can
 * leave DirectDraw locked for the rest of the boot (the rig's Win98).
 *
 * WT_SKIP="a,b": START_TEST leaves those test functions out (wt_run).
 * WT_CANARY: after every test function wt_canary makes one plain device;
 * when that fails twice, a second apart, the test names the test before
 * it in <out>\STOP.TXT and the process ends. WTRUN then runs no further
 * file (every device would fail until a restart). <out> is WTRUN's
 * folder: BOXLOG\WINETEST, or C:\2KSBOX\WINETEST. */
W98_API int wt_run(const char *name)
{
    char skip[512], *p;
    size_t n = strlen(name);

    if (!GetEnvironmentVariableA("WT_SKIP", skip, sizeof skip)) return 1;
    for (p = skip; (p = strstr(p, name)); p += n) {
        if ((p == skip || p[-1] == ',') && (p[n] == 0 || p[n] == ',')) {
            printf("wt: %s left out (WT_SKIP)\n", name);
            return 0;
        }
    }
    return 1;
}

W98_API void wt_stop(const char *name)
{
    char path[MAX_PATH], dir[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("BOXLOG", dir, sizeof dir);
    HANDLE f;
    DWORD w;

    if (!n || n >= sizeof dir) strcpy(dir, "C:\\2KSBOX");
    n = (DWORD)strlen(dir);
    while (n && (dir[n - 1] == '\\' || dir[n - 1] == ' ')) dir[--n] = 0;
    snprintf(path, sizeof path, "%s\\WINETEST\\STOP.TXT", dir);
    f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        WriteFile(f, name, (DWORD)strlen(name), &w, NULL);
        CloseHandle(f);
    }
    printf("wt: no device can be made after %s: stopping (restart the machine)\n", name);
    fflush(stdout);
    ExitProcess(3);
}

#endif
