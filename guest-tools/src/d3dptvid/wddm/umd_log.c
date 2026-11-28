/*
 * umd_log.c: the WDDM user-mode drivers' log line (Direct3D 9's
 * um/d3dptumd.c, Direct3D 11's um11/d3dpt11.c): an escape to the kernel
 * driver, which writes it to the device's DEBUG register, the QEMU log
 * beside its own lines (doc 15's rule).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <winternl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <d3dkmthk.h>
#include "d3dpt_wddm.h"
#include "umd_log.h"

/* this process in a list of executable names under HKLM\SOFTWARE\2ksbox
 * (REG_MULTI_SZ; "*" for every process): D3D11Exclude keeps a process on
 * Direct3D 9's driver (d3d10level9), D3D11Trace logs every call,
 * D3D9Trace logs the Direct3D 9 driver's routine lines and lifts its cap,
 * D3D11Win7 offers only the Windows 7 interface (an A/B for the 11.1 one),
 * D3D11FL10 keeps a process at feature level 10_1 (11_0 is offered to every
 * other since 2026-11-27, track M25), D3D11NoMSAA offers a process no
 * multisampling */
#ifndef RRF_SUBKEY_WOW6464KEY
#define RRF_SUBKEY_WOW6464KEY 0x00010000          /* winreg.h after the EWDK's 10.0.19041 */
#endif

BOOL umd_listed(const char *value)
{
    char names[1024], exe[MAX_PATH], *base;
    DWORD size = sizeof(names) - 2;
    const char *n;

    ZeroMemory(names, sizeof(names));
    /* the 64-bit view in a WoW64 process too, not WOW6432Node */
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\2ksbox", value, RRF_RT_REG_MULTI_SZ | RRF_SUBKEY_WOW6464KEY, NULL, names,
                     &size) != ERROR_SUCCESS) {
        return FALSE;
    }
    if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) {
        return FALSE;
    }
    base = strrchr(exe, '\\');
    base = base ? base + 1 : exe;
    for (n = names; *n; n += strlen(n) + 1) {
        if (!strcmp(n, "*") || !_stricmp(n, base)) {
            return TRUE;
        }
    }
    return FALSE;
}

static D3DKMT_HANDLE g_kmt_adapter;
static ULONG g_log_lines;

/* lines per process; a frame loop must not drown the QEMU log, and each
 * character is a register write, an exit to the host (a VM exit under
 * HVF, track M25): 4000 slowed Windows 11 on Arm's desktop */
#define UMD_LOG_MAX 400
#define UMD_LOG_TRACED 400000     /* a traced one: DWM's idle composition spends 4000 in seconds */

static ULONG g_log_max = UMD_LOG_MAX;

void umd_log_traced(void)
{
    g_log_max = UMD_LOG_TRACED;
}

static NTSTATUS umd_escape_log(D3DKMT_HANDLE adapter, D3DPT_ESC *m)
{
    D3DKMT_ESCAPE e;

    ZeroMemory(&e, sizeof(e));
    e.hAdapter = adapter;
    e.Type = D3DKMT_ESCAPE_DRIVERPRIVATE;
    e.pPrivateDriverData = m;
    e.PrivateDriverDataSize = sizeof(*m);
    return D3DKMTEscape(&e);
}

void umd_log(const char *fmt, ...)
{
    D3DPT_ESC m;
    va_list ap;

    if (g_log_lines >= g_log_max) {
        return;
    }
    g_log_lines++;
    ZeroMemory(&m, sizeof(m));
    m.magic = D3DPT_ESC_MAGIC;
    m.op = D3DPT_ESC_LOG;
    /* the process first: a dozen of Windows 11's open the adapter at once */
    {
        int n = _snprintf_s(m.text, sizeof(m.text), _TRUNCATE, "[%lu] ", GetCurrentProcessId());

        va_start(ap, fmt);
        _vsnprintf_s(m.text + n, sizeof(m.text) - n, _TRUNCATE, fmt, ap);
        va_end(ap);
    }
    if (g_kmt_adapter) {
        umd_escape_log(g_kmt_adapter, &m);
        return;
    }
    /* Ours is the adapter that takes the escape: on Windows 11 the
     * standard VGA's Basic Display is a display of its own, often the
     * first, and refuses a private escape (track M20 step 5). */
    for (int i = 1; i <= 16; i++) {
        D3DKMT_OPENADAPTERFROMGDIDISPLAYNAME o;
        D3DKMT_CLOSEADAPTER ca;

        ZeroMemory(&o, sizeof(o));
        wsprintfW(o.DeviceName, L"\\\\.\\DISPLAY%d", i);
        if (D3DKMTOpenAdapterFromGdiDisplayName(&o) != 0) {
            continue;
        }
        if (umd_escape_log(o.hAdapter, &m) == 0) {
            g_kmt_adapter = o.hAdapter;
            return;
        }
        ca.hAdapter = o.hAdapter;
        D3DKMTCloseAdapter(&ca);
    }
}
