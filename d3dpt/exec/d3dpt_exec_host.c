/*
 * d3dpt_exec_host.c, d3dpt-exec-host.exe: the Direct3D executor's other
 * process (docs/tracks/m15-wine-executor.md, ADR-018). A Windows program
 * run under Wine on a Linux or macOS host below DXVK's Vulkan 1.3 floor,
 * it loads the ordinary Windows build of the executor (d3dpt_exec.dll, the
 * decoder QEMU dlopens in process everywhere else) on Wine's own d3d9
 * (WineD3D over the host's OpenGL) and answers d3dpt_remote.h requests
 * from its stdin, which is QEMU's libd3dpt_exec_remote on the other end.
 *
 * It maps the regions QEMU names (the command window, VRAM) out of the
 * one shared file QEMU created, so a batch is executed where the guest
 * wrote it and a readback lands in VRAM with no copy; the executor's
 * callbacks (log, active, vram_dirty) become its stderr, a flag and a
 * list in the reply.
 *
 *   d3dpt-exec-host.exe <shared file, a Unix path>
 *
 * Environment (QEMU's side sets them; the header of d3dpt_exec_remote.c
 * says what each one defaults to): D3DPT_EXEC_LIB (the DLL; default: the
 * one beside this program), D3DPT_D3D9 (system, so the DLL takes the
 * d3d9.dll in Wine's system32, which is Wine's own), D3DPT_WINE_RENDERER
 * (wined3d's renderer, written into the prefix's registry: gl unless told).
 *
 * Build: x86_64-w64-mingw32-gcc -O2 -static -o d3dpt-exec-host.exe d3dpt_exec_host.c
 *        (scripts/build-d3dpt-exec.sh does it, into build/d3dpt/wine/)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "d3dpt_exec.h"
#include "d3dpt_remote.h"

#define MAX_REGIONS 16
#define MAX_EXECS   4

typedef struct region {
    uint32_t id;
    uint32_t size;
    uint64_t off;
    uint8_t *ptr;
    HANDLE map;
} region;

static region regions[MAX_REGIONS];
static int nregions;
static HANDLE hfile = INVALID_HANDLE_VALUE;
static HANDLE in_h, out_h;

static uint32_t (*p_version)(void);
static d3dpt_exec_t *(*p_create)(const d3dpt_exec_ops *);
static void (*p_destroy)(d3dpt_exec_t *);
static void (*p_attach)(d3dpt_exec_t *, int);
static uint32_t (*p_submit)(d3dpt_exec_t *, void *, uint32_t);
static void (*p_set_vram)(d3dpt_exec_t *, void *, uint32_t);

static d3dpt_exec_t *execs[MAX_EXECS];

/* what the executor's callbacks report during one request */
static d3dpt_rp cur;
static d3dpt_rp_range dirty[D3DPT_REMOTE_MAX_DIRTY];
static int active_flag;

