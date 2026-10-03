/*
 * umd_core.c: the display driver's shared core inside the user-mode
 * driver (umd_core.h). The core asks its per-OS layer for a register
 * page (it reads DDFLAGS) and a log; here the page is a copy holding the
 * ddflags the kernel-mode driver reported, and the log is umd_log's.
 *
 * The caps are answered through core_gdi2_answer, the XP driver's
 * GetDriverInfo2 path, so both driver models say the same thing to their
 * d3d9.dll.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <ddraw.h>
#include "../../core/d3dpt_ddi.h"
#include "../../core/d3dpt_core.h"
#include "umd_core.h"

static ULONG g_regs[D3DPT_FB_REGS_SIZE / 4];
static d3dpt_core g_core;
static BOOL g_quiet;
static char g_line[160];
static ULONG g_line_n;

/* --- what the core asks of its layer --- */

ULONG ddflags(d3dpt_core *c)
{
    return c->regs ? c->regs[D3DPT_FB_REG_DDFLAGS / 4] : 0;
}

void dbg_puts(d3dpt_core *c, const char *s)
{
    (void)c;
    for (; *s; s++) {
        if (*s == '\n' || g_line_n == sizeof(g_line) - 1) {
            g_line[g_line_n] = 0;
            if (!g_quiet) {
                umd_log("%s", g_line);
            }
            g_line_n = 0;
            if (*s == '\n') {
                continue;
            }
        }
        g_line[g_line_n++] = *s;
    }
}

void dbg_hex(d3dpt_core *c, const char *tag, ULONG v)
{
    static const char hex[] = "0123456789abcdef";
    char buf[11];
    int i;

    dbg_puts(c, tag);
    buf[0] = '0';
    buf[1] = 'x';
    for (i = 0; i < 8; i++) {
        buf[2 + i] = hex[(v >> (28 - 4 * i)) & 0xf];
    }
    buf[10] = 0;
    dbg_puts(c, buf);
}

/* core_surf.c's DirectDraw surface walk (d3d_register_*) is linked in
 * beside its format arithmetic and never called here: the allocations
 * are the kernel-mode driver's to register. Its hooks answer nothing. */
void *d3dpt_os_alloc(ULONG bytes) { (void)bytes; return NULL; }
void d3dpt_os_free(void *p) { (void)p; }
BOOL d3dpt_os_surf(d3dpt_core *c, void *os, d3dpt_surf_desc *out) { (void)c; (void)os; (void)out; return FALSE; }
ULONG d3dpt_os_attached(void *os, void **out, ULONG max) { (void)os; (void)out; (void)max; return 0; }
ULONG d3dpt_os_attached_all(void *os, void **out, ULONG max) { (void)os; (void)out; (void)max; return 0; }
void *d3dpt_os_next_mip(void *os) { (void)os; return NULL; }
ULONG heap_end(d3dpt_core *c) { (void)c; return 0; }

/* --- umd_core.h --- */

void umd_caps_init(ULONG flags)
{
    ZeroMemory(g_regs, sizeof(g_regs));
    g_regs[D3DPT_FB_REG_DDFLAGS / 4] = flags;
    ZeroMemory(&g_core, sizeof(g_core));
    g_core.regs = g_regs;
    g_core.dx9 = TRUE;
    g_core.rt_dxver = 0x902;          /* d3d9.dll's, as XP's announces itself (DXVERSION) */
    d3d_caps_init(&g_core);
}

/* one GetDriverInfo2 question, answered quietly (the core logs each) */
static HRESULT gdi2(ULONG type, void *q, ULONG size)
{
    DD_GETDRIVERINFO2DATA_ *g = (DD_GETDRIVERINFO2DATA_ *)q;
    ULONG actual;
    HRESULT hr;

    g->dwReserved = 0;
    g->dwMagic = D3DGDI2_MAGIC_;
    g->dwType = type;
    g->dwExpectedSize = size;
    g_quiet = TRUE;
    hr = core_gdi2_answer(&g_core, q, &actual);
    g_quiet = FALSE;
    return hr;
}

