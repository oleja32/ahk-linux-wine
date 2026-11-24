/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Loaded into the unmodified AutoHotkey.exe as VERSION.dll. Replaces its input imports with
 * versions that talk to ahkd; everything else is stock AutoHotkey on Wine. */
#include <winsock2.h>
#include <windows.h>
#include "../proto.h"

#ifndef PM_QS_SENDMESSAGE
#define PM_QS_SENDMESSAGE (QS_SENDMESSAGE << 16)
#endif
#define WM_HOOKCALL (WM_APP + 0x3A1)
#define REQ_TIMEOUT 5000
#define FAKE_HOOK(k) ((HHOOK)(UINT_PTR)(0x0AB00001 + (k)))
#undef memset
#undef memcpy
void *memset(void *d, int c, size_t n) { volatile unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { volatile unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }

static HINSTANCE self;
static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION txlock;
static SOCKET sock = INVALID_SOCKET;
static volatile LONG online;
static volatile BYTE keys[256], latch[256];
static struct { HOOKPROC proc; HWND wnd; DWORD tid; LONG depth; } hk[2];
static struct { volatile LONG used; DWORD id; HANDLE ev; DWORD result; } req[64];
static struct { BOOL used; HWND wnd; int id; DWORD tid; } hot[1024];
static volatile LONG next_id, unsettled, presses, escs;
static volatile DWORD press_at;
typedef struct { ahkb_msg m; } call;

/* ---- native windows ---- */
#define NWIN 256
#define WTAG 0x7A000000u            /* fake HWND = WTAG | slot */
#define PTAG 0x40000000u            /* fake PID  = PTAG | unix pid */
typedef struct { DWORD slot, pid, min; int g[8]; WCHAR title[512], cls[128], exe[512]; } nwin;
static nwin nw[NWIN];
static CRITICAL_SECTION nwlock;
static struct { volatile LONG n; ahkb_key k[256]; } lay[2];  /* 0: base layout, for hotkey names; 1: active layout */
static volatile LONG lay_differs;
#define HKL_ACTIVE ((HKL)(UINT_PTR)0x0AB00409)
static volatile LONG nactive, have_active, desk_w, desk_h;
static struct { HANDLE h; DWORD pid; } ph[16];

static nwin *nw_find(DWORD slot) { if (slot) for (int i = 0; i < NWIN; i++) if (nw[i].slot == slot) return &nw[i]; return NULL; }
static DWORD slot_of(HWND h) { UINT_PTR v = (UINT_PTR)h; return (v & 0xFF000000u) == WTAG && v <= 0xFFFFFFFFu ? (DWORD)(v & 0xFFFFFF) : 0; }
static HWND hwnd_of(DWORD slot) { return (HWND)(UINT_PTR)(WTAG | slot); }
static BOOL nw_get(HWND h, nwin *o) { /* copy out */
    DWORD s = slot_of(h); BOOL ok = FALSE; nwin *w;
    if (!s) return FALSE;
    EnterCriticalSection(&nwlock);
    if ((w = nw_find(s))) { *o = *w; ok = TRUE; }
    LeaveCriticalSection(&nwlock);
    return ok;
}
static void nw_update(const ahkb_msg *m, const char *pl) {
    nwin *w; const char *s = pl + 8 * sizeof(int), *end = pl + m->c;
    EnterCriticalSection(&nwlock);
    w = nw_find(m->id);
    if (!m->a) { if (w) w->slot = 0; }
    else {
        if (!w) for (int i = 0; i < NWIN && !w; i++) if (!nw[i].slot) w = &nw[i];
        if (w && m->c > 8 * sizeof(int)) {
            WCHAR *d[3] = { w->title, w->cls, w->exe }; int cap[3] = { 512, 128, 512 };
            memcpy(w->g, pl, sizeof w->g); w->pid = m->b; w->min = m->d;
            for (int i = 0; i < 3 && s < end; i++) {
                if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, d[i], cap[i])) d[i][0] = 0;
                while (s < end && *s) s++;
                s++;
            }
            w->slot = m->id;
        }
    }
    LeaveCriticalSection(&nwlock);
}