static void say(const char *fmt, ...)
{
    va_list ap;
    fputs("d3dpt-exec-host: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static int read_full(void *p, DWORD n)
{
    uint8_t *b = p;
    while (n) {
        DWORD got = 0;
        if (!ReadFile(in_h, b, n, &got, NULL) || got == 0) return 0;
        b += got; n -= got;
    }
    return 1;
}

static int write_full(const void *p, DWORD n)
{
    const uint8_t *b = p;
    while (n) {
        DWORD put = 0;
        if (!WriteFile(out_h, b, n, &put, NULL) || put == 0) return 0;
        b += put; n -= put;
    }
    return 1;
}

static const region *find_region(uint32_t id)
{
    for (int i = 0; i < nregions; i++) if (regions[i].id == id) return &regions[i];
    return NULL;
}

/* ---- the executor's callbacks ---- */

static void cb_log(void *ud, const char *msg)
{
    (void)ud;
    say("exec: %s", msg);
}

static void cb_active(void *ud, int on)
{
    (void)ud;
    active_flag = on;
}

static void cb_vram_dirty(void *ud, uint32_t offset, uint32_t bytes)
{
    (void)ud;
    if (cur.ndirty < D3DPT_REMOTE_MAX_DIRTY) {
        dirty[cur.ndirty].offset = offset; dirty[cur.ndirty].bytes = bytes; cur.ndirty++;
        return;
    }
    /* full: one range over everything so far and this one. More scanout
     * work for the adapter, never a byte it does not see */
    uint64_t lo = offset, hi = (uint64_t)offset + bytes;
    for (uint32_t i = 0; i < cur.ndirty; i++) {
        if (dirty[i].offset < lo) lo = dirty[i].offset;
        if ((uint64_t)dirty[i].offset + dirty[i].bytes > hi) hi = (uint64_t)dirty[i].offset + dirty[i].bytes;
    }
    dirty[0].offset = (uint32_t)lo; dirty[0].bytes = (uint32_t)(hi - lo); cur.ndirty = 1;
}

static const d3dpt_exec_ops ops = { NULL, cb_log, cb_active, cb_vram_dirty };

/* ---- set-up ---- */

static int load_dll(void)
{
    char path[MAX_PATH];
    const char *env = getenv("D3DPT_EXEC_LIB");
    if (env && *env) {
        strncpy(path, env, sizeof path - 1); path[sizeof path - 1] = 0;
    } else {
        DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
        if (!n || n >= MAX_PATH) return 0;
        char *slash = strrchr(path, '\\');
        if (!slash) return 0;
        strcpy(slash + 1, "d3dpt_exec.dll");
    }
    HMODULE h = LoadLibraryA(path);
    if (!h) { say("cannot load %s (error %lu)", path, GetLastError()); return 0; }
    p_version = (void *)GetProcAddress(h, "d3dpt_exec_version");
    p_create = (void *)GetProcAddress(h, "d3dpt_exec_create");
    p_destroy = (void *)GetProcAddress(h, "d3dpt_exec_destroy");
    p_attach = (void *)GetProcAddress(h, "d3dpt_exec_attach");
    p_submit = (void *)GetProcAddress(h, "d3dpt_exec_submit");
    p_set_vram = (void *)GetProcAddress(h, "d3dpt_exec_set_vram");
    if (!p_version || !p_create || !p_destroy || !p_attach || !p_submit || !p_set_vram) {
        say("%s lacks the executor entry points", path);
        return 0;
    }
    say("executor %s, protocol %u", path, p_version());
    return 1;
}

/* Wine's own d3d9, and wined3d's renderer: a prefix of ours, so this is
 * the one place that decides both. */
static void set_up_wine(void)
{
    if (!GetEnvironmentVariableA("D3DPT_D3D9", NULL, 0)) {
        SetEnvironmentVariableA("D3DPT_D3D9", "system");
        _putenv("D3DPT_D3D9=system");
    }
    const char *renderer = getenv("D3DPT_WINE_RENDERER");
    if (!renderer || !*renderer) renderer = "gl";
    HKEY k;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\Wine\\Direct3D", 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, "renderer", 0, REG_SZ, (const BYTE *)renderer, (DWORD)strlen(renderer) + 1);
        RegCloseKey(k);
    }
}

static int open_shared(const char *unix_path)
{
    char path[MAX_PATH + 4];
    /* a Unix path is a Z: path in every prefix; forward slashes are fine */
    if (unix_path[0] == '/') snprintf(path, sizeof path, "Z:%s", unix_path);
    else snprintf(path, sizeof path, "%s", unix_path);
    hfile = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hfile == INVALID_HANDLE_VALUE) { say("cannot open the shared file %s (error %lu)", path, GetLastError()); return 0; }
    return 1;
}

static int map_region(uint32_t id, uint64_t off, uint32_t size)
{
    if (nregions == MAX_REGIONS) { say("too many regions"); return 0; }
    if (find_region(id)) { say("region %u mapped twice", id); return 0; }
    /* one mapping object per view: the file has grown since the last one */
    HANDLE m = CreateFileMappingA(hfile, NULL, PAGE_READWRITE, 0, 0, NULL);
    if (!m) { say("CreateFileMapping: error %lu", GetLastError()); return 0; }
    void *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, (DWORD)(off >> 32), (DWORD)off, size);
    if (!p) { say("MapViewOfFile(%llu, %u): error %lu", (unsigned long long)off, size, GetLastError()); CloseHandle(m); return 0; }
    region *r = &regions[nregions++];
    r->id = id; r->off = off; r->size = size; r->ptr = p; r->map = m;
    return 1;
}

