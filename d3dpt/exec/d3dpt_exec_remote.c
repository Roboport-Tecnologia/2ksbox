/*
 * d3dpt_exec_remote.c, libd3dpt_exec_remote: the Direct3D executor's
 * d3dpt_exec.h API implemented over a child process, for a Linux or macOS
 * host below DXVK's Vulkan 1.3 floor (docs/tracks/m15-wine-executor.md,
 * ADR-018). The child is d3dpt-exec-host.exe under Wine
 * (d3dpt_exec_host.c), running the Windows build of the very same
 * executor on Wine's own d3d9. QEMU's d3dpt_exec_load.c opens this library
 * exactly as it opens the in-process one (same six entry points, same
 * protocol version) after that one found no Vulkan device, so the two
 * d3dpt devices never learn which they got.
 *
 * Two things are added to the API for the devices that can use them:
 *   d3dpt_exec_shared_alloc(name, size, &offset) → an fd of the one shared
 *     file this library owns, with `offset` the region's place in it, for
 *     memory_region_init_ram_from_fd. The guest's VRAM and command window
 *     then ARE the bytes the child sees, and a batch runs where the guest
 *     wrote it;
 *   d3dpt_exec_shared_map(offset, ptr) → where QEMU mapped that region, so
 *     a pointer handed to submit()/set_vram() translates to a file offset.
 * A caller that hands submit() memory that is not in a shared region (the
 * host tests, with their malloc'ed windows) gets a COPY: the window's used
 * part goes into a shadow region before the request and the header, the
 * return area and the dirty VRAM ranges come back after. That is the test
 * path, and the log says "copying" once so nobody mistakes it for the real
 * thing; QEMU never takes it.
 *
 * Environment: D3DPT_WINE (the wine binary; else wine64/wine on PATH and
 * the Mac app locations), D3DPT_EXEC_HOST (the .exe; else wine/ beside
 * this library, or beside it), D3DPT_WINEPREFIX (else
 * the data directory's 2ksbox/wine: $XDG_DATA_HOME, ~/.local/share, or
 * ~/Library/Application Support on macOS), D3DPT_WINE_RENDERER (passed on: gl unless
 * told), D3DPT_REMOTE_DIR (where the shared file goes: else
 * $XDG_RUNTIME_DIR, $TMPDIR, /tmp). WINEDEBUG and WINEDLLOVERRIDES are set
 * for the child unless already in the environment.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "d3dpt_exec.h"
#include "d3dpt_remote.h"
#include "../d3dpt_proto.h"

extern char **environ;

#define MAX_REGIONS 16

typedef struct region {
    uint32_t id;
    uint32_t size;
    uint64_t off;
    void *ptr;          /* where this process mapped it (QEMU's RAM, or our own mmap) */
    int mapped;         /* the child has it */
} region;

static region regions[MAX_REGIONS];
static int nregions;
static uint32_t next_id = 1;
static uint64_t file_size;
static int shm_fd = -1;
static char shm_path[1024];

static pid_t child;
static int to_child = -1, from_child = -1;
static int child_failed;
static int child_hello;
static int copying_said;

struct d3dpt_exec {
    uint32_t id;
    d3dpt_exec_ops ops;
    int active;
    /* copy mode (host tests): the caller's memory and its shadow region */
    void *vram_copy; uint32_t vram_copy_size; region *vram_shadow;
    void *win_copy;  uint32_t win_copy_size;  region *win_shadow;
};

