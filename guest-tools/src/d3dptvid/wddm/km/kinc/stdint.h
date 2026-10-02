/*
 * kinc/stdint.h: the exact-width types for the WDDM kernel driver, whose
 * kernel-mode include path has no <stdint.h> (MSVC's own pulls in the
 * user-mode CRT). Only what the shared headers (d3dpt/d3dpt_fb.h) use.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_KINC_STDINT_H
#define D3DPT_KINC_STDINT_H

typedef signed char        int8_t;
typedef short              int16_t;
typedef int                int32_t;
typedef long long          int64_t;
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

#endif
