/*
 * setup.c: SETUP.EXE, the guest-tools installer, run inside the machine.
 *
 * Which files go where differs per Windows family, and one of them
 * (FXPTL.SYS) needs a service registered before OPENGL32.DLL will load.
 * SETUP installs the components that apply to *this* Windows and logs
 * what happened.
 *
 * Guest tools come in two kinds and so does this program:
 *
 *   - things installed into Windows (system files, a driver, a service),
 *     the numbered list and `I`;
 *   - things copied next to one game's EXE (our D3D DLLs, the GL wrapper),
 *     `G`. Those are per-game by design, never system-wide, so
 *     an installer with only an install step would leave out half the ISO.
 *
 * A console program on purpose. It is the one interface Windows 98, XP
 * and a rescue command prompt all have, it needs no common controls, and
 * every step is scriptable. `SETUP /ALL` in a batch file is how our own
 * headless guest tests install the tools.
 *
 * Everything it installs comes from the folder SETUP.EXE is in, so it
 * works from the CD, from a copy on the hard disk, or from a share.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <mmsystem.h>
#include <winsvc.h>
#include <shlobj.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "guestlog.h"

#define MAX_COMPONENTS 8

/* Where the test programs and the log go: one folder of ours on the hard
 * disk, the same every time, not WINDOWS, where a SETUP.LOG would sit
 * among every other installer's. Every other program here logs there too;
 * guestlog.h holds the one definition. */
#define BOXDIR GUEST_DIR

/* Every path here is <the SETUP.EXE folder> + <folder> + <name>, so the
 * buffers are deliberately wider than MAX_PATH: a root path close to the
 * limit plus a subfolder is longer than MAX_PATH, and silently truncating
 * it would look for the wrong file. */
#define PATHBUF (MAX_PATH * 2)

static char g_root[MAX_PATH];    /* the folder SETUP.EXE lives in */
static char g_sys[MAX_PATH];     /* WINDOWS\SYSTEM or WINDOWS\system32 */
static char g_win[MAX_PATH];     /* WINDOWS */
static int g_nt;                 /* 2000/XP rather than 98/Me */
static int g_reboot;             /* a step said the machine must restart */
static int g_installing;         /* inside install_selected: a locked system file may be replaced on the next boot rather than failing */
static FILE *g_log;
static char g_log_path[PATHBUF];  /* where the log actually went, as an absolute path */

/* `rel` under the SETUP.EXE folder, in a caller-provided buffer. */
static const char *iso(char *buf, const char *rel)
{
    snprintf(buf, PATHBUF, "%s%s", g_root, rel);
    return buf;
}

/* Everything the user sees also goes to the log file: a console window
 * scrolls, and on a failed install the log is what gets pasted into a bug
 * report. Line-flushed, so a crash keeps the tail. */
static void say(const char *fmt, ...)
{
    va_list ap;
    char buf[1024];

    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    printf("%s\n", buf);
    fflush(stdout);
    if (g_log) { fprintf(g_log, "%s\n", buf); fflush(g_log); }
}

/* ------------------------------------------------------------ file steps */

/* A file that must not be overwritten where it is, because it is in use
 * (the mapper .SYS its service has loaded) or is a module of a driver
 * Windows may be running right now, is not a failure. Stage the new copy
 * beside the target, on the hard disk where a boot-time rename can
 * still find it once the CD is gone, and schedule the swap for the next
 * restart. NT has MoveFileEx for exactly this; 9x has no such call and does
 * it through WINDOWS\WININIT.INI, whose [rename] section WININIT.EXE applies
 * once, before the GUI, on the next boot (`Dest=Src` renames Src to Dest).
 * The driver step reboots anyway, so the new file is live after the restart
 * it was already going to ask for. `why` is the word for the log. */
static int replace_on_reboot(const char *src, const char *dstdir, const char *name,
                             const char *why)
{
    char dst[PATHBUF], stage[PATHBUF], base[MAX_PATH], ext[8], *dot;
    size_t n;

    snprintf(dst, sizeof dst, "%s\\%s", dstdir, name);
    /* an 8.3-safe sibling with the extension's last character made `_`
     * (D3DPT9X.DR_, D3DPT9X.IN_): WININIT.INI needs short names, and a
     * BASE.EXT.NEW would be a long name with a generated alias. The stage
     * name must also differ per extension, because the 9x driver's INF and
     * .DRV share a base name in the same folder. */
    lstrcpynA(base, name, sizeof base);
    ext[0] = 0;
    if ((dot = strrchr(base, '.')) != NULL) {
        lstrcpynA(ext, dot + 1, sizeof ext);
        *dot = 0;
    }
    n = strlen(ext);
    if (n >= 3) ext[2] = '_', ext[3] = 0;
    else ext[n] = '_', ext[n + 1] = 0;
    snprintf(stage, sizeof stage, "%s\\%s.%s", dstdir, base, ext);

    SetFileAttributesA(stage, FILE_ATTRIBUTE_NORMAL);
    if (!CopyFileA(src, stage, FALSE)) {
        say("    %s: in use, and staging a replacement failed (error %lu)", name, (unsigned long)GetLastError());
        return 1;
    }
    if (g_nt) {
        if (!MoveFileExA(stage, dst, MOVEFILE_DELAY_UNTIL_REBOOT | MOVEFILE_REPLACE_EXISTING)) {
            say("    %s: in use, and scheduling the replace failed (error %lu)", name, (unsigned long)GetLastError());
            DeleteFileA(stage);
            return 1;
        }
    } else {
        char ini[PATHBUF];
        snprintf(ini, sizeof ini, "%s\\WININIT.INI", g_win);
        if (!WritePrivateProfileStringA("rename", dst, stage, ini)) {
            say("    %s: in use, and scheduling the replace failed (error %lu)", name, (unsigned long)GetLastError());
            DeleteFileA(stage);
            return 1;
        }
    }
    g_reboot = 1;
    say("    %s: %s; the new copy replaces it on restart", name, why);
    return 0;
}

/* Copy one file and say so. A missing source is worth naming: it means
 * this ISO was built without that piece (no mingw DDK, say), not that the
 * user did anything wrong. */
static int copy_one(const char *src, const char *dstdir, const char *name)
{
    char dst[PATHBUF];
    DWORD err;

    snprintf(dst, sizeof dst, "%s\\%s", dstdir, name);
    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
        say("    %s: not on this disc", name);
        return 1;
    }
    /* a read-only copy from a previous install would refuse to be replaced */
    SetFileAttributesA(dst, FILE_ATTRIBUTE_NORMAL);
    if (CopyFileA(src, dst, FALSE)) {
        say("    %s -> %s", name, dstdir);
        return 0;
    }
    /* Locked because it is loaded (the running display driver, a started
     * .SYS): replace it on the next boot instead of failing. Only while
     * installing a component, not for a per-game copy, and only when the
     * target is really there to be replaced. */
    err = GetLastError();
    if (g_installing
        && (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_USER_MAPPED_FILE)
        && GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES)
        return replace_on_reboot(src, dstdir, name, "in use");
    say("    %s: copy failed, error %lu", name, (unsigned long)err);
    return 1;
}

/* Copy `names` (NULL-terminated) from an ISO folder into `dstdir`. */
static int copy_set(const char *isodir, const char *dstdir, const char *const *names)
{
    char src[PATHBUF];
    int bad = 0, i;

    for (i = 0; names[i]; i++) {
        snprintf(src, sizeof src, "%s%s\\%s", g_root, isodir, names[i]);
        bad |= copy_one(src, dstdir, names[i]);
    }
    return bad;
}

