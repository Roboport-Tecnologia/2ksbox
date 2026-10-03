/*
 * umd_core.h: what the user-mode driver takes from the display driver's
 * shared core (core/core_caps.c, core/core_surf.c), behind names of its
 * own: the core's headers (DirectDraw's types) and the D3DDDI headers do
 * not mix in one translation unit, so umd_core.c is the only one that
 * sees the core.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef UMD_CORE_H
#define UMD_CORE_H

/* the caps tables the XP driver's DX9 face hands d3d9.dll, built for the
 * adapter's ddflags */
void umd_caps_init(ULONG ddflags);
HRESULT umd_caps9(void *out, UINT size);           /* D3DCAPS9 */
HRESULT umd_caps8(void *out, UINT size);           /* D3DCAPS8 */
HRESULT umd_caps_hal(void *out, UINT size);        /* D3DHAL_GLOBALDRIVERDATA (DX3's device) */
HRESULT umd_caps_ext(void *out, UINT size);        /* D3DHAL_D3DEXTENDEDCAPS (DX5..7's) */
UINT umd_format_count(void);
BOOL umd_format(UINT i, ULONG *fmt, ULONG *ops, ULONG *ms);   /* ms: flip types low word, blt types high */
UINT umd_ms_levels(ULONG fmt, ULONG type);

/* the core's format arithmetic */
ULONG umd_row_bytes(ULONG fmt, ULONG w);
ULONG umd_rows(ULONG fmt, ULONG h);
BOOL umd_is_dxt(ULONG fmt);
/* the DX9 fills' and blits' pixels (core_surf.c): a D3DCOLOR in fmt's
 * layout (its bytes, 0: no such fill); one texel of the ARGB group as a
 * D3DCOLOR (FALSE: not in it); one texel from one format to another */
ULONG umd_fill_pack(ULONG fmt, ULONG c, UCHAR *out);
BOOL umd_px_unpack(ULONG fmt, const UCHAR *px, ULONG *c);
void umd_px_copy(ULONG sfmt, ULONG dfmt, UCHAR *d, const UCHAR *s, ULONG bpp);

/* d3dptumd.c: one line into the QEMU log */
void umd_log(const char *fmt, ...);

#endif
