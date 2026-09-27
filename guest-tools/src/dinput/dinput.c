/*
 * dinput.c: the DINPUT.DLL shim (the disc's DINPUT\DINPUT.DLL, next to a
 * game's EXE): forwards DirectInputCreate{A,W,Ex} to the system dinput.dll, logs what
 * the game does with its devices, and fixes the keyboard state.
 *
 * The fix (FIFA 2000 on XP, doc 15): the game's keyboard device is
 * DISCL_NONEXCLUSIVE | DISCL_FOREGROUND and polled with GetDeviceState 30
 * times a second. On XP that device is fed by a low-level keyboard hook
 * that runs on the thread which created the device, and only while that
 * thread services its message queue; FIFA's match loop never does, so the
 * state stays empty for the whole match although Windows sees every key
 * (GetAsyncKeyState), while the front end, which pumps, works. The shim
 * sets every key GetAsyncKeyState reports pressed in the state it hands
 * back (DIK from the scan code, the extended keys mapped by hand), logged
 * once per key when DirectInput's own state lacked it.
 *
 * Two modes. By default the shim is the fix and nothing else: silent, no
 * log file, no sampler thread. That is the build that ships next to a game.
 * Setting D3DPT_DINPUT_LOG=1 in the environment before launching adds the
 * diagnostics described next. The merge is always on; only the observation
 * is optional, and under TCG it is not cheap (the sampler polls 248 virtual keys every 5 ms
 * and every log line is flushed).
 *
 * The log (dinput_log.txt next to the EXE): which device is created
 * (keyboard / mouse / other), the data format, cooperative level, buffer
 * size, event notification, every Acquire / Unacquire and its result, the
 * GetDeviceState / GetDeviceData / Poll call rates once a second, every key
 * or button the API hands back (state transitions and buffered events),
 * every DIERR_INPUTLOST; a sampler thread logs every GetAsyncKeyState
 * transition (what Windows sees) and the messages the keyboard thread
 * retrieves per second.
 *
 * The wrapped COM objects keep the original vtable's layout: every entry
 * we do not implement is a thunk that swaps `this` for the original object
 * and jumps to the original method, so any interface version (1, 2, 7,
 * A or W) works without knowing its size.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define DIRECTINPUT_VERSION 0x0700
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <string.h>

#define NVT 40                              /* vtable entries copied (IDirectInputDevice8: 32) */

typedef struct Wrap {
    void **vtbl;                            /* [0]: our copy */
    void *orig;                             /* [4]: the original object */
    void **orig_vtbl;                       /* [8]: its vtable */
    int is_device;                          /* 0: IDirectInput, 1: IDirectInputDevice */
    int kind;                               /* device: 0 unknown, 1 keyboard, 2 mouse, 3 other */
    char name[24];
    unsigned st_calls, dd_calls, poll_calls, st_fail, dd_fail;
    DWORD last_report;
    BYTE prev[256];
    BYTE merged_logged[256];
    void *thunks[NVT];
} Wrap;

static HMODULE sys;
static FILE *logf;                          /* diagnostics only: NULL unless D3DPT_DINPUT_LOG */
static CRITICAL_SECTION lock;
static DWORD t0;
static int ndev;

static void logp(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    if (!logf) return;
    va_start(ap, fmt);
    int n = _snprintf(buf, sizeof buf - 1, "%6lu ", (unsigned long)(GetTickCount() - t0));
    n += _vsnprintf(buf + n, sizeof buf - 1 - n, fmt, ap);
    va_end(ap);
    EnterCriticalSection(&lock);
    fputs(buf, logf);
    fputc('\n', logf);
    fflush(logf);
    LeaveCriticalSection(&lock);
}

/* ---- what Windows sees, independently of the game: a sampler thread logs
 * every GetAsyncKeyState transition (5 ms), and a WH_GETMESSAGE hook on the
 * thread that created the keyboard device counts the messages it retrieves
 * (DirectInput's non-exclusive keyboard on XP is fed by a hook that runs
 * only while that thread pumps messages) */