/* The same, for the files of a driver that may be the one Windows is running:
 * a name that already exists at the destination is never overwritten in
 * place, whether or not the copy would go through, but staged and swapped on
 * the restart the step asks for anyway. On 9x an overwrite that *succeeds*
 * is the dangerous one. A 16-bit .DRV's code segments are discardable and
 * KERNEL reloads a discarded one from the file on disk, and a ring-3 DLL's
 * pages are demand-paged from its file the same way, so a module whose file
 * has been replaced underneath it executes the new build's bytes at the old
 * build's addresses the next time a segment or page comes back in. That
 * is a fault inside the display driver, which on 9x is a blue screen
 * (`SETUP /ALL` over an installed driver died at the restart prompt). The
 * .DRV and the VxD happen to be held open and refuse the copy (doc 19 §28);
 * the DirectDraw HAL DLL does not while a DirectDraw application has it
 * loaded. Treating all of them alike makes a reinstall safe rather than
 * lucky. A name not there yet is a first install with nothing loaded,
 * and is copied outright. */
static int stage_set(const char *isodir, const char *dstdir, const char *const *names)
{
    char src[PATHBUF], dst[PATHBUF];
    int bad = 0, i;

    for (i = 0; names[i]; i++) {
        snprintf(src, sizeof src, "%s%s\\%s", g_root, isodir, names[i]);
        snprintf(dst, sizeof dst, "%s\\%s", dstdir, names[i]);
        if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) {
            say("    %s: not on this disc", names[i]);
            bad = 1;
        } else if (GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES) {
            bad |= replace_on_reboot(src, dstdir, names[i], "installed");
        } else {
            bad |= copy_one(src, dstdir, names[i]);
        }
    }
    return bad;
}

/* Every file in an ISO folder, for the test programs. */
static int copy_folder(const char *isodir, const char *dstdir)
{
    WIN32_FIND_DATAA fd;
    char pat[PATHBUF], src[PATHBUF + MAX_PATH];
    HANDLE h;
    int bad = 0;

    CreateDirectoryA(dstdir, NULL);
    snprintf(pat, sizeof pat, "%s%s\\*.*", g_root, isodir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        say("    %s: not on this disc", isodir);
        return 1;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        snprintf(src, sizeof src, "%s%s\\%s", g_root, isodir, fd.cFileName);
        bad |= copy_one(src, dstdir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return bad;
}

/* Run a program from the ISO and put its output in our log. Returns its
 * exit code, or -1 if it could not be started. Its stdout is read through
 * a pipe rather than left on our console so that the lines land in the
 * log file too, and in order. */
static int run_logged(const char *cmdline)
{
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE rd = NULL, wr = NULL;
    char buf[512], line[512], cmd[PATHBUF * 2];
    DWORD n, code = 0, i;
    int len = 0;

    memset(&sa, 0, sizeof sa);
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    lstrcpynA(cmd, cmdline, sizeof cmd);   /* CreateProcess may write to the command line */
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, g_root, &si, &pi)) {
        say("    cannot run %s (error %lu)", cmdline, (unsigned long)GetLastError());
        CloseHandle(rd); CloseHandle(wr);
        return -1;
    }
    CloseHandle(wr);        /* our end, or the read below never sees EOF */
    while (ReadFile(rd, buf, sizeof buf, &n, NULL) && n) {
        for (i = 0; i < n; i++) {
            if (buf[i] == '\n' || len == (int)sizeof line - 1) {
                line[len] = 0;
                if (len && line[len - 1] == '\r') line[len - 1] = 0;
                if (line[0]) say("    %s", line);
                len = 0;
            } else {
                line[len++] = buf[i];
            }
        }
    }
    if (len) { line[len] = 0; say("    %s", line); }
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

/* ------------------------------------------------------------ components */

/* A 3dfx card on this machine's PCI bus (the emulated Voodoo 2,
 * `-device voodoo2`, doc 21, or any other 3dfx board) as its hardware
 * ID ("PCI\VEN_121A&DEV_0002&..."), or empty. Its driver is 3dfx's own and
 * brings its own device mapper under the name ours has (FXMEMMAP.VXD), so
 * step_mapper must know, and the Voodoo 2 start-up guard is for it alone.
 *
 * Only a device that is *present* counts: both families keep the registry
 * entry of a card that has been taken out. 9x has the live
 * devnode tree in HKEY_DYN_DATA; NT has CM_Locate_DevNode, which finds
 * only present devnodes in its normal mode. cfgmgr32 is loaded at run
 * time, as nothing else here needs it. */
static char g_3dfx[256];

static int is_3dfx(const char *id)
{
    return !strnicmp(id, "PCI\\VEN_121A&", 13);
}

static void find_3dfx_9x(void)
{
    HKEY en, dn;
    char name[32], hw[256];
    DWORD i, len, type;

    if (RegOpenKeyExA(HKEY_DYN_DATA, "Config Manager\\Enum", 0, KEY_READ, &en) != ERROR_SUCCESS)
        return;
    for (i = 0; !g_3dfx[0] && RegEnumKeyA(en, i, name, sizeof name) == ERROR_SUCCESS; i++) {
        if (RegOpenKeyExA(en, name, 0, KEY_READ, &dn) != ERROR_SUCCESS)
            continue;
        len = sizeof hw;
        if (RegQueryValueExA(dn, "HardWareKey", NULL, &type, (BYTE *)hw, &len) == ERROR_SUCCESS
            && is_3dfx(hw))
            lstrcpynA(g_3dfx, hw, sizeof g_3dfx);
        RegCloseKey(dn);
    }
    RegCloseKey(en);
}

typedef DWORD (WINAPI *CmLocateDevNodeA)(DWORD *, char *, ULONG);

static void find_3dfx_nt(void)
{
    HMODULE cm = LoadLibraryA("cfgmgr32.dll");
    CmLocateDevNodeA locate = cm ? (CmLocateDevNodeA)GetProcAddress(cm, "CM_Locate_DevNodeA") : NULL;
    HKEY pci, dev;
    char d[200], inst[200], id[sizeof d + sizeof inst + 8];
    DWORD i, j, dn;

    if (locate && RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\PCI",
                                0, KEY_READ, &pci) == ERROR_SUCCESS) {
        for (i = 0; !g_3dfx[0] && RegEnumKeyA(pci, i, d, sizeof d) == ERROR_SUCCESS; i++) {
            if (strnicmp(d, "VEN_121A&", 9)
                || RegOpenKeyExA(pci, d, 0, KEY_READ, &dev) != ERROR_SUCCESS)
                continue;
            for (j = 0; RegEnumKeyA(dev, j, inst, sizeof inst) == ERROR_SUCCESS; j++) {
                snprintf(id, sizeof id, "PCI\\%s\\%s", d, inst);
                if (locate(&dn, id, 0 /* CM_LOCATE_DEVNODE_NORMAL */) == 0 /* CR_SUCCESS */) {
                    lstrcpynA(g_3dfx, id, sizeof g_3dfx);
                    break;
                }
            }
            RegCloseKey(dev);
        }
        RegCloseKey(pci);
    }
    if (cm) FreeLibrary(cm);
}