static BOOL tx(const ahkb_msg *m, int n) {
    const char *p = (const char *)m; int len = n * (int)sizeof *m; BOOL ok = TRUE;
    EnterCriticalSection(&txlock);
    while (len > 0) { int r = send(sock, p, len, 0); if (r <= 0) { ok = FALSE; break; } p += r; len -= r; }
    LeaveCriticalSection(&txlock);
    return ok;
}
static DWORD WINAPI reader(void *unused) {
    ahkb_msg m; static char pl[8192];
    for (;;) {
        char *p = (char *)&m; int need = sizeof m;
        while (need) { int r = recv(sock, p, need, 0); if (r <= 0) goto out; p += r; need -= r; }
        if (m.type == D_WIN) {
            if (m.c >= sizeof pl) break;
            for (p = pl, need = m.c; need; ) { int r = recv(sock, p, need, 0); if (r <= 0) goto out; p += r; need -= r; }
            pl[m.c] = 0; nw_update(&m, pl); continue;
        }
        if (m.type == D_LAYOUT) {
            int w = m.a & 1;
            if (m.c > sizeof lay[w].k) break;
            for (p = pl, need = m.c; need; ) { int r = recv(sock, p, need, 0); if (r <= 0) goto out; p += r; need -= r; }
            lay[w].n = 0; memcpy(lay[w].k, pl, m.c); InterlockedExchange(&lay[w].n, m.c / sizeof(ahkb_key)); lay_differs = m.b;
            continue;
        }
        switch (m.type) {
        case D_ACTIVE: desk_w = m.a; desk_h = m.b; nactive = m.id; InterlockedExchange(&have_active, 1); break;
        case D_STATE:
            keys[m.a & 255] = (m.b & 1 ? 0x80 : 0) | (m.b & 2 ? 1 : 0);
            if (m.b & 4) {
                latch[m.a & 255] = 1;
                if (m.a == 1 || m.a == 2 || (m.a >= 4 && m.a <= 6)) { press_at = GetTickCount(); InterlockedIncrement(&presses); }
                if (m.a == VK_ESCAPE) InterlockedIncrement(&escs);
            }
            break;
        case D_DONE:
            for (int i = 0; i < 64; i++) if (req[i].used == 2 && req[i].id == m.id) { req[i].result = m.a; SetEvent(req[i].ev); break; }
            break;
        case D_HOTKEY:
            if (m.a < 1024 && hot[m.a].used) {
                if (hot[m.a].wnd) PostMessageW(hot[m.a].wnd, WM_HOTKEY, hot[m.a].id, MAKELPARAM(m.b, m.c));
                else PostThreadMessageW(hot[m.a].tid, WM_HOTKEY, hot[m.a].id, MAKELPARAM(m.b, m.c));
            }
            break;
        case D_SETTLED: InterlockedExchange(&unsettled, 0); break;
        case D_CALL: {
            HWND w = hk[m.a & 1].wnd;
            ahkb_msg *c = w ? HeapAlloc(GetProcessHeap(), 0, sizeof m) : NULL;
            if (c) *c = m;
            /* asynchronous *sent* message: dispatched inside GetMessage like a real LL hook call,
             * and the reader stays free to serve nested calls */
            if (!c || !SendNotifyMessageW(w, WM_HOOKCALL, 0, (LPARAM)c)) {
                if (c) HeapFree(GetProcessHeap(), 0, c);
                if (m.id) { ahkb_msg v = { C_VERDICT, m.id }; tx(&v, 1); }
            }
            break; }
        }
    }
out:
    InterlockedExchange(&online, 0);
    for (int i = 0; i < 64; i++) if (req[i].used == 2) SetEvent(req[i].ev);
    return 0;
}
static int hexv(char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
static BOOL CALLBACK init(PINIT_ONCE o, PVOID p, PVOID *q) {
    char port[16], tok[80]; BYTE raw[AHKB_TOKEN_LEN]; WSADATA wd; struct sockaddr_in sa; int one = 1, pn = 0;
    if (!GetEnvironmentVariableA("AHKB_PORT", port, sizeof port) || GetEnvironmentVariableA("AHKB_TOKEN", tok, sizeof tok) != AHKB_TOKEN_LEN * 2) return TRUE;
    for (int i = 0; i < AHKB_TOKEN_LEN; i++) raw[i] = hexv(tok[2 * i]) << 4 | hexv(tok[2 * i + 1]);
    for (char *c = port; *c; c++) pn = pn * 10 + *c - '0';
    if (WSAStartup(0x202, &wd)) return TRUE;
    sock = socket(AF_INET, SOCK_STREAM, 0);
    memset(&sa, 0, sizeof sa); sa.sin_family = AF_INET; sa.sin_port = htons(pn); sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (sock == INVALID_SOCKET || connect(sock, (struct sockaddr *)&sa, sizeof sa)) return TRUE;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *)&one, sizeof one);
    if (send(sock, (char *)raw, sizeof raw, 0) != sizeof raw) return TRUE;
    online = 1;
    CloseHandle(CreateThread(NULL, 0, reader, NULL, 0, NULL));
    Sleep(10); /* initial key-state snapshot */
    return TRUE;
}
static BOOL up(void) { InitOnceExecuteOnce(&once, init, NULL, NULL); return online != 0; }

/* Send messages and wait for D_DONE, dispatching sent messages meanwhile (as win32k does). */
static DWORD request(ahkb_msg *m, int n) {
    int i; DWORD r = 0, id, end;
    for (;;) { for (i = 0; i < 64; i++) if (!InterlockedCompareExchange(&req[i].used, 1, 0)) goto got; Sleep(1); }
got:
    if (!req[i].ev) req[i].ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    ResetEvent(req[i].ev);
    id = (DWORD)InterlockedIncrement(&next_id);
    m[0].id = id; req[i].id = id; req[i].result = 0;
    InterlockedExchange(&req[i].used, 2);
    if (tx(m, n)) {
        end = GetTickCount() + REQ_TIMEOUT;
        for (;;) {
            LONG left = (LONG)(end - GetTickCount()); DWORD w; MSG msg;
            if (left <= 0 || !online) break;
            w = MsgWaitForMultipleObjectsEx(1, &req[i].ev, left, QS_SENDMESSAGE, 0);
            if (w == WAIT_OBJECT_0) { r = req[i].result; break; }
            if (w == WAIT_OBJECT_0 + 1) PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
            else break;
        }
    }
    InterlockedExchange(&req[i].used, 0);
    return r;
}