HRESULT umd_caps9(void *out, UINT size)
{
    if (size != sizeof(d3d_caps9)) {
        umd_log("GetCaps(D3D9CAPS): the runtime's %u bytes, ours %u", size, (UINT)sizeof(d3d_caps9));
    }
    CopyMemory(out, &d3d_caps9, size < sizeof(d3d_caps9) ? size : sizeof(d3d_caps9));
    return S_OK;
}

/* the older runtimes' caps (d3d8.dll's D3DCAPS8; ddraw.dll's DX3..7
 * device: the HAL's global data and extended caps), the XP driver's */
static HRESULT caps_copy(const char *what, void *out, UINT size, const void *ours, UINT n)
{
    if (size != n) {
        umd_log("GetCaps(%s): the runtime's %u bytes, ours %u", what, size, n);
    }
    ZeroMemory(out, size);
    CopyMemory(out, ours, size < n ? size : n);
    return S_OK;
}

HRESULT umd_caps8(void *out, UINT size)
{
    return caps_copy("D3D8CAPS", out, size, &d3d_caps8, sizeof(d3d_caps8));
}

HRESULT umd_caps_hal(void *out, UINT size)
{
    return caps_copy("D3D3CAPS", out, size, &d3d_global, sizeof(d3d_global));
}

HRESULT umd_caps_ext(void *out, UINT size)
{
    return caps_copy("D3D7CAPS", out, size, &d3d_extcaps, sizeof(d3d_extcaps));
}

UINT umd_format_count(void)
{
    DD_GETFORMATCOUNTDATA_ f;

    ZeroMemory(&f, sizeof(f));
    return gdi2(D3DGDI2_TYPE_GETFORMATCOUNT_, &f, sizeof(f)) == DD_OK ? f.dwFormatCount : 0;
}

BOOL umd_format(UINT i, ULONG *fmt, ULONG *ops, ULONG *ms)
{
    DD_GETFORMATDATA_ f;

    ZeroMemory(&f, sizeof(f));
    f.dwFormatIndex = i;
    if (gdi2(D3DGDI2_TYPE_GETFORMAT_, &f, sizeof(f)) != DD_OK) {
        return FALSE;
    }
    *fmt = f.format.dwFourCC;
    *ops = f.format.dwRBitMask;               /* dwOperations */
    *ms = f.format.dwGBitMask;                /* MultiSampleCaps: wFlipMSTypes, wBltMSTypes */
    return TRUE;
}

UINT umd_ms_levels(ULONG fmt, ULONG type)
{
    DD_MULTISAMPLEQUALITYLEVELSDATA_ m;

    ZeroMemory(&m, sizeof(m));
    m.Format = fmt;
    m.MSType = type;
    return gdi2(D3DGDI2_TYPE_GETMULTISAMPLEQUALITYLEVELS_, &m, sizeof(m)) == DD_OK ? m.QualityLevels : 0;
}

ULONG umd_row_bytes(ULONG fmt, ULONG w)
{
    return fmt_row_bytes(fmt, w);
}

ULONG umd_rows(ULONG fmt, ULONG h)
{
    return surf_rows(fmt, h);
}

BOOL umd_is_dxt(ULONG fmt)
{
    return fmt_is_dxt(fmt);
}

ULONG umd_fill_pack(ULONG fmt, ULONG c, UCHAR *out)
{
    return fill_pack(fmt, c, out);
}

BOOL umd_px_unpack(ULONG fmt, const UCHAR *px, ULONG *c)
{
    return px_unpack(fmt, px, c);
}

void umd_px_copy(ULONG sfmt, ULONG dfmt, UCHAR *d, const UCHAR *s, ULONG bpp)
{
    px_copy(sfmt, dfmt, d, s, bpp);
}