/* The device mapper: OPENGL32.DLL and our D3D DLLs reach the pass-through
 * device through it, and without it they refuse to load (0xc0000142 on
 * NT). 9x has it as a VxD that only needs to be in SYSTEM; NT as a kernel
 * driver a service must point at.
 *
 * The 9x mapper is 3dfx's own binary (FXMEMMAP.VXD 4.10.01.0013, the Glide
 * 2.42 one, same IOCTLs), so on a machine with a 3dfx card (the emulated
 * Voodoo 2, doc 21) the copy 3dfx's driver put there serves our DLLs as
 * well and is left alone rather than downgraded; NT's FXPTL.SYS is
 * qemu-3dfx's own name and installed as always. Glide itself is not ours
 * to install: a Glide game runs on the Voodoo 2 with 3dfx's driver
 * (ADR-020). */
static int step_mapper(void)
{
    static const char *const vxd[] = { "FXMEMMAP.VXD", NULL };
    static const char *const sys[] = { "FXPTL.SYS", NULL };
    char drivers[PATHBUF], cmd[PATHBUF * 2], path[PATHBUF];
    SC_HANDLE scm, svc;
    SERVICE_STATUS st;
    int bad = 0, running;

    say("The device mapper:");
    if (g_3dfx[0])
        say("    a 3dfx card is on this machine (%s)", g_3dfx);
    if (!g_nt) {
        snprintf(path, sizeof path, "%s\\FXMEMMAP.VXD", g_sys);
        if (g_3dfx[0] && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
            say("    FXMEMMAP.VXD: already there, left alone (3dfx's driver brings its own)");
        else
            bad = copy_set("MAPPER", g_sys, vxd);
        return bad;
    }

    snprintf(drivers, sizeof drivers, "%s\\drivers", g_sys);
    bad |= copy_set("MAPPER", drivers, sys);
    if (bad) return bad;
    /* INSTDRV registers the MAPMEM service on FXPTL.SYS and starts it. Its
     * exit code also covers a 3dfx-specific probe that has nothing to do
     * with us, so the service itself is what we check afterwards. */
    snprintf(cmd, sizeof cmd, "\"%s\"", iso(path, "MAPPER\\INSTDRV.EXE"));
    run_logged(cmd);
    scm = OpenSCManagerA(NULL, NULL, SC_MANAGER_CONNECT);
    svc = scm ? OpenServiceA(scm, "MAPMEM", SERVICE_QUERY_STATUS) : NULL;
    running = svc && QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING;
    if (svc) CloseServiceHandle(svc);
    if (scm) CloseServiceHandle(scm);
    say("    MAPMEM service: %s", running ? "running" : "NOT running (are you an Administrator?)");
    return running ? 0 : 1;
}

/* The display adapter driver, 2000/XP only. DRVINST.EXE is the tested
 * installer (signing policy, the Logo dialog, UpdateDriverForPlugAndPlay)
 * and says 2 when the machine has to restart before the driver can take
 * the adapter over from the boot VGA one. */
static int step_driver_nt(void)
{
    char cmd[PATHBUF * 2 + 8], inf[PATHBUF], exe[PATHBUF];
    int rc;

    iso(exe, "DRIVER\\DRVINST.EXE");
    iso(inf, "DRIVER\\D3DPTVID.INF");
    if (GetFileAttributesA(exe) == INVALID_FILE_ATTRIBUTES) {
        say("    not on this disc");
        return 1;
    }
    snprintf(cmd, sizeof cmd, "\"%s\" \"%s\"", exe, inf);
    rc = run_logged(cmd);
    if (rc == 2) { g_reboot = 1; return 0; }
    if (rc != 0) {
        say("    failed. The machine must run with -vga none -device d3dpt-vga.");
        return 1;
    }
    return 0;
}

/* The 9x half installs itself, which is the whole point of an INF: dropped
 * into WINDOWS\INF it matches PCI\VEN_1234&DEV_3D00, and the next boot
 * installs the driver with no clicks and no installer of ours (doc 19 §16).
 * There is nothing to run here, so this step is four file copies: the INF,
 * the display driver, the mini-VDD and the DirectDraw HAL DLL (every file
 * the INF's CopyFiles names; a missing one is a PnP prompt for it).
 *
 * The binaries go beside the INF because that is where Windows looks for a
 * CopyFiles source, and into SYSTEM as well because that is the arrangement
 * the driver has been proven in. The INF's own CopyFiles should make the
 * second set redundant; it costs a few kilobytes not to find out the hard
 * way on somebody's machine.
 *
 * The restart is required. Nothing of this driver exists to Windows until
 * the boot that enumerates the adapter against the new INF, and on a
 * reinstall every file that is already
 * there is swapped by that boot rather than overwritten under the driver
 * that is drawing the desktop (stage_set). */
static int step_driver_9x(void)
{
    static const char *const all[] = { "D3DPT9X.INF", "D3DPT9X.DRV", "D3DPT9V.VXD",
                                       "D3DPT9HL.DLL", NULL };
    static const char *const bin[] = { "D3DPT9X.DRV", "D3DPT9V.VXD", "D3DPT9HL.DLL", NULL };
    char infdir[PATHBUF], probe[PATHBUF];
    int bad;

    iso(probe, "DRIVER9X\\D3DPT9X.INF");
    if (GetFileAttributesA(probe) == INVALID_FILE_ATTRIBUTES) {
        say("    not on this disc");
        return 1;
    }
    snprintf(infdir, sizeof infdir, "%s\\INF", g_win);
    CreateDirectoryA(infdir, NULL);

    bad  = stage_set("DRIVER9X", infdir, all);
    bad |= stage_set("DRIVER9X", g_sys, bin);
    if (bad) {
        say("    failed. The machine must run with -vga none -device d3dpt-vga.");
        return 1;
    }
    g_reboot = 1;
    return 0;
}

/* Both families want this adapter driven properly; only the way in differs.
 * On NT a user-mode installer hands the INF to the setup API, on 9x the INF
 * is left where PnP will find it. */
static int step_driver(void)
{
    say("Display adapter driver (d3dpt-vga):");
    return g_nt ? step_driver_nt() : step_driver_9x();
}

/* CDSHELF.EXE into the Windows directory: it is a thing you want to reach
 * by name from a Run box or a batch file mid-game, and %windir% is on the
 * search path of both families. */
static int step_cdshelf(void)
{
    static const char *const exe[] = { "CDSHELF.EXE", NULL };

    say("Disc shelf tool:");
    return copy_set("CDSHELF", g_win, exe);
}

static int step_tests(void)
{
    say("Test programs:");
    return copy_folder("TESTS", BOXDIR);
}

/* The Sound Blaster 16's wave device names, where a translation made one
 * too long for DirectX 9 (doc 20 §5.3).
 *
 * A driver's caps name is a fixed 32 bytes and nothing makes it end in a
 * NUL. Windows 98's SB16.VXD builds it as "%s [%x]", its own string and
 * the card's port, and the Portuguese one's wave-in string is 27
 * characters, so "Entrada de som wave da SB16 [220]" is 33 and arrives
 * cut to 32 with no terminator. DirectX 9.0c's DSOUND.DLL copies the wave
 * names into a 32-byte stack buffer under a /GS cookie, so whatever
 * enumerates DirectSound (dxdiag, every game) dies with c0000409 inside
 * DSOUND.DLL. Nothing on the host can change that string; the fault is
 * the same on every host, and on a real Portuguese Win98 with a real SB16.
 *
 * The VxD has a door for it: at start it reads WaveInDevName /
 * WaveOutDevName from HKLM\SOFTWARE\Creative Tech\DeviceInfo\<enumerator>
 * \<hardware ID> and uses them instead of its own strings. The key is
 * named the way SB16.VXD names it: the device ID's first component, then
 * the devnode's HardwareID without its '*' and cut at the first ','. For
 * the card QEMU's sb16 is detected as it is DeviceInfo\ROOT\PNPB003.
 * A name is written only for a device whose name did not end within its
 * 32 bytes, and only on the Creative driver that reads it; the VxD reads
 * it at boot, hence the restart. */
#define SB16_NAME_MAX 25   /* 31 characters less " [220]", the VxD's suffix */
#ifndef MM_CREATIVE
#define MM_CREATIVE 2      /* the caps' wMid for Creative Labs (mmreg.h) */
#endif

/* `name` (32 bytes, maybe unterminated) as a name that fits: the text
 * before the VxD's " [port]" suffix, less a word "wave" (which is what the
 * translations spend their length on: "Entrada de som wave da SB16" is
 * "Entrada de som da SB16"), else cut at the last space that fits. */
static void sb16_short_name(const char *name, char *out)
{
    char buf[40], *p;
    size_t n;

    memcpy(buf, name, 32);
    buf[32] = 0;
    if ((p = strstr(buf, " [")) != NULL)
        *p = 0;
    if (strlen(buf) > SB16_NAME_MAX) {
        for (p = buf; (p = strchr(p, ' ')) != NULL; p++) {
            if (!strnicmp(p + 1, "wave", 4) && (p[5] == ' ' || !p[5])) {
                memmove(p, p + 5, strlen(p + 5) + 1);
                break;
            }
        }
    }
    if (strlen(buf) > SB16_NAME_MAX) {
        buf[SB16_NAME_MAX] = 0;
        if ((p = strrchr(buf, ' ')) != NULL && p > buf)
            *p = 0;
    }
    for (n = strlen(buf); n && buf[n - 1] == ' '; n--)
        buf[n - 1] = 0;
    lstrcpynA(out, buf, SB16_NAME_MAX + 1);
}

/* The DeviceInfo key of every devnode whose driver is SB16.VXD, into
 * keys[] (at most `max`); the count. */
static int sb16_keys(char keys[][MAX_PATH], int max)
{
    HKEY en, dev, inst, cls;
    char e[64], d[128], i[64], path[PATHBUF], drv[MAX_PATH], hw[256], *id, *c;
    DWORD ie, id_, ii, len, type;
    int n = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "Enum", 0, KEY_READ, &en) != ERROR_SUCCESS)
        return 0;
    for (ie = 0; n < max && RegEnumKeyA(en, ie, e, sizeof e) == ERROR_SUCCESS; ie++) {
        for (id_ = 0; n < max; id_++) {
            snprintf(path, sizeof path, "Enum\\%s", e);
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &dev) != ERROR_SUCCESS)
                break;
            len = RegEnumKeyA(dev, id_, d, sizeof d);
            RegCloseKey(dev);
            if (len != ERROR_SUCCESS)
                break;
            for (ii = 0; n < max; ii++) {
                snprintf(path, sizeof path, "Enum\\%s\\%s", e, d);
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &dev) != ERROR_SUCCESS)
                    break;
                len = RegEnumKeyA(dev, ii, i, sizeof i);
                RegCloseKey(dev);
                if (len != ERROR_SUCCESS)
                    break;
                snprintf(path, sizeof path, "Enum\\%s\\%s\\%s", e, d, i);
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &inst) != ERROR_SUCCESS)
                    continue;
                len = sizeof drv;
                drv[0] = hw[0] = 0;
                if (RegQueryValueExA(inst, "Driver", NULL, &type, (BYTE *)drv, &len) == ERROR_SUCCESS) {
                    len = sizeof hw;
                    RegQueryValueExA(inst, "HardwareID", NULL, &type, (BYTE *)hw, &len);
                }
                RegCloseKey(inst);
                if (!drv[0] || !hw[0])
                    continue;
                snprintf(path, sizeof path,
                         "System\\CurrentControlSet\\Services\\Class\\%s", drv);
                if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &cls) != ERROR_SUCCESS)
                    continue;
                len = sizeof drv;
                drv[0] = 0;
                RegQueryValueExA(cls, "Driver", NULL, &type, (BYTE *)drv, &len);
                RegCloseKey(cls);
                if (stricmp(drv, "sb16.vxd"))
                    continue;
                id = (c = strchr(hw, '*')) != NULL ? c + 1 : hw;
                if ((c = strchr(id, ',')) != NULL)
                    *c = 0;
                snprintf(keys[n++], MAX_PATH,
                         "SOFTWARE\\Creative Tech\\DeviceInfo\\%s\\%s", e, id);
            }
        }
    }
    RegCloseKey(en);
    return n;
}