/* ---- low-level hooks ---- */
static LRESULT CALLBACK hook_wnd(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOOKCALL) {
        ahkb_msg m = *(ahkb_msg *)lp; LRESULT r = 0; int k = m.a & 1;
        HeapFree(GetProcessHeap(), 0, (void *)lp);
        if (m.id) InterlockedExchange(&unsettled, 1);
        if (hk[k].proc) {
            hk[k].depth++;
            if (k == 0) {
                KBDLLHOOKSTRUCT s; s.vkCode = m.c & 0xffff; s.scanCode = m.c >> 16; s.flags = m.d; s.time = GetTickCount(); s.dwExtraInfo = (ULONG_PTR)m.extra;
                r = hk[k].proc(HC_ACTION, m.b, (LPARAM)&s);
            } else {
                MSLLHOOKSTRUCT s; GetCursorPos(&s.pt); s.mouseData = m.c; s.flags = m.d; s.time = GetTickCount(); s.dwExtraInfo = (ULONG_PTR)m.extra;
                r = hk[k].proc(HC_ACTION, m.b, (LPARAM)&s);
            }
            hk[k].depth--;
        }
        { static LONG lastmove = 0; ahkb_msg v = { C_VERDICT, m.id, r ? 1 : 0 };
          if (m.id) tx(&v, 1); else if (lastmove != (r ? 1 : 0)) { lastmove = r ? 1 : 0; tx(&v, 1); } }
        return 0;
    }
    return DefWindowProcW(w, msg, wp, lp);
}
static HHOOK WINAPI my_SetWindowsHookExW(int id, HOOKPROC p, HINSTANCE h, DWORD tid) {
    int k = id == WH_KEYBOARD_LL ? 0 : id == WH_MOUSE_LL ? 1 : -1;
    if (k < 0 || !up()) return SetWindowsHookExW(id, p, h, tid);
    static WNDCLASSW wc; wc.lpfnWndProc = hook_wnd; wc.hInstance = self; wc.lpszClassName = L"AhkBridgeHook"; RegisterClassW(&wc);
    HWND w = CreateWindowExW(0, L"AhkBridgeHook", NULL, 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, self, NULL);
    if (!w) return NULL;
    hk[k].proc = p; hk[k].tid = GetCurrentThreadId(); hk[k].wnd = w;
    ahkb_msg m = { C_HOOK, 0, k, 1 };
    if (!request(&m, 1)) { hk[k].wnd = NULL; hk[k].proc = NULL; DestroyWindow(w); SetLastError(ERROR_ACCESS_DENIED); return NULL; }
    return FAKE_HOOK(k);
}
static BOOL WINAPI my_UnhookWindowsHookEx(HHOOK h) {
    for (int k = 0; k < 2; k++) if (h == FAKE_HOOK(k)) {
        HWND w = hk[k].wnd;
        if (!w) { SetLastError(ERROR_INVALID_HOOK_HANDLE); return FALSE; }
        hk[k].proc = NULL; hk[k].wnd = NULL;
        ahkb_msg m = { C_HOOK, 0, k, 0 };
        if (online) request(&m, 1);
        if (GetCurrentThreadId() == hk[k].tid) DestroyWindow(w); else PostMessageW(w, WM_CLOSE, 0, 0);
        return TRUE;
    }
    return UnhookWindowsHookEx(h);
}
static LRESULT WINAPI my_CallNextHookEx(HHOOK h, int c, WPARAM w, LPARAM l) {
    if (h == FAKE_HOOK(0) || h == FAKE_HOOK(1)) return 0; /* ahkd walks the chain */
    return CallNextHookEx(h, c, w, l);
}

/* ---- injection ---- */
static int in_hook(void) { DWORD t = GetCurrentThreadId(); return (hk[0].depth && hk[0].tid == t) || (hk[1].depth && hk[1].tid == t); }
static UINT WINAPI my_SendInput(UINT n, LPINPUT in, int cb) {
    if (!up()) return SendInput(n, in, cb);
    if (!n || cb != sizeof(INPUT)) { SetLastError(ERROR_INVALID_PARAMETER); return 0; }
    ahkb_msg *m = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (n + 1) * sizeof *m); UINT r = 0, c = 0;
    if (!m) return 0;
    for (UINT i = 0; i < n; i++) {
        ahkb_msg *o = &m[1 + c];
        if (in[i].type == INPUT_KEYBOARD) { o->type = C_KEY; o->a = in[i].ki.wVk; o->b = in[i].ki.wScan; o->c = in[i].ki.dwFlags; o->extra = in[i].ki.dwExtraInfo; c++; }
        else if (in[i].type == INPUT_MOUSE) { o->type = C_MOUSE; o->a = in[i].mi.dx; o->b = in[i].mi.dy; o->c = in[i].mi.dwFlags; o->d = in[i].mi.mouseData; o->extra = in[i].mi.dwExtraInfo; c++; }
    }
    m[0].type = C_BATCH; m[0].a = c; m[0].b = in_hook();
    if (c) { request(m, c + 1); r = n; }
    HeapFree(GetProcessHeap(), 0, m);
    return r;
}
static void WINAPI my_keybd_event(BYTE vk, BYTE sc, DWORD fl, ULONG_PTR x) {
    INPUT i; memset(&i, 0, sizeof i); i.type = INPUT_KEYBOARD; i.ki.wVk = vk; i.ki.wScan = sc; i.ki.dwFlags = fl; i.ki.dwExtraInfo = x; my_SendInput(1, &i, sizeof i);
}
static void WINAPI my_mouse_event(DWORD fl, DWORD dx, DWORD dy, DWORD data, ULONG_PTR x) {
    INPUT i; memset(&i, 0, sizeof i); i.type = INPUT_MOUSE; i.mi.dx = dx; i.mi.dy = dy; i.mi.dwFlags = fl; i.mi.mouseData = data; i.mi.dwExtraInfo = x; my_SendInput(1, &i, sizeof i);
}
static BOOL WINAPI my_SetCursorPos(int x, int y) {
    int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
    if (!up() || w < 2 || h < 2) return SetCursorPos(x, y);
    my_mouse_event(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, (x * 65536 + 32768) / w, (y * 65536 + 32768) / h, 0, 0);
    return TRUE;
}

