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
UINT umd_format_count(void);
BOOL umd_format(UINT i, ULONG *fmt, ULONG *ops, ULONG *ms);   /* ms: flip types low word, blt types high */
UINT umd_ms_levels(ULONG fmt, ULONG type);

/* the core's format arithmetic */
ULONG umd_row_bytes(ULONG fmt, ULONG w);
ULONG umd_rows(ULONG fmt, ULONG h);
BOOL umd_is_dxt(ULONG fmt);

/* d3dptumd.c: one line into the QEMU log */
void umd_log(const char *fmt, ...);

#endif