static int step_sb16_names(void)
{
    char in_name[33] = "", out_name[33] = "", keys[4][MAX_PATH], v[SB16_NAME_MAX + 1];
    int k, nkeys, bad = 0;
    UINT i;

    say("Sound Blaster 16 device names:");
    for (i = 0; i < waveInGetNumDevs(); i++) {
        WAVEINCAPSA c;
        memset(&c, 0xcc, sizeof c);
        if (waveInGetDevCapsA(i, &c, sizeof c) == MMSYSERR_NOERROR && c.wMid == MM_CREATIVE
            && !memchr(c.szPname, 0, sizeof c.szPname))
            memcpy(in_name, c.szPname, 32);
    }
    for (i = 0; i < waveOutGetNumDevs(); i++) {
        WAVEOUTCAPSA c;
        memset(&c, 0xcc, sizeof c);
        if (waveOutGetDevCapsA(i, &c, sizeof c) == MMSYSERR_NOERROR && c.wMid == MM_CREATIVE
            && !memchr(c.szPname, 0, sizeof c.szPname))
            memcpy(out_name, c.szPname, 32);
    }
    if (!in_name[0] && !out_name[0]) {
        say("    every name fits; nothing to do");
        return 0;
    }
    nkeys = sb16_keys(keys, 4);
    if (!nkeys) {
        say("    a name does not fit (\"%s%s%s\"), but no device here uses SB16.VXD,",
            in_name, in_name[0] && out_name[0] ? "\", \"" : "", out_name);
        say("    which is the driver this knows how to rename");
        return 1;
    }
    for (k = 0; k < nkeys; k++) {
        HKEY h;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, keys[k], 0, NULL, 0, KEY_WRITE, NULL,
                            &h, NULL) != ERROR_SUCCESS) {
            say("    HKLM\\%s: cannot create it (error %lu)", keys[k], (unsigned long)GetLastError());
            bad = 1;
            continue;
        }
        if (in_name[0]) {
            sb16_short_name(in_name, v);
            bad |= RegSetValueExA(h, "WaveInDevName", 0, REG_SZ, (BYTE *)v, strlen(v) + 1)
                   != ERROR_SUCCESS;
            say("    wave in: \"%s\" does not fit; HKLM\\%s WaveInDevName = \"%s\"",
                in_name, keys[k], v);
        }
        if (out_name[0]) {
            sb16_short_name(out_name, v);
            bad |= RegSetValueExA(h, "WaveOutDevName", 0, REG_SZ, (BYTE *)v, strlen(v) + 1)
                   != ERROR_SUCCESS;
            say("    wave out: \"%s\" does not fit; HKLM\\%s WaveOutDevName = \"%s\"",
                out_name, keys[k], v);
        }
        RegCloseKey(h);
    }
    if (bad) {
        say("    failed writing the registry");
        return 1;
    }
    g_reboot = 1;
    return 0;
}