/* ---- key state ---- */
/* A hotkey thread can start before the event that triggered it has landed in the key state. Wait for that,
 * except inside a hook call or while serving one (#If evaluated for the hook), when it cannot land yet. */
static void settle(void) {
    DWORD t = GetCurrentThreadId();
    if (!unsettled || t == hk[0].tid || t == hk[1].tid || InSendMessage()) return;
    for (int i = 0; i < 20 && unsettled; i++) Sleep(1);
}
static SHORT WINAPI my_GetAsyncKeyState(int vk) {
    if (!up()) return GetAsyncKeyState(vk);
    settle();
    if (vk < 0 || vk > 255) return 0;
    SHORT r = (keys[vk] & 0x80 ? (SHORT)0x8000 : 0) | (latch[vk] ? 1 : 0); latch[vk] = 0; return r;
}
static SHORT WINAPI my_GetKeyState(int vk) {
    if (!up()) return GetKeyState(vk);
    settle();
    if (vk < 0 || vk > 255) return 0;
    return (SHORT)((keys[vk] & 0x80 ? 0xFF80 : 0) | (keys[vk] & 1));
}
static BOOL WINAPI my_GetKeyboardState(BYTE *b) {
    if (!up()) return GetKeyboardState(b);
    settle();
    for (int i = 0; i < 256; i++) b[i] = keys[i]; return TRUE;
}

/* ---- RegisterHotKey ---- */
static BOOL WINAPI my_RegisterHotKey(HWND w, int id, UINT mods, UINT vk) {
    if (!up()) return RegisterHotKey(w, id, mods, vk);
    int s = -1;
    for (int i = 0; i < 1024; i++) { if (!hot[i].used) { if (s < 0) s = i; } else if (hot[i].wnd == w && hot[i].id == id) { SetLastError(ERROR_HOTKEY_ALREADY_REGISTERED); return FALSE; } }
    if (s < 0) return FALSE;
    hot[s].wnd = w; hot[s].id = id; hot[s].tid = GetCurrentThreadId(); hot[s].used = TRUE;
    ahkb_msg m = { C_HKREG, 0, s, mods, vk };
    if (request(&m, 1)) return TRUE;
    hot[s].used = FALSE; SetLastError(ERROR_HOTKEY_ALREADY_REGISTERED); return FALSE;
}
static BOOL WINAPI my_UnregisterHotKey(HWND w, int id) {
    if (!up()) return UnregisterHotKey(w, id);
    for (int i = 0; i < 1024; i++) if (hot[i].used && hot[i].wnd == w && hot[i].id == id) {
        hot[i].used = FALSE; ahkb_msg m = { C_HKUNREG, 0, i }; return request(&m, 1) != 0;
    }
    SetLastError(ERROR_HOTKEY_NOT_REGISTERED); return FALSE;
}

/* ---- window queries: native windows answered from the table, Wine windows passed through ---- */
static int scale(int v, BOOL x) { /* compositor units -> the X pixels Wine uses */
    int wine = GetSystemMetrics(x ? SM_CXVIRTUALSCREEN : SM_CYVIRTUALSCREEN), d = x ? desk_w : desk_h;
    return d > 0 && wine > 0 && wine != d ? MulDiv(v, wine, d) : v;
}
/* AutoHotkey activates its hidden main window before showing a menu. KWin has nothing to activate then, so report
 * success for a moment; otherwise v2 falls back to pressing Alt. */