static void say(const char *fmt, ...)
{
    va_list ap;
    fputs("d3dpt-remote: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

/* ------------------------------------------------------------ the pipe */

static int write_full(int fd, const void *p, size_t n)
{
    const uint8_t *b = p;
    while (n) {
        ssize_t w = write(fd, b, n);
        if (w < 0) { if (errno == EINTR) continue; return 0; }
        if (w == 0) return 0;
        b += w; n -= (size_t)w;
    }
    return 1;
}

static int read_full(int fd, void *p, size_t n)
{
    uint8_t *b = p;
    while (n) {
        ssize_t r = read(fd, b, n);
        if (r < 0) { if (errno == EINTR) continue; return 0; }
        if (r == 0) return 0;
        b += r; n -= (size_t)r;
    }
    return 1;
}

static void child_gone(const char *why)
{
    if (!child_failed) {
        int st = 0;
        if (child > 0 && waitpid(child, &st, WNOHANG) == child) {
            child = 0;
            if (WIFEXITED(st)) say("%s (it exited with status %d; WINEDEBUG=warn+all shows Wine's reason); Direct3D pass-through off from here on", why, WEXITSTATUS(st));
            else say("%s (signal %d); Direct3D pass-through off from here on", why, WIFSIGNALED(st) ? WTERMSIG(st) : 0);
        } else {
            say("%s; Direct3D pass-through off from here on", why);
        }
    }
    child_failed = 1;
    if (to_child >= 0) { close(to_child); to_child = -1; }
    if (from_child >= 0) { close(from_child); from_child = -1; }
}

/* one request, one reply; the dirty ranges into `ranges` (up to MAX_DIRTY) */
static int call(const d3dpt_rq *q, d3dpt_rp *p, d3dpt_rp_range *ranges)
{
    if (child_failed || to_child < 0) return 0;
    if (!write_full(to_child, q, sizeof *q)) { child_gone("the executor process stopped reading"); return 0; }
    if (!read_full(from_child, p, sizeof *p)) { child_gone("the executor process went away"); return 0; }
    if (p->ndirty > D3DPT_REMOTE_MAX_DIRTY) { child_gone("the executor process answered nonsense"); return 0; }
    if (p->ndirty && !read_full(from_child, ranges, p->ndirty * sizeof *ranges)) { child_gone("the executor process went away"); return 0; }
    return 1;
}

/* ------------------------------------------------------ the shared file */

static int ensure_file(void)
{
    if (shm_fd >= 0) return 1;
    const char *dir = getenv("D3DPT_REMOTE_DIR");
    if (!dir || !*dir) dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir) dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(shm_path, sizeof shm_path, "%s/2ksbox-d3dpt-XXXXXX", dir);
    shm_fd = mkstemp(shm_path);
    if (shm_fd < 0) { say("cannot create the shared file in %s: %s", dir, strerror(errno)); return 0; }
    fcntl(shm_fd, F_SETFD, FD_CLOEXEC);
    return 1;
}

static region *alloc_region(uint32_t id, uint64_t size)
{
    if (nregions == MAX_REGIONS) { say("too many shared regions"); return NULL; }
    if (!ensure_file()) return NULL;
    uint64_t off = (file_size + D3DPT_REMOTE_ALIGN - 1) & ~(uint64_t)(D3DPT_REMOTE_ALIGN - 1);
    uint64_t end = off + size;
    if (ftruncate(shm_fd, (off_t)end) < 0) { say("cannot grow the shared file to %llu MiB: %s", (unsigned long long)(end >> 20), strerror(errno)); return NULL; }
    file_size = end;
    region *r = &regions[nregions++];
    r->id = id; r->off = off; r->size = (uint32_t)size; r->ptr = NULL; r->mapped = 0;
    return r;
}

static region *region_of(const void *ptr, uint32_t size)
{
    for (int i = 0; i < nregions; i++) {
        region *r = &regions[i];
        if (r->ptr && (const uint8_t *)ptr >= (const uint8_t *)r->ptr &&
            (const uint8_t *)ptr + size <= (const uint8_t *)r->ptr + r->size) return r;
    }
    return NULL;
}

static int map_in_child(region *r)
{
    if (r->mapped) return 1;
    d3dpt_rq q = { D3DPT_RQ_MAP, r->id, r->size, 0, r->off, 0 };
    d3dpt_rp p; d3dpt_rp_range dummy[D3DPT_REMOTE_MAX_DIRTY];
    if (!call(&q, &p, dummy) || p.status) { say("the executor process could not map region %u", r->id); return 0; }
    r->mapped = 1;
    return 1;
}

D3DPT_EXEC_API int d3dpt_exec_shared_alloc(const char *name, uint64_t size, uint64_t *offset)
{
    region *r = alloc_region(next_id++, size);
    if (!r) return -1;
    *offset = r->off;
    int fd = dup(shm_fd);
    say("shared region \"%s\": %llu MiB at %llu MiB of %s", name, (unsigned long long)(size >> 20), (unsigned long long)(r->off >> 20), shm_path);
    return fd;
}

D3DPT_EXEC_API void d3dpt_exec_shared_map(uint64_t offset, void *ptr)
{
    for (int i = 0; i < nregions; i++) if (regions[i].off == offset) { regions[i].ptr = ptr; return; }
    say("shared_map: no region at offset %llu", (unsigned long long)offset);
}

/* ------------------------------------------------------------ the child */

static int file_exists(const char *p) { struct stat st; return p && *p && stat(p, &st) == 0; }

/* the directory this library was loaded from, for the files a package
 * stages beside it (lib/2ksbox/) */
static int lib_dir(char *buf, size_t n)
{
    Dl_info info;
    if (!dladdr((void *)lib_dir, &info) || !info.dli_fname) return 0;
    const char *slash = strrchr(info.dli_fname, '/');
    if (!slash) return 0;
    snprintf(buf, n, "%.*s", (int)(slash - info.dli_fname), info.dli_fname);
    return 1;
}

static const char *find_wine(char *buf, size_t n)
{
    const char *env = getenv("D3DPT_WINE");
    if (env && *env) return env;
    /* a Wine the package carries beside this library: the Flatpak's add-on
     * (M15 step 7), which the player also names through D3DPT_WINE */
    char dir[768];
    if (lib_dir(dir, sizeof dir)) {
        snprintf(buf, n, "%s/wine/bin/wine", dir);
        if (file_exists(buf)) return buf;
    }
    const char *fixed[] = {
        "/Applications/Wine Stable.app/Contents/Resources/wine/bin/wine",
        "/Applications/Wine Staging.app/Contents/Resources/wine/bin/wine",
        "/Applications/Wine Devel.app/Contents/Resources/wine/bin/wine",
        "build/wine/Wine Staging.app/Contents/Resources/wine/bin/wine",
        "build/wine/Wine Stable.app/Contents/Resources/wine/bin/wine",
        NULL
    };
    for (int i = 0; fixed[i]; i++) if (file_exists(fixed[i])) return fixed[i];
    /* PATH: wine64 first (a distro's 64-bit binary), then wine */
    const char *path = getenv("PATH");
    const char *names[] = { "wine64", "wine", NULL };
    for (int k = 0; names[k]; k++) {
        const char *p = path ? path : "";
        while (*p) {
            const char *e = strchr(p, ':');
            size_t len = e ? (size_t)(e - p) : strlen(p);
            snprintf(buf, n, "%.*s/%s", (int)len, p, names[k]);
            if (access(buf, X_OK) == 0) return buf;
            if (!e) break;
            p = e + 1;
        }
    }
    return NULL;
}

static const char *find_host_exe(char *buf, size_t n)
{
    const char *env = getenv("D3DPT_EXEC_HOST");
    if (env && *env) return env;
    char dir[768];
    if (lib_dir(dir, sizeof dir)) {
        const char *sub[] = { "/wine/d3dpt-exec-host.exe", "/d3dpt-exec-host.exe" };
        for (int i = 0; i < 2; i++) {
            snprintf(buf, n, "%s%s", dir, sub[i]);
            if (file_exists(buf)) return buf;
        }
    }
    if (file_exists("build/d3dpt/wine/d3dpt-exec-host.exe")) return "build/d3dpt/wine/d3dpt-exec-host.exe";
    return NULL;
}

static void set_default_env(const char *name, const char *value)
{
    if (!getenv(name)) setenv(name, value, 0);
}

static void quit_child(void)
{
    if (child > 0 && to_child >= 0 && !child_failed) {
        d3dpt_rq q = { D3DPT_RQ_QUIT, 0, 0, 0, 0, 0 };
        write_full(to_child, &q, sizeof q);
    }
    if (to_child >= 0) close(to_child);
    if (from_child >= 0) close(from_child);
    to_child = from_child = -1;
    if (child > 0) { int st; waitpid(child, &st, 0); child = 0; }
    if (shm_fd >= 0) { close(shm_fd); shm_fd = -1; unlink(shm_path); }
}

static int ensure_child(void)
{
    if (child_hello) return 1;
    if (child_failed) return 0;
    if (!ensure_file()) { child_failed = 1; return 0; }

    char wbuf[1024], hbuf[1024];
    const char *wine = find_wine(wbuf, sizeof wbuf);
    const char *exe = find_host_exe(hbuf, sizeof hbuf);
    if (!wine) { say("no Wine on this host (D3DPT_WINE, wine/ beside this library, wine64/wine on PATH, a Wine app in /Applications)"); child_failed = 1; return 0; }
    if (!exe) { say("no d3dpt-exec-host.exe (D3DPT_EXEC_HOST, or wine/ beside this library; scripts/build-d3dpt-exec.sh builds it with mingw)"); child_failed = 1; return 0; }

    /* the child's environment: our own prefix, quiet, Wine's own d3d9 */
    char prefix[1024];
    const char *pfx = getenv("D3DPT_WINEPREFIX");
    if (pfx && *pfx) {
        snprintf(prefix, sizeof prefix, "%s", pfx);
    } else {
        /* under the user's data directory, where launcher_core::paths::data_dir() puts everything else */
        const char *xdg = getenv("XDG_DATA_HOME");
        const char *home = getenv("HOME") ? getenv("HOME") : ".";
        if (xdg && *xdg) snprintf(prefix, sizeof prefix, "%s/2ksbox/wine", xdg);
#ifdef __APPLE__
        else snprintf(prefix, sizeof prefix, "%s/Library/Application Support/2ksbox/wine", home);
#else
        else snprintf(prefix, sizeof prefix, "%s/.local/share/2ksbox/wine", home);
#endif
    }
    /* the prefix's parents: Wine makes the prefix itself, not the path to it */
    for (char *q = prefix + 1; *q; q++) {
        if (*q == '/') { *q = 0; mkdir(prefix, 0755); *q = '/'; }
    }
    setenv("WINEPREFIX", prefix, 1);
    set_default_env("WINEDEBUG", "-all");
    set_default_env("WINEDLLOVERRIDES", "d3d9=b;mscoree,mshtml=");   /* Wine's d3d9; no Mono / Gecko prompts in a new prefix */
    set_default_env("D3DPT_D3D9", "system");
    /* D3DPT_EXEC_LIB names *this* library to QEMU; to the child it must
     * name the DLL beside the .exe, so it is switched for the spawn */
    char dll[1024];
    const char *lib_env = getenv("D3DPT_EXEC_LIB");
    char *saved_lib = lib_env ? strdup(lib_env) : NULL;
    {
        const char *slash = strrchr(exe, '/');
        snprintf(dll, sizeof dll, "%.*s%sd3dpt_exec.dll", slash ? (int)(slash - exe + 1) : 0, exe, slash ? "" : "");
        setenv("D3DPT_EXEC_LIB", dll, 1);
    }
    struct stat st;
    int fresh = stat(prefix, &st) != 0;
    say("executor in another process: %s %s (prefix %s%s)", wine, exe, prefix, fresh ? ", created now: ~20 s" : "");

    int in_pipe[2], out_pipe[2];
    if (pipe(in_pipe) < 0 || pipe(out_pipe) < 0) { say("pipe: %s", strerror(errno)); child_failed = 1; return 0; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in_pipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out_pipe[1], 1);
    posix_spawn_file_actions_addclose(&fa, in_pipe[1]);
    posix_spawn_file_actions_addclose(&fa, out_pipe[0]);
    char *argv[] = { (char *)wine, (char *)exe, shm_path, NULL };
    int rc = posix_spawnp(&child, wine, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (saved_lib) { setenv("D3DPT_EXEC_LIB", saved_lib, 1); free(saved_lib); } else unsetenv("D3DPT_EXEC_LIB");
    close(in_pipe[0]); close(out_pipe[1]);
    if (rc) { say("cannot start %s: %s", wine, strerror(rc)); close(in_pipe[1]); close(out_pipe[0]); child_failed = 1; return 0; }
    to_child = in_pipe[1]; from_child = out_pipe[0];
    fcntl(to_child, F_SETFD, FD_CLOEXEC); fcntl(from_child, F_SETFD, FD_CLOEXEC);
    signal(SIGPIPE, SIG_IGN);
    atexit(quit_child);

    d3dpt_rq q = { D3DPT_RQ_HELLO, D3DPT_REMOTE_VERSION, 0, 0, 0, 0 };
    d3dpt_rp p; d3dpt_rp_range dummy[D3DPT_REMOTE_MAX_DIRTY];
    if (!call(&q, &p, dummy)) return 0;
    if (p.status) { child_gone("the executor process refused the wire version"); return 0; }
    if (p.ret != D3DPT_PROTO_VERSION) {
        say("the executor DLL in the other process speaks protocol %u, this library %u", p.ret, D3DPT_PROTO_VERSION);
        child_gone("protocol mismatch");
        return 0;
    }
    child_hello = 1;
    return 1;
}

/* ------------------------------------------------------------- the API */

D3DPT_EXEC_API uint32_t d3dpt_exec_version(void)
{
    return D3DPT_PROTO_VERSION;
}

/* The child's Direct3D device, made ahead of the first create(): under Wine
 * that is Direct3DCreate9 plus a device on wined3d's GL renderer, 3.5 s on
 * the Air under Rosetta, and the device makes it at the guest's first read
 * of the status register, inside an MMIO access, with the vCPU stopped.
 * XP took that; Windows 98 did not (D3D7TEST on base98-us: the
 * machine reset at that read and came back to the Startup Menu's "Windows
 * did not finish loading"). So the device is made here, at the probe the
 * adapter's realize runs before the guest boots, and create() collects it;
 * a second create() in the same process makes its own. */
static uint32_t ready_id;               /* an executor the child made at probe time (ids start at 0) */
static int have_ready;

static int make_device(uint32_t *id_out, const d3dpt_exec_ops *ops)
{
    d3dpt_rq q = { D3DPT_RQ_CREATE, 0, 0, 0, 0, 0 };
    d3dpt_rp p; d3dpt_rp_range dummy[D3DPT_REMOTE_MAX_DIRTY];
    if (!call(&q, &p, dummy) || p.status) {
        if (ops && ops->log) ops->log(ops->ud, "the executor in the other process found no usable Direct3D 9");
        else say("the executor in the other process found no usable Direct3D 9");
        return 0;
    }
    *id_out = p.ret;
    return 1;
}

D3DPT_EXEC_API int d3dpt_exec_probe(void)
{
    if (!ensure_child()) return 0;
    if (!have_ready) {
        if (!make_device(&ready_id, NULL)) {
            child_gone("no Direct3D 9 device in the executor process");
            return 0;
        }
        have_ready = 1;
    }
    return 1;
}

D3DPT_EXEC_API d3dpt_exec_t *d3dpt_exec_create(const d3dpt_exec_ops *ops)
{
    uint32_t id;

    if (!ensure_child()) return NULL;
    if (have_ready) {
        id = ready_id;
        have_ready = 0;
    } else if (!make_device(&id, ops)) {
        return NULL;
    }
    struct d3dpt_exec *x = calloc(1, sizeof *x);
    x->id = id;
    x->ops = *ops;
    if (ops->log) ops->log(ops->ud, "Direct3D executor in another process (Wine), ready");
    return x;
}

D3DPT_EXEC_API void d3dpt_exec_destroy(d3dpt_exec_t *x)
{
    if (!x) return;
    d3dpt_rq q = { D3DPT_RQ_DESTROY, x->id, 0, 0, 0, 0 };
    d3dpt_rp p; d3dpt_rp_range dummy[D3DPT_REMOTE_MAX_DIRTY];
    call(&q, &p, dummy);
    free(x);
}

D3DPT_EXEC_API void d3dpt_exec_attach(d3dpt_exec_t *x, int attach)
{
    d3dpt_rq q = { D3DPT_RQ_ATTACH, x->id, (uint32_t)(attach != 0), 0, 0, 0 };
    d3dpt_rp p; d3dpt_rp_range dummy[D3DPT_REMOTE_MAX_DIRTY];
    if (call(&q, &p, dummy) && (int)p.active != x->active) {
        x->active = (int)p.active;
        if (x->ops.active) x->ops.active(x->ops.ud, x->active);
    }
}

/* a caller's buffer that is not shared memory gets a shadow region */
static region *shadow_for(const char *what, uint32_t size)
{
    if (!copying_said++) say("%s is not in a shared region: copying (the host tests do this; QEMU never should)", what);
    region *r = alloc_region(next_id++, size);
    if (!r) return NULL;
    r->ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, (off_t)r->off);
    if (r->ptr == MAP_FAILED) { say("cannot map a shadow: %s", strerror(errno)); r->ptr = NULL; return NULL; }
    return r;
}

D3DPT_EXEC_API void d3dpt_exec_set_vram(d3dpt_exec_t *x, void *vram, uint32_t size)
{
    region *r = region_of(vram, size);
    uint64_t off;
    if (r) {
        off = (uint64_t)((uint8_t *)vram - (uint8_t *)r->ptr);
        x->vram_copy = NULL;
    } else {
        if (!x->vram_shadow || x->vram_copy_size != size) x->vram_shadow = shadow_for("VRAM", size);
        if (!x->vram_shadow) return;
        r = x->vram_shadow; off = 0;
        x->vram_copy = vram; x->vram_copy_size = size;
    }
    if (!map_in_child(r)) return;
    d3dpt_rq q = { D3DPT_RQ_SET_VRAM, x->id, r->id, size, off, 0 };
    d3dpt_rp p; d3dpt_rp_range dummy[D3DPT_REMOTE_MAX_DIRTY];
    if (!call(&q, &p, dummy) || p.status) say("set_vram refused");
}

D3DPT_EXEC_API uint32_t d3dpt_exec_submit(d3dpt_exec_t *x, void *shm, uint32_t shm_size)
{
    d3dpt_shm_hdr *hdr = shm;
    region *r = region_of(shm, shm_size);
    uint64_t off;
    int copy = 0;
    if (r) {
        off = (uint64_t)((uint8_t *)shm - (uint8_t *)r->ptr);
    } else {
        if (!x->win_shadow || x->win_copy_size != shm_size) x->win_shadow = shadow_for("the command window", shm_size);
        if (!x->win_shadow) { hdr->ret_status = D3DPT_ERR_HOST; return D3DPT_ERR_HOST; }
        r = x->win_shadow; off = 0; copy = 1;
        x->win_copy = shm; x->win_copy_size = shm_size;
        /* in: the header page and the records; the return area is the executor's to write */
        uint32_t used = D3DPT_CMD_OFFSET + hdr->cmd_bytes;
        if (used > shm_size) used = shm_size;
        memcpy(r->ptr, shm, used);
        if (x->vram_copy) memcpy(x->vram_shadow->ptr, x->vram_copy, x->vram_copy_size);
    }
    if (!map_in_child(r)) { hdr->ret_status = D3DPT_ERR_HOST; return D3DPT_ERR_HOST; }

    d3dpt_rq q = { D3DPT_RQ_SUBMIT, x->id, r->id, shm_size, off, 0 };
    d3dpt_rp p; d3dpt_rp_range ranges[D3DPT_REMOTE_MAX_DIRTY];
    if (!call(&q, &p, ranges) || p.status) {
        hdr->ret_status = D3DPT_ERR_HOST; hdr->cmd_bytes = 0; hdr->cmd_count = 0;
        return D3DPT_ERR_HOST;
    }
    if (copy) {
        /* out: the header, the return area, the dirty VRAM */
        memcpy(shm, r->ptr, sizeof(d3dpt_shm_hdr));
        if (shm_size > D3DPT_RET_OFFSET) memcpy((uint8_t *)shm + D3DPT_RET_OFFSET, (uint8_t *)r->ptr + D3DPT_RET_OFFSET, shm_size - D3DPT_RET_OFFSET);
        for (uint32_t i = 0; i < p.ndirty && x->vram_copy; i++) {
            uint64_t o = ranges[i].offset, n = ranges[i].bytes;
            if (o < x->vram_copy_size && n <= x->vram_copy_size - o)
                memcpy((uint8_t *)x->vram_copy + o, (uint8_t *)x->vram_shadow->ptr + o, n);
        }
    }
    if ((int)p.active != x->active) {
        x->active = (int)p.active;
        if (x->ops.active) x->ops.active(x->ops.ud, x->active);
    }
    for (uint32_t i = 0; i < p.ndirty; i++) if (x->ops.vram_dirty) x->ops.vram_dirty(x->ops.ud, ranges[i].offset, ranges[i].bytes);
    return p.ret;
}