/* The Voodoo 2's start-up guard (doc 21 §11): V2START.EXE in the Windows
 * folder and in the Run key, in place of 3dfx's own `Voodoo2` entry.
 *
 * 3dfx's driver runs `rundll32 3dfxv2ps.dll,UpdateRegSettings` at every
 * login, which initialises the card from another process for about three
 * seconds under TCG, and a Glide game started inside them hangs. The
 * command moves to HKLM\SOFTWARE\2ksbox\Voodoo2 and V2START runs it at
 * login instead, with a notice on the desktop until it has finished (the
 * program's own header has the rest).
 *
 * Only on a machine with a 3dfx card. A card whose driver is not installed
 * yet has no entry to move: the guard goes in anyway, and moves the entry
 * itself at the first login after 3dfx's driver writes it. */
#define RUN_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define V2_KEY "SOFTWARE\\2ksbox\\Voodoo2"
#define V2_RUN_NAME "2ksbox Voodoo 2"

static int step_voodoo2_guard(void)
{
    static const char *const exe[] = { "V2START.EXE", NULL };
    char cmd[PATHBUF], ours[PATHBUF];
    DWORD len = sizeof cmd, type;
    HKEY run, v2;
    int moved = 0, bad;

    say("Voodoo 2 start-up guard:");
    if (!g_3dfx[0]) {
        say("    no 3dfx card on this machine; nothing to do");
        return 0;
    }
    bad = copy_set("VOODOO2", g_win, exe);
    if (bad)
        return 1;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, RUN_KEY, 0, NULL, 0, KEY_READ | KEY_WRITE, NULL,
                        &run, NULL) != ERROR_SUCCESS) {
        say("    HKLM\\%s: cannot open it (error %lu)", RUN_KEY, (unsigned long)GetLastError());
        return 1;
    }
    if (RegQueryValueExA(run, "Voodoo2", NULL, &type, (BYTE *)cmd, &len) == ERROR_SUCCESS
        && type == REG_SZ && len > 1) {
        cmd[sizeof cmd - 1] = 0;
        if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, V2_KEY, 0, NULL, 0, KEY_WRITE, NULL, &v2, NULL)
                != ERROR_SUCCESS
            || RegSetValueExA(v2, "Command", 0, REG_SZ, (BYTE *)cmd, strlen(cmd) + 1) != ERROR_SUCCESS) {
            say("    HKLM\\%s: cannot write the command (error %lu)", V2_KEY, (unsigned long)GetLastError());
            RegCloseKey(run);
            return 1;
        }
        RegCloseKey(v2);
        RegDeleteValueA(run, "Voodoo2");
        say("    3dfx's start-up entry \"%s\" moved to HKLM\\%s", cmd, V2_KEY);
        moved = 1;
    }
    snprintf(ours, sizeof ours, "%s\\V2START.EXE", g_win);
    if (RegSetValueExA(run, V2_RUN_NAME, 0, REG_SZ, (BYTE *)ours, strlen(ours) + 1) != ERROR_SUCCESS) {
        say("    HKLM\\%s: cannot add \"%s\" (error %lu)", RUN_KEY, V2_RUN_NAME, (unsigned long)GetLastError());
        RegCloseKey(run);
        return 1;
    }
    RegCloseKey(run);
    if (!moved) {
        len = sizeof cmd;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, V2_KEY, 0, KEY_READ, &v2) == ERROR_SUCCESS) {
            if (RegQueryValueExA(v2, "Command", NULL, &type, (BYTE *)cmd, &len) == ERROR_SUCCESS)
                moved = 2;
            RegCloseKey(v2);
        }
        say(moved ? "    3dfx's start-up entry was moved by an earlier install"
                  : "    3dfx's driver has no start-up entry yet; the guard takes it at the first login after it does");
    }
    say("    \"%s\" = %s: from the next start, a notice says when the card is ready", V2_RUN_NAME, ours);
    return 0;
}

/* MS-DOS mode with the CD-ROM drive and the Sound Blaster (Win98/Me).
 *
 * A DOS title that dies in a DOS box (Mortal Kombat 3 does, on any host)
 * plays from Windows' MS-DOS mode, but that DOS has no CD-ROM drive and
 * no BLASTER variable unless something says so. Windows' own way of
 * saying so is a program information file that carries its own
 * CONFIG.SYS and AUTOEXEC.BAT: starting it restarts the machine into
 * real-mode DOS with those two, and EXIT boots Windows again. Windows 98
 * ships two samples in its folder ("MS-DOS mode for games", KB 188157)
 * built just like this; the bytes below are that structure with the
 * text taken out. The step writes one on the desktop. Nothing goes into
 * the machine's CONFIG.SYS: a real-mode CD driver loaded under Windows
 * makes IOS route every drive through real mode (IOS.LOG: "UnKnown int
 * 13 hooker UIDE"), which is the compatibility mode nobody wants.
 *
 * The PIF's CONFIG.SYS is the machine's own lines (its codepage and
 * country) plus HIMEM and UIDE.SYS (FreeDOS's driver, public domain, in
 * C:\2KSBOX) as a CD-only driver with no cache (/N1: no hard disk; /N3:
 * no XMS, so it loads whatever comes before it). Its AUTOEXEC.BAT is the
 * machine's own lines (its keyboard layout) plus PATH, MSCDEX and
 * BLASTER: a game's own setup (Miles' SETSOUND) offers the Sound Blaster
 * drivers only with it set. QEMU's SB16 is 220h, IRQ 5, DMA 1 and 5, the
 * MPU-401 is at 330h.
 *
 * Layout: 0x171 bytes of header (title at 2, 30 bytes space-padded;
 * program at 0x24; start directory at 0x65; byte 1 is 0x78 in every file
 * Windows writes), then sections of a 16-byte name, the next section's
 * offset (0xffff for the last), the data's offset and length. The
 * "WINDOWS 386 3.0" section's word at its +0x12 is 0x0082: MS-DOS mode
 * with the file's own configuration (0x1082 is "use the current one",
 * which is what Shut Down's "Restart in MS-DOS mode" makes). */
static const unsigned char pif_head[0x3c7] = {
    0x00, 0x78, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x80, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0xff, 0x19, 0x50, 0x00, 0x00, 0x07, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x4d, 0x49, 0x43, 0x52, 0x4f, 0x53, 0x4f, 0x46, 0x54, 0x20, 0x50, 0x49, 0x46, 0x45, 0x58,
    0x00, 0x87, 0x01, 0x00, 0x00, 0x71, 0x01, 0x57, 0x49, 0x4e, 0x44, 0x4f, 0x57, 0x53, 0x20, 0x33,
    0x38, 0x36, 0x20, 0x33, 0x2e, 0x30, 0x00, 0x05, 0x02, 0x9d, 0x01, 0x68, 0x00, 0x80, 0x02, 0x00,
    0x00, 0x64, 0x00, 0x32, 0x00, 0xff, 0xff, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00, 0x02, 0x10, 0x82,
    0x00, 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x57, 0x49, 0x4e, 0x44, 0x4f, 0x57, 0x53, 0x20, 0x56, 0x4d, 0x4d,
    0x20, 0x34, 0x2e, 0x30, 0x00, 0xc7, 0x03, 0x1b, 0x02, 0xac, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x50, 0x49, 0x46, 0x4d, 0x47, 0x52, 0x2e, 0x44, 0x4c, 0x4c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x32, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01,
    0x00, 0x00, 0x00, 0x05, 0x00, 0x19, 0x00, 0x03, 0x00, 0xc8, 0x00, 0xe8, 0x03, 0x02, 0x00, 0x0a,
    0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x54, 0x65, 0x72, 0x6d, 0x69, 0x6e, 0x61, 0x6c, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x43, 0x6f, 0x75, 0x72, 0x69, 0x65, 0x72, 0x20, 0x4e, 0x65, 0x77,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
};