static HWND own_fore; static DWORD own_at;
static HWND WINAPI my_GetForegroundWindow(void) {
    DWORD a; HWND o = own_fore;
    if (o && GetTickCount() - own_at < 100 && GetWindowThreadProcessId(o, NULL) == GetCurrentThreadId()) return o;
    if (!up() || !have_active || (a = nactive) == 0xFFFFFFFFu) return GetForegroundWindow();
    return a ? hwnd_of(a) : NULL;
}
static BOOL WINAPI my_IsWindow(HWND h) { nwin w; return slot_of(h) ? nw_get(h, &w) : IsWindow(h); }
static BOOL WINAPI my_IsWindowVisible(HWND h) { nwin w; return slot_of(h) ? nw_get(h, &w) : IsWindowVisible(h); }
static BOOL WINAPI my_IsIconic(HWND h) { nwin w; return slot_of(h) ? nw_get(h, &w) && w.min : IsIconic(h); }
static BOOL WINAPI my_IsZoomed(HWND h) { return slot_of(h) ? FALSE : IsZoomed(h); }
static int copyw(WCHAR *d, int n, const WCHAR *s) { int i = 0; if (!d || n <= 0) return 0; while (i < n - 1 && s[i]) { d[i] = s[i]; i++; } d[i] = 0; return i; }
static int WINAPI my_GetWindowTextW(HWND h, LPWSTR b, int n) { nwin w; if (!slot_of(h)) return GetWindowTextW(h, b, n); if (nw_get(h, &w)) return copyw(b, n, w.title); if (n > 0) *b = 0; return 0; }
static int WINAPI my_GetWindowTextLengthW(HWND h) { nwin w; return !slot_of(h) ? GetWindowTextLengthW(h) : nw_get(h, &w) ? lstrlenW(w.title) : 0; }
static int WINAPI my_GetClassNameW(HWND h, LPWSTR b, int n) { nwin w; if (!slot_of(h)) return GetClassNameW(h, b, n); if (nw_get(h, &w)) return copyw(b, n, w.cls); if (n > 0) *b = 0; return 0; }
static DWORD WINAPI my_GetWindowThreadProcessId(HWND h, LPDWORD pid) {
    nwin w;
    if (!slot_of(h)) return GetWindowThreadProcessId(h, pid);
    if (!nw_get(h, &w)) { if (pid) *pid = 0; return 0; }
    if (pid) *pid = PTAG | w.pid;
    return PTAG | w.slot;
}
static BOOL WINAPI my_EnumWindows(WNDENUMPROC cb, LPARAM lp) {
    DWORD s[NWIN]; int n = 0; DWORD a;
    if (up() && have_active) {
        a = nactive;
        EnterCriticalSection(&nwlock);
        if (nw_find(a)) s[n++] = a; /* frontmost first, as in z-order */
        for (int i = 0; i < NWIN; i++) if (nw[i].slot && nw[i].slot != a) s[n++] = nw[i].slot;
        LeaveCriticalSection(&nwlock);
        for (int i = 0; i < n; i++) if (!cb(hwnd_of(s[i]), lp)) return FALSE;
    }
    return EnumWindows(cb, lp);
}
static BOOL WINAPI my_GetWindowRect(HWND h, LPRECT r) {
    nwin w;
    if (!slot_of(h)) return GetWindowRect(h, r);
    if (!nw_get(h, &w)) return FALSE;
    r->left = scale(w.g[0], TRUE); r->top = scale(w.g[1], FALSE); r->right = scale(w.g[0] + w.g[2], TRUE); r->bottom = scale(w.g[1] + w.g[3], FALSE);
    return TRUE;
}
static BOOL WINAPI my_GetClientRect(HWND h, LPRECT r) {
    nwin w;
    if (!slot_of(h)) return GetClientRect(h, r);
    if (!nw_get(h, &w)) return FALSE;
    r->left = r->top = 0; r->right = scale(w.g[6], TRUE); r->bottom = scale(w.g[7], FALSE);
    return TRUE;
}
static BOOL WINAPI my_ClientToScreen(HWND h, LPPOINT p) {
    nwin w;
    if (!slot_of(h)) return ClientToScreen(h, p);
    if (!nw_get(h, &w)) return FALSE;
    p->x += scale(w.g[4], TRUE); p->y += scale(w.g[5], FALSE);
    return TRUE;
}
static BOOL WINAPI my_ScreenToClient(HWND h, LPPOINT p) {
    nwin w;
    if (!slot_of(h)) return ScreenToClient(h, p);
    if (!nw_get(h, &w)) return FALSE;
    p->x -= scale(w.g[4], TRUE); p->y -= scale(w.g[5], FALSE);
    return TRUE;
}
static int WINAPI my_MapWindowPoints(HWND from, HWND to, LPPOINT p, UINT n) {
    if (!slot_of(from) && !slot_of(to)) return MapWindowPoints(from, to, p, n);
    for (UINT i = 0; i < n; i++) { if (from) my_ClientToScreen(from, &p[i]); if (to) my_ScreenToClient(to, &p[i]); }
    return 1;
}
static HWND WINAPI my_GetAncestor(HWND h, UINT f) { return !slot_of(h) ? GetAncestor(h, f) : f == GA_PARENT ? NULL : h; }
static LONG nstyle(int i) { return i == GWL_STYLE ? (LONG)(WS_OVERLAPPEDWINDOW | WS_VISIBLE) : 0; }
static LONG WINAPI my_GetWindowLongW(HWND h, int i) { return slot_of(h) ? nstyle(i) : GetWindowLongW(h, i); }
static LONG_PTR WINAPI my_GetWindowLongPtrW(HWND h, int i) { return slot_of(h) ? nstyle(i) : GetWindowLongPtrW(h, i); }
static BOOL winop(HWND h, int op) { ahkb_msg m = { C_WINOP, 0, op, slot_of(h) }; return up() && request(&m, 1); }
static BOOL WINAPI my_SetForegroundWindow(HWND h) {
    own_fore = NULL;
    if (!slot_of(h)) {
        BOOL r = SetForegroundWindow(h);
        if (!IsWindowVisible(h) && GetWindowThreadProcessId(h, NULL) == GetCurrentThreadId()) { own_at = GetTickCount(); own_fore = h; }
        return r;
    }
    if (!winop(h, W_ACTIVATE)) return FALSE;
    /* Win32 activates synchronously; give KWin a moment so AutoHotkey sees the result and does not fall back to pressing Alt */
    for (int i = 0; i < 40 && my_GetForegroundWindow() != h; i++) Sleep(5);
    return my_GetForegroundWindow() == h;
}
static BOOL WINAPI my_ShowWindow(HWND h, int c) {
    if (!slot_of(h)) return ShowWindow(h, c);
    switch (c) {
    case SW_MINIMIZE: case SW_SHOWMINIMIZED: case SW_SHOWMINNOACTIVE: case SW_FORCEMINIMIZE: return winop(h, W_MINIMIZE);
    case SW_MAXIMIZE: return winop(h, W_MAXIMIZE);
    case SW_RESTORE: case SW_SHOWNORMAL: return winop(h, W_RESTORE);
    }
    return TRUE;
}
static LRESULT WINAPI my_SendMessageTimeoutW(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT f, UINT t, PDWORD_PTR r) {
    if (!slot_of(h)) return SendMessageTimeoutW(h, msg, wp, lp, f, t, r);
    if (r) *r = 0;
    return msg == WM_NULL; /* "not hung"; native windows take no Win32 messages */
}
/* ---- keyboard layout: characters come from the compositor's layout, not Wine's ---- */
static HKL WINAPI my_GetKeyboardLayout(DWORD tid) {
    if ((tid & 0xC0000000u) != PTAG) return GetKeyboardLayout(tid);
    return up() && lay_differs && lay[1].n ? HKL_ACTIVE : GetKeyboardLayout(0);
}
static SHORT WINAPI my_VkKeyScanExW(WCHAR ch, HKL h) {
    int w = h == HKL_ACTIVE, n = up() ? lay[w].n : 0;
    if (!n || ch < 0x20) return VkKeyScanExW(ch, w ? GetKeyboardLayout(0) : h);
    for (int i = 0; i < n; i++) if (lay[w].k[i].ch == ch && lay[w].k[i].mods < 2) return (SHORT)(lay[w].k[i].vk | lay[w].k[i].mods << 8);
    return -1; /* AutoHotkey then sends the character itself, which ahkd types from another level or layout */
}
static SHORT WINAPI my_VkKeyScanW(WCHAR ch) { return my_VkKeyScanExW(ch, NULL); }
static int WINAPI my_ToUnicodeEx(UINT vk, UINT sc, const BYTE *st, LPWSTR b, int cap, UINT f, HKL h) {
    int w = h == HKL_ACTIVE, n = up() ? lay[w].n : 0, ctrl = st && st[VK_CONTROL] & 0x80, alt = st && st[VK_MENU] & 0x80, known = 0;
    if (n && st && !ctrl == !alt && cap > 0) {
        unsigned mods = (st[VK_SHIFT] & 0x80 ? 1 : 0) | (ctrl ? 2 : 0);
        WCHAR c = 0, o = 0; /* this key with these modifiers, and with Shift inverted */
        for (int i = 0; i < n; i++) if (lay[w].k[i].vk == vk) {
            known = 1;
            if (lay[w].k[i].mods == mods) c = lay[w].k[i].ch;
            if (lay[w].k[i].mods == (mods ^ 1)) o = lay[w].k[i].ch;
        }
        if (c && o && st[VK_CAPITAL] & 1 && (WCHAR)(ULONG_PTR)CharLowerW((LPWSTR)(ULONG_PTR)c) == (WCHAR)(ULONG_PTR)CharLowerW((LPWSTR)(ULONG_PTR)o)) c = o;
        if (c) { b[0] = c; if (cap > 1) b[1] = 0; return 1; }
        if (known && w) return 0;
    }
    return ToUnicodeEx(vk, sc, st, b, cap, f, w ? GetKeyboardLayout(0) : h);
}
static int WINAPI my_ToUnicode(UINT vk, UINT sc, const BYTE *st, LPWSTR b, int cap, UINT f) { return my_ToUnicodeEx(vk, sc, st, b, cap, f, NULL); }
static UINT WINAPI my_MapVirtualKeyExW(UINT c, UINT t, HKL h) { return MapVirtualKeyExW(c, t, h == HKL_ACTIVE ? GetKeyboardLayout(0) : h); }
/* ---- popup menus ---- */
/* Wine's menu is an XWayland window: it never sees a click on a native window, so it would stay open, above everything.
 * ahkd sees every mouse press; one that does not also reach the menu's thread landed elsewhere and closes the menu. */