static volatile LONG pumped;
static HHOOK pump_hook;
static DWORD kbd_thread;
static LRESULT CALLBACK pump_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && wp == PM_REMOVE) InterlockedIncrement(&pumped);
    return CallNextHookEx(pump_hook, code, wp, lp);
}
static DWORD WINAPI sampler(LPVOID arg)
{
    SHORT prev[256];
    DWORD last = GetTickCount();
    (void)arg;
    memset(prev, 0, sizeof prev);
    for (;;) {
        for (int vk = 8; vk < 256; vk++) {
            SHORT s = GetAsyncKeyState(vk);
            if ((s ^ prev[vk]) & 0x8000) logp("async: VK 0x%02x %s", vk, (s & 0x8000) ? "down" : "up");
            prev[vk] = s;
        }
        DWORD now = GetTickCount();
        if (now - last >= 1000) {
            LONG n = InterlockedExchange(&pumped, 0);
            if (kbd_thread) logp("pump: keyboard thread %lu retrieved %ld messages in %lu ms", (unsigned long)kbd_thread, (long)n, (unsigned long)(now - last));
            last = now;
        }
        Sleep(5);
    }
    return 0;
}

/* ---- generic thunks: this -> orig, jump to orig vtbl[i] (stdcall: the callee pops) */
#define THUNK(i) \
    __attribute__((naked)) static void thunk_##i(void) { \
        __asm__ volatile("movl 4(%%esp), %%eax\n\t" "movl 8(%%eax), %%ecx\n\t" "movl 4(%%eax), %%eax\n\t" \
                         "movl %%eax, 4(%%esp)\n\t" "jmp *%c0(%%ecx)" : : "i"((i) * 4)); }
#define T10(a) THUNK(a##0) THUNK(a##1) THUNK(a##2) THUNK(a##3) THUNK(a##4) THUNK(a##5) THUNK(a##6) THUNK(a##7) THUNK(a##8) THUNK(a##9)
THUNK(0) THUNK(1) THUNK(2) THUNK(3) THUNK(4) THUNK(5) THUNK(6) THUNK(7) THUNK(8) THUNK(9)
T10(1) T10(2) T10(3)
#define E10(a) thunk_##a##0, thunk_##a##1, thunk_##a##2, thunk_##a##3, thunk_##a##4, thunk_##a##5, thunk_##a##6, thunk_##a##7, thunk_##a##8, thunk_##a##9
static void *const thunk_table[NVT] = { thunk_0, thunk_1, thunk_2, thunk_3, thunk_4, thunk_5, thunk_6, thunk_7, thunk_8, thunk_9, E10(1), E10(2), E10(3) };

static Wrap *wrap_new(void *orig, int is_device, const char *name)
{
    Wrap *w = (Wrap *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *w);
    w->orig = orig;
    w->orig_vtbl = *(void ***)orig;
    memcpy(w->thunks, thunk_table, sizeof w->thunks);
    w->vtbl = w->thunks;
    w->is_device = is_device;
    lstrcpynA(w->name, name, sizeof w->name);
    return w;
}
#define ORIG(w, i, T) ((T)((w)->orig_vtbl[i]))