#define PIF_NAME "2ksbox MS-DOS mode.pif"
#define PIF_TITLE "2ksbox MS-DOS mode"

/* The machine's own file's lines, CRLF-terminated, or nothing. */
static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(n + 3);
    if (!buf || fread(buf, 1, n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    if (n && buf[n - 1] != '\n') { buf[n++] = '\r'; buf[n++] = '\n'; }
    buf[n] = 0;
    return buf;
}

static void pif_section(FILE *f, const char *name, unsigned next, unsigned data, const char *text)
{
    char hdr[16] = { 0 };
    unsigned short w;

    memcpy(hdr, name, strlen(name));
    fwrite(hdr, 1, 16, f);
    w = next; fwrite(&w, 2, 1, f);
    w = data; fwrite(&w, 2, 1, f);
    w = strlen(text); fwrite(&w, 2, 1, f);
    fwrite(text, 1, strlen(text), f);
}

static int step_dos_mode(void)
{
    static const char *const files[] = { "UIDE.SYS", NULL };
    char path[PATHBUF], sys[PATHBUF], bat[PATHBUF], desk[MAX_PATH];
    char *own_sys, *own_bat, *cfg, *aut;
    unsigned char head[sizeof pif_head];
    size_t ncfg, naut;
    unsigned off;
    FILE *f;

    say("MS-DOS mode with the CD-ROM drive and the Sound Blaster:");
    CreateDirectoryA(BOXDIR, NULL);
    if (copy_set("DOSMODE", BOXDIR, files))
        return 1;
    snprintf(sys, sizeof sys, "%c:\\CONFIG.SYS", g_win[0]);
    snprintf(bat, sizeof bat, "%c:\\AUTOEXEC.BAT", g_win[0]);
    own_sys = read_text(sys);
    own_bat = read_text(bat);
    ncfg = (own_sys ? strlen(own_sys) : 0) + 4 * PATHBUF;
    naut = (own_bat ? strlen(own_bat) : 0) + 8 * PATHBUF;
    cfg = malloc(ncfg);
    aut = malloc(naut);
    if (!cfg || !aut) {
        say("    out of memory");
        return 1;
    }
    snprintf(cfg, ncfg,
             "%s"
             "DOS=HIGH\r\n"
             "%s%s%s"
             "DEVICE=" BOXDIR "\\UIDE.SYS /N1 /N3 /D:MSCD001\r\n",
             own_sys ? own_sys : "",
             own_sys && strstr(own_sys, "HIMEM") ? "" : "DEVICE=",
             own_sys && strstr(own_sys, "HIMEM") ? "" : g_win,
             own_sys && strstr(own_sys, "HIMEM") ? "" : "\\HIMEM.SYS\r\n");
    snprintf(aut, naut,
             "%s"
             "SET PATH=%s;%s\\COMMAND\r\n"
             "SET PROMPT=MS-DOS mode $p$g\r\n"
             "SET winbootdir=%s\r\n"
             "SET BLASTER=A220 I5 D1 H5 P330 T6\r\n"
             "%s\\COMMAND\\MSCDEX.EXE /D:MSCD001\r\n"
             "ECHO 2ksbox MS-DOS mode: the CD-ROM is the drive after the hard disk. EXIT returns to Windows.\r\n",
             own_bat ? own_bat : "", g_win, g_win, g_win, g_win);
    free(own_sys);
    free(own_bat);

    if (!SHGetSpecialFolderPathA(NULL, desk, CSIDL_DESKTOPDIRECTORY, FALSE))
        lstrcpynA(desk, g_win, sizeof desk);
    snprintf(path, sizeof path, "%s\\" PIF_NAME, desk);
    memcpy(head, pif_head, sizeof head);
    memset(head + 2, ' ', 30);
    memcpy(head + 2, PIF_TITLE, strlen(PIF_TITLE));
    snprintf((char *)head + 0x24, 63, "%s\\COMMAND.COM", g_win);
    snprintf((char *)head + 0x65, 64, "%c:\\", g_win[0]);
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    f = fopen(path, "wb");
    if (!f) {
        say("    %s: cannot write it (error %lu)", path, (unsigned long)GetLastError());
        free(cfg);
        free(aut);
        return 1;
    }
    fwrite(head, 1, sizeof head, f);
    off = sizeof head;
    pif_section(f, "CONFIG  SYS 4.0", off + 22 + strlen(cfg), off + 22, cfg);
    off += 22 + strlen(cfg);
    pif_section(f, "AUTOEXECBAT 4.0", 0xffff, off + 22, aut);
    fclose(f);
    free(cfg);
    free(aut);
    say("    %s: written (its own CONFIG.SYS and AUTOEXEC.BAT: this machine's lines,", path);
    say("    then HIMEM, UIDE.SYS as a CD-only driver, MSCDEX and BLASTER)");
    say("    Open it to restart in MS-DOS mode: the CD-ROM is the drive after the hard");
    say("    disk, BLASTER is set, and EXIT boots Windows again. A game's sound setup");
    say("    wants Sound Blaster 16, 220h, IRQ 5, DMA 1. Nothing here changes how");
    say("    Windows itself starts.");
    return 0;
}

typedef struct {
    const char *label;
    const char *note;
    int on_9x, on_nt, def;
    int (*run)(void);
    int on;
} Component;

static Component g_comp[MAX_COMPONENTS] = {
    { "Display adapter driver (d3dpt-vga)", "needs a restart",              1, 1, 1, step_driver,  0 },
    { "The device mapper",                  "needed by OPENGL32.DLL",       1, 1, 1, step_mapper,  0 },
    { "Disc shelf tool",                    "CDSHELF.EXE in the Windows folder", 1, 1, 1, step_cdshelf, 0 },
    { "Test programs",                      "in C:\\2KSBOX",                1, 1, 0, step_tests,   0 },
    /* last, so no earlier component's /I number moves */
    { "Sound Blaster 16 device names",      "DirectX 9 fix, only if needed", 1, 0, 1, step_sb16_names, 0 },
    /* after it, for the same reason */
    { "Voodoo 2 start-up guard",            "only with a 3dfx card",         1, 0, 1, step_voodoo2_guard, 0 },
    /* and after that */
    { "MS-DOS mode: CD-ROM drive + Sound Blaster", "a PIF on the desktop; Win98/Me", 1, 0, 0, step_dos_mode, 0 },
};
static int g_ncomp;

/* ------------------------------------------------- the per-game file sets */

/* Copied next to one game's EXE, never into the system directory. Each
 * set is self-contained (what a game needs to run on that stack and
 * nothing else), so two stacks can never end up in one folder. The files
 * are pairs, <name on the ISO> then <name it must have next to the EXE>.
 * No set renames anything: the folders carry the DLLs under the names a
 * game loads, so copying a folder from Explorer and running SETUP /GAME
 * give the same result. A retired set keeps its place (no dir) so the
 * numbers after it do not move. */
typedef struct {
    const char *label;
    const char *dir;
    const char *files[10];
} GameSet;

static const GameSet g_sets[] = {
    { "Direct3D 8/9 DLLs: retired. Games use Windows' own Direct3D on the display driver.",
      NULL,      { NULL } },
    { "DirectInput keyboard fix (DINPUT.DLL)",
      "DINPUT",  { "DINPUT.DLL", "DINPUT.DLL", NULL } },
    { "OpenGL pass-through (OPENGL32.DLL WRAPGL32.EXT)",
      "OPENGL",  { "OPENGL32.DLL", "OPENGL32.DLL",
                   "WRAPGL32.EXT", "WRAPGL32.EXT", NULL } },
};
#define NSETS ((int)(sizeof g_sets / sizeof g_sets[0]))

/* A file of a set that is settings rather than a binary: the user edits it
 * next to the game (the OpenGL wrapper's own, which holds the extension-year
 * cap a title needs), so a second SETUP /GAME must not undo that. The
 * binaries beside it are still replaced, which is the point of running the
 * verb again after a rebuild. */
static int is_settings_file(const char *name)
{
    return !stricmp(name, "WRAPGL32.EXT");
}

static int copy_game_set(int n, const char *dir)
{
    const GameSet *s;
    char src[PATHBUF], dst[PATHBUF];
    int bad = 0, i;

    if (n < 1 || n > NSETS) { say("no such set: %d", n); return 1; }
    s = &g_sets[n - 1];
    if (!s->dir) { say("set %d: %s", n, s->label); return 1; }
    if (GetFileAttributesA(dir) == INVALID_FILE_ATTRIBUTES) {
        say("%s: no such folder", dir);
        return 1;
    }
    say("%s", s->label);
    say("  into %s", dir);
    for (i = 0; s->files[i]; i += 2) {
        snprintf(src, sizeof src, "%s%s\\%s", g_root, s->dir, s->files[i]);
        snprintf(dst, sizeof dst, "%s\\%s", dir, s->files[i + 1]);
        if (is_settings_file(s->files[i + 1])
            && GetFileAttributesA(dst) != INVALID_FILE_ATTRIBUTES) {
            say("    %s: already there, left alone (it is yours to edit)", s->files[i + 1]);
            continue;
        }
        bad |= copy_one(src, dir, s->files[i + 1]);
    }
    return bad;
}

/* --------------------------------------------------------------- install */

static int install_selected(void)
{
    int i, bad = 0, any = 0;

    g_installing = 1;   /* a locked system file may now be swapped on reboot rather than failing */
    for (i = 0; i < g_ncomp; i++) {
        if (!g_comp[i].on) continue;
        any = 1;
        bad |= g_comp[i].run();
    }
    g_installing = 0;
    if (!any) { say("nothing selected"); return 0; }
    say("");
    if (bad) say("Finished with errors. See the lines above.");
    else if (g_reboot) say("Installed. Restart Windows to finish.");
    else say("Installed.");
    return bad;
}

/* The restart itself, in whichever process can actually perform it.
 *
 * On NT this is the whole of it: take SE_SHUTDOWN_NAME and call.
 *
 * On 9x the call must not be made by *this* process. Measured on Windows 98
 * 4.10.2222: ExitWindowsEx from a console process never returns and no
 * shutdown begins at all. There is no dialog, the desktop is untouched, and
 * the machine is still up five minutes later. The thread stuck inside it
 * holds the Win16Mutex, so pumping messages does not rescue it either: a
 * second thread pumping while a first one calls goes down with it, and the
 * whole process is then deaf to USER. The console costs the process its
 * own message queue (a console app's window belongs to the DOS box hosting
 * it, not to us). The same call from a process started with
 * DETACHED_PROCESS, which has no console at all, restarts the machine
 * cleanly. That process is this one again, run as `SETUP /REBOOTNOW`, so
 * the disc carries no second binary for it.
 *
 * Variants that do not work:
 * `rundll32 shell32.dll,SHExitWindowsEx 2` does nothing; `rundll32
 * krnl386.exe,exitkernel` does bring Windows down, but as a forced exit that
 * leaves the FAT dirty and the next boot in ScanDisk. */
static void reboot_now(void)
{
    HANDLE tok;
    TOKEN_PRIVILEGES tp;

    say("restarting Windows");

    if (!g_nt) {
        char self[MAX_PATH], cmd[MAX_PATH + 16];
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;

        GetModuleFileNameA(NULL, self, sizeof self);
        snprintf(cmd, sizeof cmd, "\"%s\" /REBOOTNOW", self);
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, DETACHED_PROCESS,
                           NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return;
        }
        say("    could not restart (error %lu). Restart Windows yourself.",
            GetLastError());
        return;
    }

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        LookupPrivilegeValueA(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL);
        CloseHandle(tok);
    }
    if (!ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0))
        say("    Windows refused to restart (error %lu). Restart it yourself.",
            GetLastError());
}