#define MENU_CLICK_MS 150   /* for a press to travel compositor -> XWayland -> Wine */
#define MENU_TOGGLE_MS 1000
static struct { BOOL open; LONG presses, escs, active; DWORD got, closed_at; LONG closed_press; HHOOK hook; BOOL closed; } mn;
static BOOL over_menu(LPARAM lp) { /* in a menu loop, mouse messages carry screen coordinates */
    POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) }; WCHAR c[16];
    HWND w = WindowFromPoint(pt);
    return w && GetClassNameW(w, c, 16) && !lstrcmpW(c, L"#32768");
}
static LRESULT CALLBACK menu_filter(int code, WPARAM wp, LPARAM lp) {
    if (code == MSGF_MENU) switch (((MSG *)lp)->message) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN: case WM_NCXBUTTONDOWN:
    case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_NCLBUTTONDBLCLK: case WM_NCRBUTTONDBLCLK:
        if (over_menu(((MSG *)lp)->lParam)) mn.got = GetTickCount();
    }
    return CallNextHookEx(mn.hook, code, wp, lp);
}
static void CALLBACK menu_tick(HWND w, UINT msg, UINT_PTR id, DWORD t) {
    LONG a = nactive, p = presses; DWORD now = GetTickCount();
    if (have_active && a) { /* 0 = the panel or nothing; not a switch to another window */
        if (!mn.active) mn.active = a;
        if (a != mn.active) { EndMenu(); return; }
    }
    if (escs != mn.escs) { EndMenu(); return; }
    if (p == mn.presses) return;
    if (mn.got && (LONG)(mn.got - press_at) > -MENU_CLICK_MS) { mn.presses = p; return; } /* the menu got it */
    if (now - press_at < MENU_CLICK_MS) return;
    EndMenu();
}
static BOOL WINAPI my_TrackPopupMenuEx(HMENU m, UINT f, int x, int y, HWND w, LPTPMPARAMS tp) {
    BOOL r; UINT_PTR t; LONG p0;
    if (!up() || mn.open) return TrackPopupMenuEx(m, f, x, y, w, tp);
    /* the press that closed the menu was on the tray icon, and AutoHotkey now wants to open it again */
    if (mn.closed && mn.closed_press == presses && GetTickCount() - mn.closed_at < MENU_TOGGLE_MS) { mn.closed = FALSE; return FALSE; }
    memset(&mn, 0, sizeof mn);
    mn.open = TRUE; mn.presses = p0 = presses; mn.escs = escs; mn.active = have_active ? nactive : 0;
    mn.hook = SetWindowsHookExW(WH_MSGFILTER, menu_filter, NULL, GetCurrentThreadId());
    t = SetTimer(NULL, 0, 30, menu_tick);
    r = TrackPopupMenuEx(m, f, x, y, w, tp);
    KillTimer(NULL, t); UnhookWindowsHookEx(mn.hook);
    mn.open = FALSE;
    if (presses != p0) { mn.closed = TRUE; mn.closed_press = presses; mn.closed_at = GetTickCount(); }
    return r;
}
static BOOL WINAPI my_TrackPopupMenu(HMENU m, UINT f, int x, int y, int z, HWND w, const RECT *rc) { return my_TrackPopupMenuEx(m, f, x, y, w, NULL); }
/* process names: AutoHotkey opens the process, asks for its name once, closes it */
static HANDLE WINAPI my_OpenProcess(DWORD acc, BOOL inh, DWORD pid) {
    static volatile LONG next;
    if ((pid & 0xC0000000u) != PTAG) return OpenProcess(acc, inh, pid);
    HANDLE h = CreateEventW(NULL, FALSE, FALSE, NULL); /* a real handle, so CloseHandle works */
    if (h) { int i = InterlockedIncrement(&next) & 15; ph[i].pid = pid & ~PTAG; ph[i].h = h; }
    return h;
}
static int nw_exe(HANDLE h, WCHAR *out) { /* 0 = not ours */
    for (int i = 0; i < 16; i++) if (h && ph[i].h == h) {
        int n = 0; DWORD pid = ph[i].pid; ph[i].h = NULL;
        EnterCriticalSection(&nwlock);
        for (int j = 0; j < NWIN && !n; j++) if (nw[j].slot && nw[j].pid == pid) n = copyw(out, 512, nw[j].exe);
        LeaveCriticalSection(&nwlock);
        return n;
    }
    return 0;
}
static const WCHAR *wbase(const WCHAR *s) { const WCHAR *b = s; for (; *s; s++) if (*s == '\\') b = s + 1; return b; }
typedef DWORD (WINAPI *namefn)(HANDLE, HMODULE, LPWSTR, DWORD);
typedef DWORD (WINAPI *imgfn)(HANDLE, LPWSTR, DWORD);
static namefn real_base, real_full; static imgfn real_img;
static DWORD WINAPI my_GetModuleBaseNameW(HANDLE h, HMODULE m, LPWSTR b, DWORD n) { WCHAR e[512]; return nw_exe(h, e) ? copyw(b, n, wbase(e)) : real_base(h, m, b, n); }
static DWORD WINAPI my_GetModuleFileNameExW(HANDLE h, HMODULE m, LPWSTR b, DWORD n) { WCHAR e[512]; return nw_exe(h, e) ? copyw(b, n, e) : real_full(h, m, b, n); }
static DWORD WINAPI my_GetProcessImageFileNameW(HANDLE h, LPWSTR b, DWORD n) { WCHAR e[512]; return nw_exe(h, e) ? copyw(b, n, e) : real_img(h, b, n); }