/* ---- IDirectInputDevice wrappers (vtable indices of IDirectInputDevice7A) */
typedef HRESULT (WINAPI *fn_qi)(void *, REFIID, void **);
typedef HRESULT (WINAPI *fn_setprop)(void *, REFGUID, LPCDIPROPHEADER);
typedef HRESULT (WINAPI *fn_void)(void *);
typedef HRESULT (WINAPI *fn_state)(void *, DWORD, LPVOID);
typedef HRESULT (WINAPI *fn_data)(void *, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
typedef HRESULT (WINAPI *fn_fmt)(void *, LPCDIDATAFORMAT);
typedef HRESULT (WINAPI *fn_evt)(void *, HANDLE);
typedef HRESULT (WINAPI *fn_coop)(void *, HWND, DWORD);
typedef HRESULT (WINAPI *fn_create)(void *, REFGUID, void **, LPUNKNOWN);
typedef HRESULT (WINAPI *fn_createex)(void *, REFGUID, REFIID, void **, LPUNKNOWN);

static void dev_wrap_vtbl(Wrap *w);

static void report(Wrap *w)
{
    DWORD now;
    if (!logf) return;
    now = GetTickCount();
    if (now - w->last_report >= 1000) {
        if (w->st_calls || w->dd_calls || w->poll_calls)
            logp("%s: per %lu ms: GetDeviceState %u (%u failed), GetDeviceData %u (%u failed), Poll %u", w->name,
                 (unsigned long)(now - w->last_report), w->st_calls, w->st_fail, w->dd_calls, w->dd_fail, w->poll_calls);
        w->st_calls = w->dd_calls = w->poll_calls = w->st_fail = w->dd_fail = 0;
        w->last_report = now;
    }
}

static HRESULT WINAPI dev_QueryInterface(void *self, REFIID iid, void **out)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 0, fn_qi)(w->orig, iid, out);
    logp("%s: QueryInterface {%08lx-...} -> 0x%08lx", w->name, (unsigned long)iid->Data1, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {
        Wrap *n = wrap_new(*out, 1, w->name);
        n->kind = w->kind;
        dev_wrap_vtbl(n);
        *out = n;
    }
    return hr;
}
static HRESULT WINAPI dev_SetProperty(void *self, REFGUID guid, LPCDIPROPHEADER ph)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 6, fn_setprop)(w->orig, guid, ph);
    const char *what = guid == DIPROP_BUFFERSIZE ? "BUFFERSIZE" : guid == DIPROP_AXISMODE ? "AXISMODE" : "other";
    DWORD v = ph && ph->dwSize == sizeof(DIPROPDWORD) ? ((const DIPROPDWORD *)ph)->dwData : 0;
    logp("%s: SetProperty %s = %lu -> 0x%08lx", w->name, what, (unsigned long)v, (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI dev_Acquire(void *self)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 7, fn_void)(w->orig);
    logp("%s: Acquire -> 0x%08lx (foreground window %s ours)", w->name, (unsigned long)hr,
         GetForegroundWindow() && GetWindowThreadProcessId(GetForegroundWindow(), NULL) == GetCurrentThreadId() ? "is" : "is NOT");
    return hr;
}
static HRESULT WINAPI dev_Unacquire(void *self)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 8, fn_void)(w->orig);
    logp("%s: Unacquire -> 0x%08lx", w->name, (unsigned long)hr);
    return hr;
}
/* DIK code of a virtual key: the scan code, + 0x80 for the extended keys */
static BYTE vk_to_dik(int vk)
{
    switch (vk) {
    case VK_LEFT: return 0xcb; case VK_RIGHT: return 0xcd; case VK_UP: return 0xc8; case VK_DOWN: return 0xd0;
    case VK_INSERT: return 0xd2; case VK_DELETE: return 0xd3; case VK_HOME: return 0xc7; case VK_END: return 0xcf;
    case VK_PRIOR: return 0xc9; case VK_NEXT: return 0xd1; case VK_RCONTROL: return 0x9d; case VK_RMENU: return 0xb8;
    case VK_DIVIDE: return 0xb5; case VK_NUMLOCK: return 0xc5; case VK_LWIN: return 0xdb; case VK_RWIN: return 0xdc;
    case VK_APPS: return 0xdd; case VK_SHIFT: case VK_CONTROL: case VK_MENU: return 0;   /* the L/R ones map */
    default: { UINT sc = MapVirtualKeyA(vk, 0); return sc < 0x80 ? (BYTE)sc : 0; }
    }
}