/* --------------------------------------------------------- the interface */

static void print_components(void)
{
    int i;

    say("Install into Windows:");
    for (i = 0; i < g_ncomp; i++)
        say("  %d [%c] %-34s %s", i + 1, g_comp[i].on ? 'x' : ' ', g_comp[i].label, g_comp[i].note);
}

static void print_sets(void)
{
    int i;

    say("Copy next to one game's EXE:");
    for (i = 0; i < NSETS; i++)
        if (g_sets[i].dir) say("  %d %s", i + 1, g_sets[i].label);
}

/* A line from the console, trimmed; NULL at end of input (a redirected
 * stdin that ran out, which is also how a batch file leaves us). */
static char *ask(const char *prompt, char *buf, int len)
{
    char *p;

    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, len, stdin)) return NULL;
    for (p = buf + strlen(buf); p > buf && (p[-1] == '\n' || p[-1] == '\r' || p[-1] == ' '); p--)
        p[-1] = 0;
    for (p = buf; *p == ' '; p++) ;
    return p;
}

static void menu(void)
{
    char buf[PATHBUF + 16], *in;

    for (;;) {
        say("");
        print_components();
        in = ask("\nNumber toggles, I installs, G per-game files, Q quits: ", buf, sizeof buf);
        if (!in) return;
        if (isdigit((unsigned char)*in)) {
            int n = atoi(in);
            if (n >= 1 && n <= g_ncomp) g_comp[n - 1].on = !g_comp[n - 1].on;
            else say("no such component: %s", in);
            continue;
        }
        switch (toupper((unsigned char)*in)) {
        case 'I':
            say("");
            install_selected();
            if (g_reboot) {
                in = ask("\nRestart Windows now (y/N)? ", buf, sizeof buf);
                if (in && toupper((unsigned char)*in) == 'Y') { reboot_now(); return; }
            }
            break;
        case 'G': {
            int n;
            char dir[PATHBUF];
            say("");
            print_sets();
            in = ask("\nWhich set (Enter cancels)? ", buf, sizeof buf);
            if (!in || !*in) break;
            n = atoi(in);
            in = ask("Folder holding the game's EXE: ", dir, sizeof dir);
            if (!in || !*in) break;
            say("");
            copy_game_set(n, in);
            break;
        }
        case 'Q':
        case 0:
            return;
        default:
            say("?");
        }
    }
}