static uint8_t *region_ptr(uint32_t id, uint64_t off, uint32_t size)
{
    const region *r = find_region(id);
    if (!r) { say("request names region %u, which is not mapped", id); return NULL; }
    if (off > r->size || size > r->size - off) { say("request outside region %u (%llu + %u > %u)", id, (unsigned long long)off, size, r->size); return NULL; }
    return r->ptr + off;
}

static d3dpt_exec_t *exec_of(uint32_t id)
{
    if (id >= MAX_EXECS || !execs[id]) { say("no executor %u", id); return NULL; }
    return execs[id];
}

int main(int argc, char **argv)
{
    if (argc < 2) { say("usage: d3dpt-exec-host.exe <shared file>"); return 2; }
    in_h = GetStdHandle(STD_INPUT_HANDLE);
    out_h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (in_h == INVALID_HANDLE_VALUE || out_h == INVALID_HANDLE_VALUE) { say("no stdio"); return 2; }
    set_up_wine();
    if (!load_dll()) return 3;
    if (!open_shared(argv[1])) return 4;

    for (;;) {
        d3dpt_rq q;
        d3dpt_rp p;
        if (!read_full(&q, sizeof q)) break;          /* QEMU went away: so do we */
        memset(&cur, 0, sizeof cur);
        p = cur;
        switch (q.op) {
        case D3DPT_RQ_HELLO:
            if (q.a0 != D3DPT_REMOTE_VERSION) { say("wire version %u, this program speaks %u", q.a0, D3DPT_REMOTE_VERSION); p.status = 1; }
            p.ret = p_version();
            break;
        case D3DPT_RQ_MAP:
            if (!map_region(q.a0, q.a64, q.a1)) p.status = 1;
            break;
        case D3DPT_RQ_CREATE: {
            uint32_t id;
            for (id = 0; id < MAX_EXECS && execs[id]; id++) ;
            if (id == MAX_EXECS) { say("too many executors"); p.status = 1; break; }
            execs[id] = p_create(&ops);
            if (!execs[id]) { p.status = 2; break; }
            p.ret = id;
            break;
        }
        case D3DPT_RQ_DESTROY: {
            d3dpt_exec_t *x = exec_of(q.a0);
            if (!x) { p.status = 1; break; }
            p_destroy(x);
            execs[q.a0] = NULL;
            break;
        }
        case D3DPT_RQ_ATTACH: {
            d3dpt_exec_t *x = exec_of(q.a0);
            if (!x) { p.status = 1; break; }
            p_attach(x, (int)q.a1);
            break;
        }
        case D3DPT_RQ_SET_VRAM: {
            d3dpt_exec_t *x = exec_of(q.a0);
            uint8_t *ptr = x ? region_ptr(q.a1, q.a64, q.a2) : NULL;
            if (!ptr) { p.status = 1; break; }
            p_set_vram(x, ptr, q.a2);
            break;
        }
        case D3DPT_RQ_SUBMIT: {
            d3dpt_exec_t *x = exec_of(q.a0);
            uint8_t *ptr = x ? region_ptr(q.a1, q.a64, q.a2) : NULL;
            if (!ptr) { p.status = 1; break; }
            p.ret = p_submit(x, ptr, q.a2);
            p.ndirty = cur.ndirty;
            break;
        }
        case D3DPT_RQ_QUIT:
            for (uint32_t id = 0; id < MAX_EXECS; id++) if (execs[id]) { p_destroy(execs[id]); execs[id] = NULL; }
            write_full(&p, sizeof p);
            return 0;
        default:
            say("unknown request %u", q.op);
            p.status = 1;
            break;
        }
        p.active = (uint32_t)active_flag;
        if (!write_full(&p, sizeof p)) break;
        if (p.ndirty && !write_full(dirty, p.ndirty * sizeof dirty[0])) break;
    }
    for (uint32_t id = 0; id < MAX_EXECS; id++) if (execs[id]) p_destroy(execs[id]);
    return 0;
}