static HRESULT WINAPI dev_GetDeviceState(void *self, DWORD size, LPVOID data)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 9, fn_state)(w->orig, size, data);
    w->st_calls++;
    if (FAILED(hr)) {
        if (w->st_fail++ == 0) logp("%s: GetDeviceState -> 0x%08lx%s", w->name, (unsigned long)hr, hr == DIERR_INPUTLOST ? " (INPUTLOST)" : hr == DIERR_NOTACQUIRED ? " (NOTACQUIRED)" : "");
    } else if (w->kind == 1 && size == 256) {
        /* the fix: a key Windows reports pressed (GetAsyncKeyState) is pressed
         * in the state we hand back, whether or not DirectInput's own hook
         * (serviced only when this thread pumps messages) has seen it */
        BYTE *st = (BYTE *)data;
        for (int vk = 8; vk < 256; vk++) {
            if (!(GetAsyncKeyState(vk) & 0x8000)) continue;
            BYTE dik = vk_to_dik(vk);
            if (dik && !(st[dik] & 0x80)) {
                st[dik] = 0x80;
                if (!w->merged_logged[dik]) { w->merged_logged[dik] = 1; logp("%s: DIK 0x%02x set from GetAsyncKeyState VK 0x%02x (DirectInput's state did not have it)", w->name, dik, vk); }
            }
        }
        if (logf) {
            const BYTE *s = (const BYTE *)data;
            for (int k = 0; k < 256; k++)
                if ((s[k] & 0x80) != (w->prev[k] & 0x80)) logp("%s: state DIK 0x%02x %s", w->name, k, (s[k] & 0x80) ? "down" : "up");
            memcpy(w->prev, s, 256);
        }
    } else if (w->kind == 2 && size >= 16 && logf) {
        const DIMOUSESTATE *m = (const DIMOUSESTATE *)data;
        for (int b = 0; b < 4; b++)
            if ((m->rgbButtons[b] & 0x80) != (w->prev[b] & 0x80)) logp("%s: state button %d %s", w->name, b, (m->rgbButtons[b] & 0x80) ? "down" : "up");
        memcpy(w->prev, m->rgbButtons, 4);
    }
    report(w);
    return hr;
}
static HRESULT WINAPI dev_GetDeviceData(void *self, DWORD size, LPDIDEVICEOBJECTDATA data, LPDWORD count, DWORD flags)
{
    Wrap *w = (Wrap *)self;
    DWORD want = count ? *count : 0;
    HRESULT hr = ORIG(w, 10, fn_data)(w->orig, size, data, count, flags);
    w->dd_calls++;
    if (FAILED(hr)) {
        if (w->dd_fail++ == 0) logp("%s: GetDeviceData -> 0x%08lx%s", w->name, (unsigned long)hr, hr == DIERR_INPUTLOST ? " (INPUTLOST)" : hr == DIERR_NOTACQUIRED ? " (NOTACQUIRED)" : "");
    } else if (data && count && !(flags & DIGDD_PEEK)) {
        for (DWORD i = 0; i < *count; i++)
            logp("%s: data ofs 0x%02lx = 0x%02lx seq %lu%s", w->name, (unsigned long)data[i].dwOfs, (unsigned long)data[i].dwData,
                 (unsigned long)data[i].dwSequence, hr == DI_BUFFEROVERFLOW ? " (BUFFEROVERFLOW)" : "");
    }
    (void)want;
    report(w);
    return hr;
}
static HRESULT WINAPI dev_SetDataFormat(void *self, LPCDIDATAFORMAT f)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 11, fn_fmt)(w->orig, f);
    if (f && f->dwDataSize == 256 && f->dwNumObjs == 256) w->kind = 1;
    else if (f && (f->dwDataSize == sizeof(DIMOUSESTATE) || f->dwDataSize == 20)) w->kind = 2;
    logp("%s: SetDataFormat (%lu objects, %lu bytes%s) -> 0x%08lx", w->name, f ? (unsigned long)f->dwNumObjs : 0UL,
         f ? (unsigned long)f->dwDataSize : 0UL, w->kind == 1 ? ": keyboard" : w->kind == 2 ? ": mouse" : "", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI dev_SetEventNotification(void *self, HANDLE h)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 12, fn_evt)(w->orig, h);
    logp("%s: SetEventNotification %p -> 0x%08lx", w->name, h, (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI dev_SetCooperativeLevel(void *self, HWND hwnd, DWORD flags)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 13, fn_coop)(w->orig, hwnd, flags);
    char title[64] = "";
    if (hwnd) GetWindowTextA(hwnd, title, sizeof title);
    logp("%s: SetCooperativeLevel hwnd %p \"%s\" flags 0x%lx (%s%s%s%s) -> 0x%08lx", w->name, hwnd, title, (unsigned long)flags,
         (flags & DISCL_EXCLUSIVE) ? "EXCLUSIVE " : "", (flags & DISCL_NONEXCLUSIVE) ? "NONEXCLUSIVE " : "",
         (flags & DISCL_FOREGROUND) ? "FOREGROUND" : "", (flags & DISCL_BACKGROUND) ? "BACKGROUND" : "", (unsigned long)hr);
    return hr;
}
static HRESULT WINAPI dev_Poll(void *self)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 25, fn_void)(w->orig);
    w->poll_calls++;
    return hr;
}