static void usage(void)
{
    printf("SETUP - 2ksbox guest tools\n\n"
           "  SETUP                 the menu\n"
           "  SETUP /ALL            install every component this Windows can use\n"
           "  SETUP /I <n> [<n>...] install those components\n"
           "  SETUP /LIST           print the component and file-set lists\n"
           "  SETUP /GAME <n> <dir> copy file set <n> next to a game's EXE\n"
           "  SETUP /REBOOT         with /ALL or /I: restart if one asked for it\n"
           "  SETUP /LOG <file>     write the log there (default C:\\2KSBOX\\SETUP.LOG)\n");
}

/* "Windows 98 SE" / "Windows XP", shown so the user can confirm it: the
 * component list differs per family. */
static void os_name(char *out, OSVERSIONINFOA *v)
{
    const char *n = "Windows";

    if (v->dwPlatformId == VER_PLATFORM_WIN32_NT) {
        if (v->dwMajorVersion == 5 && v->dwMinorVersion == 0) n = "Windows 2000";
        else if (v->dwMajorVersion == 5) n = "Windows XP";
        else if (v->dwMajorVersion > 5) n = "Windows, newer than XP";
        else n = "Windows NT";
    } else if (v->dwMajorVersion == 4) {
        if (v->dwMinorVersion == 0) n = "Windows 95";
        else if (v->dwMinorVersion == 10) n = "Windows 98";
        else if (v->dwMinorVersion == 90) n = "Windows Me";
    }
    snprintf(out, 128, "%s (%lu.%lu build %lu)", n, (unsigned long)v->dwMajorVersion,
            (unsigned long)v->dwMinorVersion, (unsigned long)(v->dwBuildNumber & 0xffff));
}

/* Open `path` for the log and remember where it landed, as an absolute
 * path, so the program can tell the user at the end. A Windows 98 console
 * has no scrollback, so "here is the whole log" is the one line that has to
 * survive on screen. */
static int try_log(const char *path)
{
    g_log = fopen(path, "w");
    if (!g_log) return 0;
    if (!GetFullPathNameA(path, sizeof g_log_path, g_log_path, NULL))
        lstrcpynA(g_log_path, path, sizeof g_log_path);
    return 1;
}

/* Where the log goes: an explicit /LOG wins, otherwise C:\2KSBOX\SETUP.LOG
 * (BOXDIR above; a read-only CD's directory would lose it). TEMP is the
 * last resort if C:\2KSBOX cannot be made. Whichever wins, its
 * absolute path is announced; the log is never left somewhere to guess at. */
static void open_log(const char *want)
{
    char path[PATHBUF];

    if (want && try_log(want)) return;
    CreateDirectoryA(BOXDIR, NULL);
    if (try_log(BOXDIR "\\SETUP.LOG")) return;
    if (GetTempPathA(sizeof path - 16, path)) {
        lstrcatA(path, "SETUP.LOG");
        try_log(path);
    }
}

/* The last line on screen: where to read the whole log. Console only (the
 * log need not name itself), and printed at the end of every mode so it is
 * what stays visible after the console has scrolled past everything else. */
static void announce_log(void)
{
    if (g_log_path[0])
        printf("\nFull log of what was done: %s\n", g_log_path);
    else
        printf("\n(a log file could not be written anywhere)\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    OSVERSIONINFOA ver;
    char osname[128], *slash;
    int i, want_reboot = 0, mode_all = 0, mode_list = 0, sel = 0, rc = 0;
    int game = 0;
    const char *game_dir = NULL, *logfile = NULL;

    /* The detached second copy of ourselves that reboot_now() starts on 9x:
     * no console, no log, nothing but the call this program cannot make in
     * the process the user ran. Handled before anything else, so it neither
     * prints nor truncates SETUP.LOG. */
    if (argc == 2 && (argv[1][0] == '/' || argv[1][0] == '-')
        && !stricmp(argv[1] + 1, "rebootnow")) {
        ExitWindowsEx(EWX_REBOOT | EWX_FORCE, 0);
        return 0;
    }

    ver.dwOSVersionInfoSize = sizeof ver;
    GetVersionExA(&ver);
    g_nt = ver.dwPlatformId == VER_PLATFORM_WIN32_NT;
    os_name(osname, &ver);

    GetModuleFileNameA(NULL, g_root, sizeof g_root);
    slash = strrchr(g_root, '\\');
    if (slash) slash[1] = 0;
    GetSystemDirectoryA(g_sys, sizeof g_sys);
    GetWindowsDirectoryA(g_win, sizeof g_win);
    if (g_nt) find_3dfx_nt();
    else find_3dfx_9x();

    /* only the components this Windows can use, in the listed order */
    for (i = 0; i < MAX_COMPONENTS && g_comp[i].label; i++) {
        if (!(g_nt ? g_comp[i].on_nt : g_comp[i].on_9x)) continue;
        if (i != g_ncomp) g_comp[g_ncomp] = g_comp[i];
        g_comp[g_ncomp].on = g_comp[g_ncomp].def;
        g_ncomp++;
    }

    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '/' || a[0] == '-') a++;
        if (!stricmp(a, "all")) mode_all = 1;
        else if (!stricmp(a, "list")) mode_list = 1;
        else if (!stricmp(a, "reboot")) want_reboot = 1;
        else if (!stricmp(a, "log") && i + 1 < argc) logfile = argv[++i];
        else if (!stricmp(a, "i")) {
            for (i++; i < argc && isdigit((unsigned char)argv[i][0]); i++) {
                int n = atoi(argv[i]);
                if (n >= 1 && n <= g_ncomp) sel |= 1 << (n - 1);
            }
            i--;
            mode_all = 2;
        } else if (!stricmp(a, "game") && i + 2 < argc) {
            game = atoi(argv[++i]);
            game_dir = argv[++i];
        } else {
            usage();
            return 1;
        }
    }

    open_log(logfile);
    {
        SYSTEMTIME lt;
        GetLocalTime(&lt);
        say("2ksbox guest tools - %s", osname);
        say("%04d-%02d-%02d %02d:%02d:%02d  %s", lt.wYear, lt.wMonth, lt.wDay,
            lt.wHour, lt.wMinute, lt.wSecond, GetCommandLineA());
        say("files from %s", g_root);
        say("log: %s", g_log_path[0] ? g_log_path : "(none)");
    }

    if (mode_list) { say(""); print_components(); say(""); print_sets(); announce_log(); return 0; }
    if (game_dir) { rc = copy_game_set(game, game_dir); announce_log(); return rc; }

    /* /ALL means all of them, including the ones the menu leaves
     * unticked: a batch file that says /ALL is not choosing defaults. */
    if (mode_all == 1)
        for (i = 0; i < g_ncomp; i++) g_comp[i].on = 1;
    if (mode_all == 2)
        for (i = 0; i < g_ncomp; i++) g_comp[i].on = (sel >> i) & 1;
    if (mode_all) {
        say("");
        print_components();
        say("");
        rc = install_selected();
        announce_log();
        if (g_reboot && want_reboot) reboot_now();
        return rc;
    }
    menu();
    announce_log();
    return 0;
}
