/*
 * d3dpt11.h: what the Direct3D 11 user-mode driver's files share
 * (d3dpt11.c: the adapter, formats and the command buffer; d11_res.c:
 * resources; d11_pipe.c: views, states, shaders, bindings and work;
 * d11_dev.c: the device and DXGI's functions; d10_dev.c: Direct3D 10's
 * and 10.1's interfaces; d11_video.c: the 11.1 interface's video
 * functions).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef D3DPT11_H
#define D3DPT11_H

/* the Windows 8 headers: the 11.1 interface and its video functions beside
 * Windows 7's (a callback table a Windows 7 runtime hands over is the
 * smaller one, so the driver keeps pointers to them, never copies) */
#define D3D_UMD_INTERFACE_VERSION D3D_UMD_INTERFACE_VERSION_WIN8
#define DXGKDDI_INTERFACE_VERSION DXGKDDI_INTERFACE_VERSION_WIN7
#include <windows.h>
#include <winternl.h>
#include <stdarg.h>
#include <stdio.h>
#include <d3d10umddi.h>
#include "../../../../../d3dpt/d3dpt_proto.h"
#include "../../../../../d3dpt/d3dpt_fb.h"
#include "../d3dpt_wddm.h"
#include "../umd_log.h"

/* the Direct3D 11 API's values the protocol speaks (d3d11.h is not
 * included beside the DDI's headers) */
#define API_BIND_UNORDERED_ACCESS 0x80u
#define API_CPU_WRITE 0x10000u
#define API_CPU_READ 0x20000u
#define API_MISC_TEXTURECUBE 0x4u
#define DXGI_DDI_ERR_WASSTILLDRAWING_ ((HRESULT)0x887B0001u)

#define D11_TODO(fn) do { static ULONG n_; if (n_ < 4) { n_++; umd_log("d3d11: todo: %s", fn); } } while (0)
/* every call, in a process HKLM\SOFTWARE\2ksbox's D3D11Trace names, and
 * nothing at all elsewhere (one flag read): a line is a register write a
 * character, a VM exit each, and a dozen of Windows 11's processes open
 * the adapter */
#include <intrin.h>
/* the profile's clock: the TSC on x86 / x64, the performance counter on Arm */
static __inline ULONG64 d11_ticks(void)
{
#if (defined(_M_IX86) || defined(_M_X64)) && !defined(_M_ARM64EC)
    return __rdtsc();
#else
    LARGE_INTEGER q;

    QueryPerformanceCounter(&q);
    return (ULONG64)q.QuadPart;
#endif
}
extern BOOL g_d11_verbose;
extern BOOL g_d11_msaa;                           /* multisampling offered to this process (d3dpt11.c) */
UINT fmt_msaa_counts(UINT f);                     /* the counts a format may claim: bit 0 2x .. bit 3 16x (d3dpt11.c) */
/* D3D11Prof (track M25): where this driver's time goes in the process,
 * in TSC cycles per kind, logged every 5 s at a present (d11_prof_tick) */
enum { D11P_RENDER, D11P_UPLOAD, D11P_MAP, D11P_UNMAP, D11P_QUERY, D11P_PRESENT, D11P_N };
extern BOOL g_d11_prof;
extern ULONG64 g_d11_prof_cyc[D11P_N], g_d11_prof_n[D11P_N], g_d11_prof_bytes[D11P_N], g_d11_calls;
#define D11_PROF_T0() (g_d11_prof ? d11_ticks() : 0)
#define D11_PROF_ADD(k, t0, b) do { if (g_d11_prof) { g_d11_prof_cyc[k] += d11_ticks() - (t0); g_d11_prof_n[k]++; g_d11_prof_bytes[k] += (b); } } while (0)
void d11_prof_tick(void);
#define D11_TRACE(fn) do { g_d11_calls++; if (g_d11_verbose) umd_log("d3d11: > %s", fn); } while (0)
/* a query asked by the thousand (formats, multisample counts): its first
 * calls only, even traced, or it spends the log's lines */
#define D11_TRACE_FEW(fn) do { static LONG n_; if (g_d11_verbose && InterlockedIncrement(&n_) <= 2) umd_log("d3d11: > %s", fn); } while (0)

typedef struct D11_ADAPTER {
    D3D10DDI_HRTADAPTER rt;
    const D3DDDI_ADAPTERCALLBACKS *cb;
    D3DPT_UMD_INFO info;
} D11_ADAPTER;

/* A resource, in the runtime's memory (CalcPrivateResourceSize): its
 * description in the protocol's terms, its allocation and the places of
 * its subresources (sub[], nsub of them) in VRAM (vram) or in the CPU's
 * copy of a dynamic one (shadow). */
typedef struct D11_SUB { UINT offset, row_pitch, depth_pitch, w, h, d; } D11_SUB;
typedef struct D11_RES {
    D3D10DDI_HRTRESOURCE rt;
    D3DKMT_HANDLE kmt;
    d3dpt_d11_res r;                              /* r.res stays 0: patched in each time a token names it */
    D3DPT_ALLOC_DESC a;                           /* the allocation's private data */
    BOOL shared, opened, vram;                    /* vram: the layout is the allocation's */
    BOOL d3d9;                                    /* described to the host as a Direct3D 9 surface too */
    BYTE *shadow, *sent;                          /* a dynamic resource: the CPU's copy, and what the host has */
    BYTE *shadow_mem;                             /* their heap block (shadow is it aligned to 64) */
    UINT shadow_bytes;
    UINT mapped;                                  /* subresource mapped + 1, 0: none */
    BOOL map_write;
    BOOL host_newer;                              /* in VRAM, and the host wrote it since: Map reads it back first */
    UINT nsub;
    D11_SUB sub[1];                               /* nsub of them */
} D11_RES;