static void dev_wrap_vtbl(Wrap *w)
{
    w->thunks[0] = (void *)dev_QueryInterface;
    w->thunks[6] = (void *)dev_SetProperty;
    w->thunks[7] = (void *)dev_Acquire;
    w->thunks[8] = (void *)dev_Unacquire;
    w->thunks[9] = (void *)dev_GetDeviceState;
    w->thunks[10] = (void *)dev_GetDeviceData;
    w->thunks[11] = (void *)dev_SetDataFormat;
    w->thunks[12] = (void *)dev_SetEventNotification;
    w->thunks[13] = (void *)dev_SetCooperativeLevel;
    w->thunks[25] = (void *)dev_Poll;
}

/* ---- IDirectInput wrappers */
static const char *guid_name(REFGUID g)
{
    if (!g) return "null";
    if (IsEqualGUID(g, &GUID_SysKeyboard)) return "SysKeyboard";
    if (IsEqualGUID(g, &GUID_SysMouse)) return "SysMouse";
    return "other";
}
static Wrap *wrap_device(void *dev, REFGUID g)
{
    char name[24];
    _snprintf(name, sizeof name, "dev%d(%s)", ++ndev, guid_name(g));
    Wrap *w = wrap_new(dev, 1, name);
    dev_wrap_vtbl(w);
    if (g && IsEqualGUID(g, &GUID_SysKeyboard) && kbd_thread != GetCurrentThreadId()) {
        if (pump_hook) UnhookWindowsHookEx(pump_hook);
        kbd_thread = GetCurrentThreadId();
        pump_hook = SetWindowsHookExA(WH_GETMESSAGE, pump_proc, NULL, kbd_thread);
        logp("%s: created on thread %lu, message-pump hook %p", name, (unsigned long)kbd_thread, pump_hook);
    }
    return w;
}
static void di_wrap_vtbl(Wrap *w);
static HRESULT WINAPI di_QueryInterface(void *self, REFIID iid, void **out)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 0, fn_qi)(w->orig, iid, out);
    logp("%s: QueryInterface {%08lx-...} -> 0x%08lx", w->name, (unsigned long)iid->Data1, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) {
        Wrap *n = wrap_new(*out, 0, w->name);
        di_wrap_vtbl(n);
        *out = n;
    }
    return hr;
}
static HRESULT WINAPI di_CreateDevice(void *self, REFGUID g, void **out, LPUNKNOWN outer)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 3, fn_create)(w->orig, g, out, outer);
    logp("%s: CreateDevice(%s) -> 0x%08lx", w->name, guid_name(g), (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) *out = wrap_device(*out, g);
    return hr;
}
static HRESULT WINAPI di_CreateDeviceEx(void *self, REFGUID g, REFIID iid, void **out, LPUNKNOWN outer)
{
    Wrap *w = (Wrap *)self;
    HRESULT hr = ORIG(w, 9, fn_createex)(w->orig, g, iid, out, outer);
    logp("%s: CreateDeviceEx(%s, {%08lx-...}) -> 0x%08lx", w->name, guid_name(g), (unsigned long)iid->Data1, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) *out = wrap_device(*out, g);
    return hr;
}
static void di_wrap_vtbl(Wrap *w)
{
    w->thunks[0] = (void *)di_QueryInterface;
    w->thunks[3] = (void *)di_CreateDevice;
    w->thunks[9] = (void *)di_CreateDeviceEx;
}