/* ---- import patching ---- */
static FARPROC WINAPI my_GetProcAddress(HMODULE h, LPCSTR n);
static const struct { const char *name; void *fn; } tab[] = {
    { "SetWindowsHookExW", my_SetWindowsHookExW }, { "UnhookWindowsHookEx", my_UnhookWindowsHookEx }, { "CallNextHookEx", my_CallNextHookEx },
    { "SendInput", my_SendInput }, { "keybd_event", my_keybd_event }, { "mouse_event", my_mouse_event }, { "SetCursorPos", my_SetCursorPos },
    { "GetAsyncKeyState", my_GetAsyncKeyState }, { "GetKeyState", my_GetKeyState }, { "GetKeyboardState", my_GetKeyboardState },
    { "RegisterHotKey", my_RegisterHotKey }, { "UnregisterHotKey", my_UnregisterHotKey },
    { "GetForegroundWindow", my_GetForegroundWindow }, { "IsWindow", my_IsWindow }, { "IsWindowVisible", my_IsWindowVisible },
    { "IsIconic", my_IsIconic }, { "IsZoomed", my_IsZoomed }, { "GetWindowTextW", my_GetWindowTextW }, { "GetWindowTextLengthW", my_GetWindowTextLengthW },
    { "GetClassNameW", my_GetClassNameW }, { "GetWindowThreadProcessId", my_GetWindowThreadProcessId }, { "EnumWindows", my_EnumWindows },
    { "GetWindowRect", my_GetWindowRect }, { "GetClientRect", my_GetClientRect }, { "ClientToScreen", my_ClientToScreen }, { "ScreenToClient", my_ScreenToClient },
    { "MapWindowPoints", my_MapWindowPoints }, { "GetAncestor", my_GetAncestor },
    { "GetWindowLongW", my_GetWindowLongW }, { "GetWindowLongPtrW", my_GetWindowLongPtrW }, { "SetForegroundWindow", my_SetForegroundWindow },
    { "ShowWindow", my_ShowWindow }, { "SendMessageTimeoutW", my_SendMessageTimeoutW },
    { "TrackPopupMenuEx", my_TrackPopupMenuEx }, { "TrackPopupMenu", my_TrackPopupMenu },
    { "GetKeyboardLayout", my_GetKeyboardLayout }, { "VkKeyScanExW", my_VkKeyScanExW }, { "VkKeyScanW", my_VkKeyScanW },
    { "ToUnicodeEx", my_ToUnicodeEx }, { "ToUnicode", my_ToUnicode }, { "MapVirtualKeyExW", my_MapVirtualKeyExW },
    /* not user32: */
    { "OpenProcess", my_OpenProcess }, { "GetModuleBaseNameW", my_GetModuleBaseNameW }, { "GetModuleFileNameExW", my_GetModuleFileNameExW },
    { "GetProcessImageFileNameW", my_GetProcessImageFileNameW }, { "GetProcAddress", my_GetProcAddress },
};
#define NTAB (sizeof tab / sizeof *tab)
#define NOTUSER 5
static void *lookup(const char *n, BOOL user) {
    for (unsigned i = user ? 0 : NTAB - NOTUSER; i < (user ? NTAB - NOTUSER : NTAB); i++) if (!lstrcmpA(n, tab[i].name)) return tab[i].fn;
    return NULL;
}
/* scripts' DllCall() resolves through GetProcAddress */
static FARPROC WINAPI my_GetProcAddress(HMODULE h, LPCSTR n) {
    if ((ULONG_PTR)n > 0xffff && h == GetModuleHandleW(L"user32.dll")) { void *f = lookup(n, TRUE); if (f) return (FARPROC)f; }
    return GetProcAddress(h, n);
}
static void patch(void) {
    BYTE *b = (BYTE *)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    DWORD rva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if (!rva) return;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(b + rva); d->Name; d++) {
        const char *dll = (const char *)(b + d->Name);
        BOOL u = !lstrcmpiA(dll, "user32.dll");
        if (!u && lstrcmpiA(dll, "kernel32.dll") && lstrcmpiA(dll, "psapi.dll")) continue;
        IMAGE_THUNK_DATA *o = (IMAGE_THUNK_DATA *)(b + d->OriginalFirstThunk), *t = (IMAGE_THUNK_DATA *)(b + d->FirstThunk);
        if (!d->OriginalFirstThunk) continue;
        for (; o->u1.AddressOfData; o++, t++) {
            if (IMAGE_SNAP_BY_ORDINAL(o->u1.Ordinal)) continue;
            const char *n = (const char *)((IMAGE_IMPORT_BY_NAME *)(b + o->u1.AddressOfData))->Name;
            void *f = lookup(n, u); DWORD old;
            if (!f) continue;
            if (f == (void *)my_GetModuleBaseNameW) real_base = (namefn)t->u1.Function;
            if (f == (void *)my_GetModuleFileNameExW) real_full = (namefn)t->u1.Function;
            if (f == (void *)my_GetProcessImageFileNameW) real_img = (imgfn)t->u1.Function;
            VirtualProtect(&t->u1.Function, sizeof t->u1.Function, PAGE_READWRITE, &old);
            t->u1.Function = (ULONG_PTR)f;
            VirtualProtect(&t->u1.Function, sizeof t->u1.Function, old, &old);
        }
    }
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD why, LPVOID r) {
    if (why == DLL_PROCESS_ATTACH) { self = h; DisableThreadLibraryCalls(h); InitializeCriticalSection(&txlock); InitializeCriticalSection(&nwlock); patch(); }
    return TRUE;
}