/* every other object: the id the host knows it by */
typedef struct D11_OBJ {
    UINT id;
    UINT kind;                                    /* D3DPT_D11K_*; a shader's stage in `extra` */
    UINT extra;
    D11_RES *res;                                 /* a view's resource */
    /* a view: its creation as sent, made again when its resource's
     * identity rotates (a flip-model swap chain's buffers), in the
     * device's list of views */
    struct D11_OBJ *vnext, *vprev;
    UINT vop;
    d3dpt_d11_view view;
} D11_OBJ;

typedef struct D11_DEV {
    D3D10DDI_HRTDEVICE rt;
    D3D10DDI_HRTCORELAYER core;
    D11_ADAPTER *ad;
    const D3DDDI_DEVICECALLBACKS *kt;
    const D3D11DDI_CORELAYER_DEVICECALLBACKS *um;
    DXGI_DDI_BASE_CALLBACKS *dxgi;
    HANDLE ctx;                                   /* the kernel context */
    ULONG host_ctx;
    ULONG next_id;
    /* the command buffer, its allocation and patch lists */
    BYTE *buf;
    UINT buf_size, used, count;
    D3DDDI_ALLOCATIONLIST *al;
    UINT al_size, al_n;
    D3DDDI_PATCHLOCATIONLIST *pl;
    UINT pl_size, pl_n;
    /* each allocation's slot in the list, by a hash of its handle: valid
     * while gen is the buffer's al_gen (a linear walk of up to 512 slots
     * per patch cost a game's frame ~3,000 walks) */
    UINT al_gen;
    struct { UINT gen, idx; } al_hash[1024];
    UINT rec_off;                                 /* the open D11 record (0: none) */
    D11_OBJ *views;                               /* every view of this device (d11_pipe.c) */
    /* the output merger's UAV slots (ids; v34): the DDI names the slots it
     * changes, the host's API takes every bound one at once */
    UINT om_uav[64];
    BOOL om_uav_any;
    ULONG errors;
    BOOL v11_1;                                   /* made with the 11.1 interface (Windows 8 on) */
} D11_DEV;

#define DEV(h) ((D11_DEV *)(h).pDrvPrivate)
#define RES(h) ((D11_RES *)(h).pDrvPrivate)
#define OBJ(h) ((D11_OBJ *)(h).pDrvPrivate)

/* d3dpt11.c */
UINT fmt_bytes(UINT f, UINT *block);
UINT fmt_d3d9(UINT f);
UINT fmt_from_d3d9(UINT f);
UINT fmt_support(UINT f);
void cmd_reset(D11_DEV *d);
HRESULT cmd_flush(D11_DEV *d);
void rec_close(D11_DEV *d);
void *cmd_rec(D11_DEV *d, UINT op, UINT body, UINT nal);
void cmd_reserve(D11_DEV *d, UINT bytes, UINT nal);
void *tok(D11_DEV *d, UINT op, UINT body, UINT nal);
void *tok_z(D11_DEV *d, UINT op, UINT body, UINT zero, UINT nal);
UINT tok_max(D11_DEV *d);
UINT al_find(D11_DEV *d, D3DKMT_HANDLE kmt);
UINT al_slot(D11_DEV *d, D3DKMT_HANDLE kmt);
void patch(D11_DEV *d, void *p, D3DKMT_HANDLE kmt);
void patch_rw(D11_DEV *d, void *p, D3DKMT_HANDLE kmt, BOOL write);
void patch_res(D11_DEV *d, void *p, const D11_RES *r);
void patch_dst(D11_DEV *d, void *p, const D11_RES *r);
void set_error(D11_DEV *d, HRESULT hr);

/* d11_res.c */
void res_ref(D11_DEV *d, UINT op, D11_RES *r, UINT sub);
BYTE *res_lock(D11_DEV *d, D11_RES *r, BOOL donotwait, HRESULT *hr);
void res_unlock(D11_DEV *d, D11_RES *r);
void d11_res_funcs(D3D11DDI_DEVICEFUNCS *f);
void d11_res_funcs11_1(D3D11_1DDI_DEVICEFUNCS *f);

/* d11_pipe.c */
void d11_pipe_funcs(D3D11DDI_DEVICEFUNCS *f);
void d11_pipe_funcs11_1(D3D11_1DDI_DEVICEFUNCS *f);
void d11_pipe_destroy(D11_DEV *d);
void d11_views_rotated(D11_DEV *d, D11_RES *const *res, UINT n);

/* d11_dev.c */
void d11_device_funcs(D3D11DDI_DEVICEFUNCS *f);

/* d10_dev.c */
UINT d10_interface(UINT iface);
void d10_device_funcs(UINT ten, D3D10DDIARG_CREATEDEVICE *c);

/* d11_video.c */
void d11_video_funcs(D3D11_1DDI_VIDEODEVICEFUNCS *f);

#endif