/* ---- exports */
static void load_sys(void)
{
    if (sys) return;
    char path[MAX_PATH];
    GetSystemDirectoryA(path, sizeof path);
    lstrcatA(path, "\\dinput.dll");
    sys = LoadLibraryA(path);
    logp("system dinput.dll %s -> %p", path, sys);
}
#define SYS(name) (load_sys(), GetProcAddress(sys, name))

HRESULT WINAPI DirectInputCreateA(HINSTANCE inst, DWORD ver, LPDIRECTINPUTA *out, LPUNKNOWN outer)
{
    HRESULT (WINAPI *p)(HINSTANCE, DWORD, LPDIRECTINPUTA *, LPUNKNOWN) = (void *)SYS("DirectInputCreateA");
    HRESULT hr = p ? p(inst, ver, out, outer) : E_FAIL;
    logp("DirectInputCreateA(version 0x%04lx) -> 0x%08lx", (unsigned long)ver, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) { Wrap *w = wrap_new(*out, 0, "dinputA"); di_wrap_vtbl(w); *out = (LPDIRECTINPUTA)w; }
    return hr;
}
HRESULT WINAPI DirectInputCreateW(HINSTANCE inst, DWORD ver, LPDIRECTINPUTW *out, LPUNKNOWN outer)
{
    HRESULT (WINAPI *p)(HINSTANCE, DWORD, LPDIRECTINPUTW *, LPUNKNOWN) = (void *)SYS("DirectInputCreateW");
    HRESULT hr = p ? p(inst, ver, out, outer) : E_FAIL;
    logp("DirectInputCreateW(version 0x%04lx) -> 0x%08lx", (unsigned long)ver, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) { Wrap *w = wrap_new(*out, 0, "dinputW"); di_wrap_vtbl(w); *out = (LPDIRECTINPUTW)w; }
    return hr;
}
HRESULT WINAPI DirectInputCreateEx(HINSTANCE inst, DWORD ver, REFIID iid, void **out, LPUNKNOWN outer)
{
    HRESULT (WINAPI *p)(HINSTANCE, DWORD, REFIID, void **, LPUNKNOWN) = (void *)SYS("DirectInputCreateEx");
    HRESULT hr = p ? p(inst, ver, iid, out, outer) : E_FAIL;
    logp("DirectInputCreateEx(version 0x%04lx, {%08lx-...}) -> 0x%08lx", (unsigned long)ver, (unsigned long)iid->Data1, (unsigned long)hr);
    if (SUCCEEDED(hr) && out && *out) { Wrap *w = wrap_new(*out, 0, "dinputEx"); di_wrap_vtbl(w); *out = w; }
    return hr;
}
HRESULT WINAPI DllGetClassObject(REFCLSID clsid, REFIID iid, void **out)
{
    HRESULT (WINAPI *p)(REFCLSID, REFIID, void **) = (void *)SYS("DllGetClassObject");
    logp("DllGetClassObject (COM path, not wrapped)");
    return p ? p(clsid, iid, out) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI DllCanUnloadNow(void)
{
    HRESULT (WINAPI *p)(void) = (void *)SYS("DllCanUnloadNow");
    return p ? p() : S_FALSE;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)inst; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        char path[MAX_PATH], env[8];
        InitializeCriticalSection(&lock);
        t0 = GetTickCount();
        /* the fix runs always; the log and the sampler thread cost too much
         * under TCG to leave on in a game folder (header) */
        if (!(GetEnvironmentVariableA("D3DPT_DINPUT_LOG", env, sizeof env) > 0 && env[0] != '0'))
            return TRUE;
        GetModuleFileNameA(NULL, path, sizeof path);
        char *slash = strrchr(path, '\\');
        if (slash) lstrcpyA(slash + 1, "dinput_log.txt"); else lstrcpyA(path, "dinput_log.txt");
        logf = fopen(path, "a");
        logp("---- dinput shim loaded into pid %lu", (unsigned long)GetCurrentProcessId());
        CloseHandle(CreateThread(NULL, 0, sampler, NULL, 0, NULL));
    } else if (reason == DLL_PROCESS_DETACH) {
        logp("---- process detach");
        if (logf) fclose(logf);
        logf = NULL;
    }
    return TRUE;
}
