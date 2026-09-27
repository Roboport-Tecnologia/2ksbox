/*
 * WTRUN.EXE: runs Wine's Direct3D conformance tests in the guest (track M16,
 * docs/tracks/m16-dx9-ddi.md), one test file per process.
 *
 *   WTRUN <seconds> <exe> <test> [<test>...]
 *   WTRUN 900 D3D9_TEST.EXE d3d9ex device stateblock visual
 *
 * Each test's stdout and stderr go to <exe stem>_<test>.txt in the output
 * folder (guestlog.h: C:\2KSBOX\WINETEST, or BOXLOG), and one line per test
 * goes to WTRUN.LOG there and to COM1: its exit code, or "timeout" when it
 * ran past <seconds> and was killed. A driver bug can hang a test, and a
 * hung test must not take the rest of the run with it. The last line on
 * COM1 is "WTDONE", the harness's marker. A STOP.TXT in the output folder
 * (winetest patch 09: a test found that no device can be made any more)
 * leaves every later file unrun, with the test it names.
 *
 * The lines Wine's runner prints ("visual: 1234 tests executed (5 marked as
 * todo, 0 as flaky, 6 failures), 7 skipped.") are read on the host by
 * tools/winetest-summary.py.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../guestlog.h"

static HANDLE com1 = INVALID_HANDLE_VALUE;
static FILE *logf;

static void say(const char *fmt, ...)
{
    char line[512];
    DWORD n;
    va_list ap;

    va_start(ap, fmt);
    _vsnprintf(line, sizeof line - 3, fmt, ap);
    va_end(ap);
    line[sizeof line - 3] = 0;
    strcat(line, "\r\n");
    fputs(line, stdout);
    if (logf) {
        fputs(line, logf);
        fflush(logf);
    }
    if (com1 != INVALID_HANDLE_VALUE)
        WriteFile(com1, line, (DWORD)strlen(line), &n, NULL);
}

int main(int argc, char **argv)
{
    char dir[GUEST_PATHBUF], stem[MAX_PATH], path[GUEST_PATHBUF], cmd[MAX_PATH * 2];
    const char *base, *dot;
    DWORD cap_ms;
    int i;

    if (argc < 4) {
        fprintf(stderr, "usage: WTRUN <seconds> <exe> <test> [<test>...]\n");
        return 2;
    }
    cap_ms = (DWORD)atoi(argv[1]) * 1000;

    guest_path(dir, sizeof dir, "WINETEST");
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof path, "%s\\WTRUN.LOG", dir);
    logf = fopen(path, "a");
    com1 = CreateFileA("COM1", GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);

    /* the stem names the output files: D3D9_TEST.EXE -> d3d9_test */
    base = strrchr(argv[2], '\\');
    base = base ? base + 1 : argv[2];
    snprintf(stem, sizeof stem, "%s", base);
    if ((dot = strrchr(stem, '.')))
        stem[dot - stem] = 0;
    CharLowerA(stem);

    /* no "the program has encountered a problem" box for a crashing test:
     * the mode is inherited, and Wine's runner reports the crash itself */
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    for (i = 3; i < argc; i++) {
        SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
        char stop[MAX_PATH], why[128] = "";
        FILE *sf;
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        HANDLE out;
        DWORD t0, rc = 0, w;

        /* a test's canary found no device after some test (winetest
         * patch 09, w98compat.h's wt_stop): every file after it would only
         * fail, until the machine restarts */
        snprintf(stop, sizeof stop, "%s\\STOP.TXT", dir);
        if ((sf = fopen(stop, "r"))) {
            if (!fgets(why, sizeof why, sf)) why[0] = 0;
            fclose(sf);
            say("wtrun: %s %s: not run, no device can be made after %s (restart the machine)", stem, argv[i], why);
            continue;
        }
        snprintf(path, sizeof path, "%s\\%s_%s.txt", dir, stem, argv[i]);
        out = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, NULL);
        if (out == INVALID_HANDLE_VALUE) {
            say("wtrun: %s %s: cannot write %s", stem, argv[i], path);
            continue;
        }
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = out;
        si.hStdError = out;
        snprintf(cmd, sizeof cmd, "\"%s\" %s", argv[2], argv[i]);
        say("wtrun: %s %s: start", stem, argv[i]);
        t0 = GetTickCount();
        if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            say("wtrun: %s %s: cannot start (error %lu)", stem, argv[i], GetLastError());
            CloseHandle(out);
            continue;
        }
        w = WaitForSingleObject(pi.hProcess, cap_ms);
        if (w == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 0xdead);
            WaitForSingleObject(pi.hProcess, 10000);
            say("wtrun: %s %s: timeout after %lu s", stem, argv[i],
                (GetTickCount() - t0) / 1000);
        } else {
            GetExitCodeProcess(pi.hProcess, &rc);
            say("wtrun: %s %s: exit %lu in %lu s", stem, argv[i], rc,
                (GetTickCount() - t0) / 1000);
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(out);
    }
    say("WTDONE");
    if (logf)
        fclose(logf);
    return 0;
}
