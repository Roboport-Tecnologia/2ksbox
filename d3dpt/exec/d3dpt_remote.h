/*
 * d3dpt_remote.h: the wire between the out-of-process Direct3D executor's
 * two halves (docs/tracks/m15-wine-executor.md, ADR-018): QEMU's side,
 * libd3dpt_exec_remote (d3dpt_exec_remote.c, POSIX, the d3dpt_exec.h API
 * over a child process), and the child, d3dpt-exec-host.exe
 * (d3dpt_exec_host.c, a Windows program run under Wine, which loads the
 * ordinary d3dpt_exec.dll on Wine's own d3d9).
 *
 * Requests go down the child's stdin, replies come up its stdout, both as
 * fixed 32-byte records (little-endian both sides; x86_64 and arm64 hosts
 * agree on this layout). Everything with a size (the command window,
 * VRAM) lives in ONE shared file. QEMU
 * maps regions of it as guest RAM (memory_region_init_ram_from_fd), the
 * child maps the same regions with MapViewOfFile, and a request names a
 * region and an offset rather than carrying bytes. Every region starts at
 * a multiple of D3DPT_REMOTE_ALIGN in the file because Wine maps a view
 * at 64 KiB granularity.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT_REMOTE_H
#define D3DPT_REMOTE_H

#include <stdint.h>

#define D3DPT_REMOTE_VERSION    2u           /* 2: no frame slot (the guest DLLs' Present, M16 step 7) */
#define D3DPT_REMOTE_ALIGN      (1u << 20)
#define D3DPT_REMOTE_MAX_DIRTY  64u          /* vram_dirty ranges a reply carries before they fold into one */

enum {
    D3DPT_RQ_HELLO = 1, /* a0 = D3DPT_REMOTE_VERSION → ret = the DLL's D3DPT_PROTO_VERSION; status != 0: no d3d9 */
    D3DPT_RQ_MAP,       /* a0 = region id, a1 = size, a64 = file offset → the child maps it */
    D3DPT_RQ_CREATE,    /* → ret = executor id; status != 0: refused (no device) */
    D3DPT_RQ_DESTROY,   /* a0 = executor id */
    D3DPT_RQ_ATTACH,    /* a0 = id, a1 = 1 attach / 0 detach */
    D3DPT_RQ_SET_VRAM,  /* a0 = id, a1 = region id, a2 = size, a64 = offset in the region */
    D3DPT_RQ_SUBMIT,    /* a0 = id, a1 = region id, a2 = window size, a64 = offset → ret = D3DPT_ERR_* */
    D3DPT_RQ_QUIT,
};

typedef struct d3dpt_rq {
    uint32_t op, a0, a1, a2;
    uint64_t a64;
    uint64_t pad;
} d3dpt_rq;

typedef struct d3dpt_rp {
    uint32_t status;        /* 0 = done; else the child could not honour the request (its stderr says why) */
    uint32_t ret;           /* the call's own return value */
    uint32_t active;        /* the executor's "3D on" flag after the call */
    uint32_t ndirty;        /* d3dpt_rp_range records follow the reply */
    uint32_t pad[4];
} d3dpt_rp;

typedef struct d3dpt_rp_range {
    uint32_t offset, bytes; /* of the executor's VRAM */
} d3dpt_rp_range;

#endif
