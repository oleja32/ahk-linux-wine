/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ahkd - input broker for the AutoHotkey bridge.
 * Reads evdev, routes events through client LL hooks exactly as win32k would, re-emits via uinput. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>
#include "../proto.h"

#define HOOK_TIMEOUT_MS 300           /* Windows' default LowLevelHooksTimeout */
#define IDLE_EXIT_MS    5000
#define PHYS_TAG        "ahk-bridge"
#define MAXDEV 64
#define MAXCLI 32
#define MAXHK  512
#define BIT(a, n) (((a)[(n) / 8] >> ((n) % 8)) & 1)

static int verbose;
static void lg(const char *f, ...) {
    if (!verbose) return;
    va_list a; va_start(a, f); vfprintf(stderr, f, a); va_end(a); fputc('\n', stderr);
}
static void die(const char *m) { perror(m); exit(1); }
static int64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

/* ---- evdev <-> Windows key tables ---- */
typedef struct { uint8_t vk, sc, ext; } km;
static km K[KEY_CNT];
static void ktab(void) {
    static const struct { uint16_t c; uint8_t vk, sc, e; } t[] = {
        {KEY_ESC,0x1B,1,0},{KEY_MINUS,0xBD,12,0},{KEY_EQUAL,0xBB,13,0},{KEY_BACKSPACE,8,14,0},{KEY_TAB,9,15,0},
        {KEY_LEFTBRACE,0xDB,26,0},{KEY_RIGHTBRACE,0xDD,27,0},{KEY_ENTER,13,28,0},{KEY_LEFTCTRL,0xA2,29,0},
        {KEY_SEMICOLON,0xBA,39,0},{KEY_APOSTROPHE,0xDE,40,0},{KEY_GRAVE,0xC0,41,0},{KEY_LEFTSHIFT,0xA0,42,0},
        {KEY_BACKSLASH,0xDC,43,0},{KEY_COMMA,0xBC,51,0},{KEY_DOT,0xBE,52,0},{KEY_SLASH,0xBF,53,0},
        {KEY_RIGHTSHIFT,0xA1,54,1},{KEY_KPASTERISK,0x6A,55,0},{KEY_LEFTALT,0xA4,56,0},{KEY_SPACE,0x20,57,0},
        {KEY_CAPSLOCK,0x14,58,0},{KEY_NUMLOCK,0x90,0x45,1},{KEY_SCROLLLOCK,0x91,0x46,0},{KEY_KPMINUS,0x6D,74,0},
        {KEY_KPPLUS,0x6B,78,0},{KEY_102ND,0xE2,86,0},{KEY_F11,0x7A,87,0},{KEY_F12,0x7B,88,0},
        {KEY_KPENTER,13,0x1C,1},{KEY_RIGHTCTRL,0xA3,0x1D,1},{KEY_KPSLASH,0x6F,0x35,1},{KEY_SYSRQ,0x2C,0x37,1},
        {KEY_RIGHTALT,0xA5,0x38,1},{KEY_HOME,0x24,0x47,1},{KEY_UP,0x26,0x48,1},{KEY_PAGEUP,0x21,0x49,1},
        {KEY_LEFT,0x25,0x4B,1},{KEY_RIGHT,0x27,0x4D,1},{KEY_END,0x23,0x4F,1},{KEY_DOWN,0x28,0x50,1},
        {KEY_PAGEDOWN,0x22,0x51,1},{KEY_INSERT,0x2D,0x52,1},{KEY_DELETE,0x2E,0x53,1},{KEY_PAUSE,0x13,0x45,0},
        {KEY_LEFTMETA,0x5B,0x5B,1},{KEY_RIGHTMETA,0x5C,0x5C,1},{KEY_COMPOSE,0x5D,0x5D,1},
        {KEY_MUTE,0xAD,0x20,1},{KEY_VOLUMEDOWN,0xAE,0x2E,1},{KEY_VOLUMEUP,0xAF,0x30,1},{KEY_NEXTSONG,0xB0,0x19,1},
        {KEY_PREVIOUSSONG,0xB1,0x10,1},{KEY_STOPCD,0xB2,0x24,1},{KEY_PLAYPAUSE,0xB3,0x22,1},{KEY_BACK,0xA6,0x6A,1},
        {KEY_FORWARD,0xA7,0x69,1},{KEY_REFRESH,0xA8,0x67,1},{KEY_SEARCH,0xAA,0x65,1},{KEY_HOMEPAGE,0xAC,0x32,1},
        {KEY_MAIL,0xB4,0x6C,1},{KEY_CALC,0xB7,0x21,1},
        {KEY_KP7,0x67,71,0},{KEY_KP8,0x68,72,0},{KEY_KP9,0x69,73,0},{KEY_KP4,0x64,75,0},{KEY_KP5,0x65,76,0},
        {KEY_KP6,0x66,77,0},{KEY_KP1,0x61,79,0},{KEY_KP2,0x62,80,0},{KEY_KP3,0x63,81,0},{KEY_KP0,0x60,82,0},{KEY_KPDOT,0x6E,83,0},
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) K[t[i].c] = (km){t[i].vk, t[i].sc, t[i].e};
    const char *rows = "qwertyuiop"; for (int i = 0; i < 10; i++) K[KEY_Q + i] = (km){rows[i] - 32, KEY_Q + i, 0};
    rows = "asdfghjkl"; for (int i = 0; i < 9; i++) K[KEY_A + i] = (km){rows[i] - 32, KEY_A + i, 0};
    rows = "zxcvbnm"; for (int i = 0; i < 7; i++) K[KEY_Z + i] = (km){rows[i] - 32, KEY_Z + i, 0};
    for (int i = 0; i < 9; i++) K[KEY_1 + i] = (km){'1' + i, KEY_1 + i, 0};
    K[KEY_0] = (km){'0', KEY_0, 0};
    for (int i = 0; i < 10; i++) K[KEY_F1 + i] = (km){0x70 + i, KEY_F1 + i, 0};
    for (int i = 0; i < 12; i++) K[KEY_F13 + i] = (km){0x7C + i, i == 11 ? 0x76 : 0x64 + i, 0};
}
/* numpad keys with NumLock off: evdev code -> nav VK */
static int kp_nav(int code) {
    switch (code) {
    case KEY_KP7: return 0x24; case KEY_KP8: return 0x26; case KEY_KP9: return 0x21; case KEY_KP4: return 0x25;
    case KEY_KP5: return 0x0C; case KEY_KP6: return 0x27; case KEY_KP1: return 0x23; case KEY_KP2: return 0x28;
    case KEY_KP3: return 0x22; case KEY_KP0: return 0x2D; case KEY_KPDOT: return 0x2E;
    } return 0;
}
static int vk_to_code(int vk, int sc, int ext) {
    if (vk == 0x10) vk = (sc & 0xff) == 0x36 ? 0xA1 : 0xA0;
    if (vk == 0x11) vk = ext ? 0xA3 : 0xA2;
    if (vk == 0x12) vk = ext ? 0xA5 : 0xA4;
    if (vk == 0x0D && ext) return KEY_KPENTER;
    if (sc && !ext) for (int c = KEY_KP7; c <= KEY_KPDOT; c++) if (vk && kp_nav(c) == vk) return c;
    if (vk == 0x0C) return KEY_KP5;
    if (vk) for (int c = 1; c < KEY_CNT; c++) if (K[c].vk == vk) return c;
    if (sc) for (int c = 1; c < KEY_CNT; c++) if (K[c].vk && K[c].sc == (sc & 0xff) && K[c].ext == !!ext) return c;
    return 0;
}

/* ---- state ---- */
typedef struct { int fd, node, kbd, mouse, grabbed, wheel, clone; int32_t rx, ry, rw, rhw, hw, hhw; } dev;
static dev D[MAXDEV];
typedef struct { int fd, authed, gen, hook[2], hook_seq[2], blockmove, batch_left; size_t n; uint8_t rx[65536]; struct task *batch; } cli;
static cli C[MAXCLI];
static struct { int used, gen; uint32_t cookie, mods, vk; } HK[MAXHK];
static uint8_t state[256], toggled[256], out_down[KEY_CNT], raw_down[KEY_CNT];
static int ep, ino, lsock, okbd = -1, omouse = -1, oabs = -1, seqno, gens;
static uint8_t token[AHKB_TOKEN_LEN];
static int64_t idle_since, ungrab_at;
static const char *only;
static volatile sig_atomic_t beat, missed;

static void emit(int fd, int t, int c, int v) { struct input_event e = {.type = t, .code = c, .value = v}; if (write(fd, &e, sizeof e) < 0) lg("emit: %s", strerror(errno)); }
static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

static void drop_client(cli *c);
static void csend(cli *c, ahkb_msg m) {
    if (c->fd < 0) return;
    if (send(c->fd, &m, sizeof m, MSG_DONTWAIT | MSG_NOSIGNAL) != sizeof m) { lg("client stalled"); shutdown(c->fd, SHUT_RDWR); }
}
static void bcast_state(int vk, int went) {
    ahkb_msg m = {D_STATE, 0, vk, (state[vk] ? 1 : 0) | (toggled[vk] ? 2 : 0) | (went ? 4 : 0)};
    for (int i = 0; i < MAXCLI; i++) if (C[i].fd >= 0 && C[i].authed) csend(&C[i], m);
}
static void set_state(int vk, int down) {
    int went = down && !state[vk];
    state[vk] = down;
    if (went && vk) toggled[vk] ^= 1;
    bcast_state(vk, went);
    int g = (vk & 0xFE) == 0xA0 ? 0x10 : (vk & 0xFE) == 0xA2 ? 0x11 : (vk & 0xFE) == 0xA4 ? 0x12 : 0;
    if (g) { int v = state[vk & 0xFE] || state[vk | 1]; int w = v && !state[g]; state[g] = v; bcast_state(g, w); }
}

/* ---- uinput ---- */
static int ui_open(const char *name, int vendor, int product, int version, int bus, int kind) { /* 0 kbd 1 mouse 2 abs */
    int fd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) die("/dev/uinput");
    struct uinput_setup s = {.id = {bus, vendor, product, version}};
    snprintf(s.name, sizeof s.name, "%s", name);
    ioctl(fd, UI_SET_PHYS, PHYS_TAG);
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    if (kind == 0) {
        for (int c = 1; c < 0x100; c++) ioctl(fd, UI_SET_KEYBIT, c);
        for (int c = 0x160; c < 0x220; c++) ioctl(fd, UI_SET_KEYBIT, c);
        ioctl(fd, UI_SET_EVBIT, EV_LED);
        for (int l = 0; l <= LED_SCROLLL; l++) ioctl(fd, UI_SET_LEDBIT, l);
    } else {
        for (int c = BTN_LEFT; c <= BTN_TASK; c++) ioctl(fd, UI_SET_KEYBIT, c);
        if (kind == 1) {
            ioctl(fd, UI_SET_EVBIT, EV_REL);
            int r[] = {REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES};
            for (int i = 0; i < 6; i++) ioctl(fd, UI_SET_RELBIT, r[i]);
        } else {
            ioctl(fd, UI_SET_EVBIT, EV_ABS);
            for (int a = ABS_X; a <= ABS_Y; a++) { struct uinput_abs_setup x = {.code = a, .absinfo = {.maximum = 65535}}; ioctl(fd, UI_ABS_SETUP, &x); }
        }
    }
    if (ioctl(fd, UI_DEV_SETUP, &s) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) die("uinput create");
    return fd;
}
static void ep_add(int fd, uint64_t tag) { struct epoll_event e = {.events = EPOLLIN, .data.u64 = tag}; epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e); }
enum { T_LISTEN = 1, T_INO, T_OKBD, T_BUS, T_DEV = 0x1000, T_CLI = 0x2000 };

/* ---- devices ---- */
static void dev_close(dev *d) {
    if (d->fd < 0) return;
    lg("device %d closed", d->node);
    uint8_t kb[KEY_CNT / 8 + 1] = {0};
    (void)kb;
    close(d->fd); d->fd = -1;
    if (d->clone >= 0) { close(d->clone); d->clone = -1; }
}
static void dev_open(int node) {
    for (int i = 0; i < MAXDEV; i++) if (D[i].fd >= 0 && D[i].node == node) return;
    char p[64]; snprintf(p, sizeof p, "/dev/input/event%d", node);
    int fd = open(p, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    char name[256] = "", phys[256] = "";
    ioctl(fd, EVIOCGNAME(sizeof name - 1), name); ioctl(fd, EVIOCGPHYS(sizeof phys - 1), phys);
    uint8_t kb[KEY_CNT / 8 + 1] = {0}, rb[4] = {0}, ab[8] = {0}, ev[4] = {0};
    ioctl(fd, EVIOCGBIT(0, sizeof ev), ev); ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof rb), rb); ioctl(fd, EVIOCGBIT(EV_ABS, sizeof ab), ab);
    int hasabs = BIT(ab, ABS_X) || BIT(ab, ABS_MT_POSITION_X), sw = BIT(ev, EV_SW);
    int mouse = BIT(rb, REL_X) && BIT(kb, BTN_LEFT) && !hasabs;
    int kbd = !hasabs && !sw && ((BIT(kb, KEY_A) && BIT(kb, KEY_Z) && BIT(kb, KEY_ENTER)) || BIT(kb, KEY_PLAYPAUSE) || BIT(kb, KEY_VOLUMEUP));
    if (!strcmp(phys, PHYS_TAG) || (only && !strstr(name, only)) || (!mouse && !kbd)) { close(fd); return; }
    for (int i = 0; i < MAXDEV; i++) if (D[i].fd < 0) {
        D[i] = (dev){.fd = fd, .node = node, .kbd = kbd, .mouse = mouse, .clone = -1};
        ep_add(fd, T_DEV + i);
        uint8_t led[2] = {0};
        if (kbd && BIT(ev, EV_LED) && ioctl(fd, EVIOCGLED(sizeof led), led) >= 0) {
            toggled[0x90] |= BIT(led, LED_NUML); toggled[0x14] |= BIT(led, LED_CAPSL); toggled[0x91] |= BIT(led, LED_SCROLLL);
        }
        lg("device %d '%s'%s%s", node, name, kbd ? " kbd" : "", mouse ? " mouse" : "");
        return;
    }
    close(fd);
}
static void dev_scan(void) {
    DIR *d = opendir("/dev/input"); if (!d) return;
    struct dirent *e; int n;
    while ((e = readdir(d))) if (sscanf(e->d_name, "event%d", &n) == 1) dev_open(n);
    closedir(d);
}
static int want_grab(void) {
    int w = 0; /* bit0: keyboards, bit1: mice */
    for (int i = 0; i < MAXCLI; i++) if (C[i].fd >= 0) { if (C[i].hook[0]) w |= 1; if (C[i].hook[1]) w |= 2; }
    for (int i = 0; i < MAXHK; i++) if (HK[i].used) w |= 1;
    return w;
}
/* Keyboards and mice are grabbed only while something could suppress their events, and only once no
 * key is physically held (otherwise the compositor would never see the release). */
static int regrab(void) {
    int w = want_grab(), pending = 0;
    if (w) ungrab_at = 0; else if (!ungrab_at) ungrab_at = now_ms() + 1000;
    for (int i = 0; i < MAXDEV; i++) {
        dev *d = &D[i]; if (d->fd < 0) continue;
        int want = (d->kbd && (w & 1)) || (d->mouse && (w & 2));
        if (want == d->grabbed) continue;
        if (!want && !w && now_ms() < ungrab_at) { pending = 1; continue; }
        uint8_t kb[KEY_CNT / 8 + 1] = {0}; int held = 0;
        ioctl(d->fd, EVIOCGKEY(sizeof kb), kb);
        for (size_t j = 0; j < sizeof kb; j++) held |= kb[j];
        if (held) { pending = 1; continue; }
        if (want && d->mouse && d->clone < 0) {
            struct input_id id = {0}; char name[256] = ""; uint8_t rb[4] = {0};
            ioctl(d->fd, EVIOCGID, &id); ioctl(d->fd, EVIOCGNAME(sizeof name - 1), name); ioctl(d->fd, EVIOCGBIT(EV_REL, sizeof rb), rb);
            d->wheel = BIT(rb, REL_WHEEL_HI_RES);
            d->clone = ui_open(name, id.vendor, id.product, id.version, id.bustype, 1);
        }
        if (ioctl(d->fd, EVIOCGRAB, want ? (void *)1 : (void *)0) < 0) { lg("grab %d: %s", d->node, strerror(errno)); pending = 1; continue; }
        d->grabbed = want; lg("device %d %s", d->node, want ? "grabbed" : "released");
    }
    return pending;
}

/* ---- events and tasks ---- */
enum { E_KEY, E_BTN, E_WHEEL, E_HWHEEL, E_MOTION, E_REL, E_ABS, E_UNI };
typedef struct { uint8_t kind, inj, suppress; int dev; int code, val, x, y; uint32_t vk, sc, ext; uint64_t extra; } ev;
typedef struct task { struct task *next; ev *e; int n, cap, i, rgen; uint32_t rid; int stage_seq, waiting, wgen, hooked; uint32_t call; int64_t deadline; } task;
static task *stack, *qh, **qt = &qh;
static uint32_t callno;

static cli *by_gen(int g) { for (int i = 0; i < MAXCLI; i++) if (C[i].fd >= 0 && C[i].gen == g) return &C[i]; return NULL; }
static void tpush(task *t, ev e) { if (t->n == t->cap) { t->cap = t->cap ? t->cap * 2 : 4; t->e = realloc(t->e, t->cap * sizeof(ev)); } t->e[t->n++] = e; }
static task *tnew(void) { task *t = calloc(1, sizeof *t); t->stage_seq = -1; return t; }
static void tq(task *t) { t->next = NULL; *qt = t; qt = &t->next; }
static void q1(ev e) { task *t = tnew(); tpush(t, e); tq(t); }

/* ---- keyboard layouts ----
 * Text is typed through the compositor's layouts: a character missing from the active one is typed by switching the layout
 * for a moment. Clients get the tables, so AutoHotkey finds keys as it would from a Windows layout. */
#define MAXLAY 16
#define LAY_TIMEOUT_MS 250    /* a layout switch that takes longer is given up; input must never hang on it */
#define LAY_SETTLE_MS  20     /* for the compositor to read emitted keys before the layout changes under them */
#define LAY_RESTORE_MS 150
static struct { int n, latin; struct { uint32_t ch; uint16_t code, mods; } e[256]; } L[MAXLAY];
static int nlay, lay_base, lay_cur, lay_real, lay_target, lay_wait; /* cur: the user's layout; real: what the compositor has now */
static int64_t lay_deadline, lay_typed, lay_off, lay_late;
static sd_bus *bus;

static int lay_find(int g, uint32_t ch) { for (int i = 0; i < L[g].n; i++) if (L[g].e[i].ch == ch) return i; return -1; }
static int lay_load(const char *layout, const char *variant, int g) {
    struct xkb_context *x = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_rule_names r = {.layout = layout, .variant = variant, .options = ""};
    struct xkb_keymap *m = x ? xkb_keymap_new_from_names(x, &r, 0) : NULL;
    struct xkb_state *s = m ? xkb_state_new(m) : NULL;
    L[g].n = 0;
    if (s) {
        xkb_mod_index_t sh = xkb_keymap_mod_get_index(m, "Shift"), l3 = xkb_keymap_mod_get_index(m, "Mod5");
        for (int pass = 0; pass < 2; pass++) /* the extra key of ISO keyboards last: it may not exist */
            for (int mods = 0; mods < 4; mods++) {
                xkb_state_update_mask(s, (mods & 1 ? 1u << sh : 0) | (mods & 2 ? 1u << l3 : 0), 0, 0, 0, 0, 0);
                for (int c = 1; c < 128; c++) {
                    int kp = (c >= KEY_KP7 && c <= KEY_KPDOT) || c == KEY_KPASTERISK || c == KEY_KPSLASH || c == KEY_KPENTER;
                    if (!K[c].vk || kp || (c == KEY_102ND) != pass || L[g].n == 256) continue;
                    uint32_t ch = xkb_state_key_get_utf32(s, c + 8); int dup = 0;
                    if (ch < 0x20 || ch == 0x7f || ch > 0xffff) continue;
                    for (int i = 0; i < L[g].n; i++) dup |= L[g].e[i].code == c && L[g].e[i].ch == ch; /* AltGr without a third level */
                    if (dup) continue;
                    L[g].e[L[g].n].ch = ch; L[g].e[L[g].n].code = c; L[g].e[L[g].n].mods = mods; L[g].n++;
                }
            }
        L[g].latin = lay_find(g, 'a') >= 0 && lay_find(g, 'z') >= 0;
    }
    xkb_state_unref(s); xkb_keymap_unref(m); xkb_context_unref(x);
    lg("layout %d '%s' '%s': %d characters%s", g, layout, variant, L[g].n, L[g].latin ? ", latin" : "");
    return L[g].n;
}
static void lay_send(cli *c, int which) {
    int g = which ? lay_cur : lay_base; char b[sizeof(ahkb_msg) + 256 * sizeof(ahkb_key)]; ahkb_key *k = (ahkb_key *)(b + sizeof(ahkb_msg));
    if (!nlay) return;
    for (int i = 0; i < L[g].n; i++) k[i] = (ahkb_key){L[g].e[i].ch, K[L[g].e[i].code].vk, L[g].e[i].mods};
    ahkb_msg m = {D_LAYOUT, 0, which, lay_cur != lay_base, L[g].n * sizeof *k};
    memcpy(b, &m, sizeof m);
    ssize_t n = sizeof m + m.c;
    if (c->fd >= 0 && c->authed && send(c->fd, b, n, MSG_DONTWAIT | MSG_NOSIGNAL) != n) shutdown(c->fd, SHUT_RDWR);
}
static void lay_bcast(void) { for (int i = 0; i < MAXCLI; i++) { lay_send(&C[i], 0); lay_send(&C[i], 1); } }
static void lay_set(int cur) { /* the user's choice */
    if (cur < 0 || cur >= nlay) return;
    lay_cur = lay_real = cur; lay_wait = 0; lg("layout is %d", cur); lay_bcast();
}
static void lay_done(void) { lay_base = 0; for (int i = nlay - 1; i >= 0; i--) if (L[i].latin) lay_base = i; }
static void ini(const char *file, const char *key, char *out, size_t n) { /* Key=value, or Option "Key" "value" */
    char l[512]; FILE *f = fopen(file, "r"); size_t k = strlen(key);
    if (!f) return;
    while (fgets(l, sizeof l, f)) {
        char *p = l + strspn(l, " \t"), *v = NULL;
        if (!strncmp(p, key, k) && p[k] == '=') v = p + k + 1;
        else if ((p = strstr(l, key)) && p > l && p[-1] == '"' && p[k] == '"' && (v = strchr(p + k + 1, '"'))) v++;
        if (v) { v[strcspn(v, "\"\r\n")] = 0; snprintf(out, n, "%s", v); }
    }
    fclose(f);
}
static void lay_from_files(void) { /* with a single layout KWin offers no D-Bus interface */
    char p[512], ls[256] = "", vs[256] = ""; const char *c = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
    snprintf(p, sizeof p, "%s%s/kxkbrc", c && *c ? c : h ? h : "", c && *c ? "" : "/.config");
    ini(p, "LayoutList", ls, sizeof ls); ini(p, "VariantList", vs, sizeof vs);
    if (!*ls) { ini("/etc/X11/xorg.conf.d/00-keyboard.conf", "XkbLayout", ls, sizeof ls); ini("/etc/X11/xorg.conf.d/00-keyboard.conf", "XkbVariant", vs, sizeof vs); }
    if (!*ls) strcpy(ls, "us");
    ls[strcspn(ls, ",")] = 0; vs[strcspn(vs, ",")] = 0; /* the first one is all we can know to be active */
    nlay = lay_load(ls, vs, 0) > 0; lay_done(); lay_set(0);
}
static sd_bus_message *lay_ask(const char *method) { /* a short wait, never starting a service */
    sd_bus_message *q = NULL, *r = NULL;
    if (bus && sd_bus_message_new_method_call(bus, &q, "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", method) >= 0 && sd_bus_message_set_auto_start(q, 0) >= 0)
        sd_bus_call(bus, q, 250000, NULL, &r);
    sd_bus_message_unref(q);
    return r;
}
static void lay_query(void) {
    sd_bus_message *r = lay_ask("getLayoutsList"); uint32_t cur = 0; int n = 0; const char *l, *v, *d;
    if (r && sd_bus_message_enter_container(r, 'a', "(sss)") >= 0)
        while (n < MAXLAY && sd_bus_message_read(r, "(sss)", &l, &v, &d) > 0) { lay_load(l, v, n); n++; }
    sd_bus_message_unref(r);
    if (!n) { lay_from_files(); return; }
    if ((r = lay_ask("getLayout"))) sd_bus_message_read(r, "u", &cur);
    sd_bus_message_unref(r);
    nlay = n; lay_done(); lay_set(cur < (uint32_t)n ? cur : 0);
}
static int on_laylist(sd_bus_message *m, void *u, sd_bus_error *e) { (void)m; (void)u; (void)e; lay_query(); return 0; }
static int on_layout(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e; uint32_t i;
    if (sd_bus_message_read(m, "u", &i) < 0 || (int)i >= nlay) return 0;
    if ((int)i == lay_target && (int)i != lay_cur && (lay_wait || now_ms() < lay_late)) { /* our own switch, perhaps after we gave up on it */
        lay_real = i; if (!lay_wait) lay_off = 0;
    }
    else if ((int)i != lay_real) lay_set(i);
    return 0;
}
static int on_setlay(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e; int ok = 0;
    if ((int)(intptr_t)u != lay_target || !lay_wait) return 0;
    if (!sd_bus_message_is_method_error(m, NULL)) sd_bus_message_read(m, "b", &ok);
    lay_wait = 0;
    if (ok) lay_real = lay_target; else { lg("layout switch refused"); lay_off = now_ms() + 5000; }
    return 0;
}
/* 1 = the compositor has layout g (or switching does not work); 0 = wait */
static int lay_goto(int g) {
    int64_t n = now_ms();
    if (lay_real == g || n < lay_off || !bus) return 1;
    if (lay_wait && n > lay_deadline) { lg("layout switch timed out"); lay_wait = 0; lay_off = n + 5000; lay_late = n + 30000; return 1; }
    if (lay_wait || n - lay_typed < LAY_SETTLE_MS) return 0;
    lay_target = g; lay_wait = 1; lay_deadline = n + LAY_TIMEOUT_MS;
    if (sd_bus_call_method_async(bus, NULL, "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "setLayout", on_setlay, (void *)(intptr_t)g, "u", g) < 0) { lay_wait = 0; lay_off = n + 5000; return 1; }
    return 0;
}
/* the layout an event must be typed in, -1 = any */
static int lay_need(ev *e, int pass) {
    if (e->kind == E_UNI) {
        if (e->code) return -1; /* Enter, Tab */
        if (lay_find(lay_cur, e->vk) >= 0) return e->x = lay_cur;
        for (int g = 0; g < nlay; g++) if (lay_find(g, e->vk) >= 0) return e->x = g;
        fprintf(stderr, "ahkd: cannot type U+%04X (not on any of your keyboard layouts)\n", e->vk); e->suppress = 1;
        return -1;
    }
    if (e->kind != E_KEY || !e->val || !pass || lay_real == lay_cur) return -1;
    for (int i = 0; i < L[lay_cur].n; i++) if (L[lay_cur].e[i].code == e->code) return lay_cur;
    return -1;
}

static int btn_vk(int code) { switch (code) { case BTN_LEFT: return 1; case BTN_RIGHT: return 2; case BTN_MIDDLE: return 4; case BTN_SIDE: case BTN_BACK: return 5; case BTN_EXTRA: case BTN_FORWARD: return 6; } return 0; }

static void hook_msg(ev *e, uint32_t id, ahkb_msg *m) {
    *m = (ahkb_msg){.type = D_CALL, .id = id, .extra = e->extra};
    if (e->kind == E_KEY) {
        int alt = state[0x12], ctrl = state[0x11], up = !e->val;
        int isalt = e->vk == 0xA4 || e->vk == 0xA5;
        int sys = !ctrl && (up ? alt : (alt || isalt)); if (e->vk == 0x79 && !ctrl) sys = 1;
        m->a = 0; m->b = up ? (sys ? 0x105 : 0x101) : (sys ? 0x104 : 0x100);
        m->c = e->vk | e->sc << 16;
        m->d = (e->ext ? 1 : 0) | (e->inj ? 0x10 : 0) | ((alt || (isalt && !up)) ? 0x20 : 0) | (up ? 0x80 : 0);
    } else {
        m->a = 1; m->d = e->inj ? 1 : 0;
        if (e->kind == E_BTN) {
            int vk = btn_vk(e->code), dn = e->val;
            m->b = vk == 1 ? 0x202 - dn : vk == 2 ? 0x205 - dn : vk == 4 ? 0x208 - dn : 0x20C - dn;
            m->c = vk == 5 ? 1u << 16 : vk == 6 ? 2u << 16 : 0;
        } else if (e->kind == E_WHEEL || e->kind == E_HWHEEL) { m->b = e->kind == E_WHEEL ? 0x20A : 0x20E; m->c = (uint32_t)e->val << 16; }
        else m->b = 0x200;
    }
}
static int hookable(ev *e) {
    if (e->kind == E_KEY) return e->vk != 0;
    if (e->kind == E_BTN) return btn_vk(e->code) != 0;
    return e->kind == E_WHEEL || e->kind == E_HWHEEL;
}
/* next hook after `seq` in LIFO installation order */
static cli *next_hook(int kind, int seq) {
    cli *b = NULL;
    for (int i = 0; i < MAXCLI; i++) if (C[i].fd >= 0 && C[i].hook[kind] && (seq < 0 || C[i].hook_seq[kind] < seq) && (!b || C[i].hook_seq[kind] > b->hook_seq[kind])) b = &C[i];
    return b;
}
static int hotkey(ev *e) {
    uint32_t m = (state[0x12] ? 1 : 0) | (state[0x11] ? 2 : 0) | (state[0x10] ? 4 : 0) | (state[0x5B] || state[0x5C] ? 8 : 0);
    for (int i = 0; i < MAXHK; i++) if (HK[i].used && HK[i].vk == e->vk && (HK[i].mods & 15) == m) {
        cli *c = by_gen(HK[i].gen); if (!c) continue;
        if (!(state[e->vk] && (HK[i].mods & 0x4000))) csend(c, (ahkb_msg){D_HOTKEY, 0, HK[i].cookie, m, e->vk});
        return 1;
    }
    return 0;
}
static void deliver(ev *e) { /* event survived every hook */
    dev *d = e->dev >= 0 ? &D[e->dev] : NULL;
    int pass = e->inj || (d && d->fd >= 0 && d->grabbed);
    switch (e->kind) {
    case E_KEY: {
        int hk = e->vk && e->val && hotkey(e);
        if (e->vk) { if (e->val || state[e->vk]) set_state(e->vk, !!e->val); }
        if (!pass) return; /* not grabbed: the compositor already has it */
        lay_typed = now_ms();
        if (e->val && !hk) { if (!out_down[e->code]) { out_down[e->code] = 1; emit(okbd, EV_KEY, e->code, 1); syn(okbd); } }
        else if (!e->val && out_down[e->code]) { out_down[e->code] = 0; emit(okbd, EV_KEY, e->code, 0); syn(okbd); }
        break; }
    case E_BTN:
        if (btn_vk(e->code)) set_state(btn_vk(e->code), e->val);
        if (pass && out_down[e->code] != e->val) { out_down[e->code] = e->val; emit(omouse, EV_KEY, e->code, e->val); syn(omouse); }
        break;
    case E_WHEEL: case E_HWHEEL:
        if (pass) {
            int h = e->kind == E_HWHEEL, fd = d && d->clone >= 0 ? d->clone : omouse;
            emit(fd, EV_REL, h ? REL_HWHEEL_HI_RES : REL_WHEEL_HI_RES, e->val);
            if (e->val / 120) emit(fd, EV_REL, h ? REL_HWHEEL : REL_WHEEL, e->val / 120);
            syn(fd);
        }
        break;
    case E_MOTION: case E_REL: {
        int fd = e->kind == E_REL ? omouse : d ? d->clone : -1;
        if (!pass || fd < 0) break;
        if (e->x) emit(fd, EV_REL, REL_X, e->x);
        if (e->y) emit(fd, EV_REL, REL_Y, e->y);
        syn(fd); break; }
    case E_UNI: {
        int i = e->code ? -1 : lay_find(e->x, e->vk), mods = i >= 0 ? L[e->x].e[i].mods : 0, code = i >= 0 ? L[e->x].e[i].code : e->code;
        int sh = (mods & 1) && !out_down[KEY_LEFTSHIFT] && !out_down[KEY_RIGHTSHIFT], gr = (mods & 2) && !out_down[KEY_RIGHTALT];
        if (!e->code && lay_real != e->x) break; /* the switch failed: typing it would give a wrong character */
        if (out_down[code]) { out_down[code] = 0; emit(okbd, EV_KEY, code, 0); syn(okbd); }
        if (sh) { emit(okbd, EV_KEY, KEY_LEFTSHIFT, 1); syn(okbd); }
        if (gr) { emit(okbd, EV_KEY, KEY_RIGHTALT, 1); syn(okbd); }
        emit(okbd, EV_KEY, code, 1); syn(okbd); emit(okbd, EV_KEY, code, 0); syn(okbd);
        if (gr) { emit(okbd, EV_KEY, KEY_RIGHTALT, 0); syn(okbd); }
        if (sh) { emit(okbd, EV_KEY, KEY_LEFTSHIFT, 0); syn(okbd); }
        lay_typed = now_ms(); break; }
    case E_ABS: emit(oabs, EV_ABS, ABS_X, e->x); emit(oabs, EV_ABS, ABS_Y, e->y); syn(oabs); break;
    }
}
static int motion_blocked(ev *e) {
    static int64_t last; int block = 0, note = now_ms() - last >= 16;
    for (int i = 0; i < MAXCLI; i++) if (C[i].fd >= 0 && C[i].hook[1]) {
        if (note) { ahkb_msg m; hook_msg(e, 0, &m); csend(&C[i], m); }
        block |= C[i].blockmove;
    }
    if (note) last = now_ms();
    return block;
}
/* evdev readers (the compositor) have a ~64-event kernel buffer and silently lose events when a burst
 * overflows it, so injected events are metered: a small burst, then 1 per millisecond. */
#define BURST 20
static int tokens = BURST; static int64_t tok_at, paced;
static int pace(void) {
    int64_t n = now_ms();
    if (n > tok_at) { int64_t k = tokens + (n - tok_at); tokens = k > BURST ? BURST : (int)k; tok_at = n; }
    if (tokens > 0) { tokens--; return 1; }
    return 0;
}
static void pump(void) {
    paced = 0;
    for (;;) {
        task *t = stack;
        if (!t) { t = qh; if (!t) return; qh = t->next; if (!qh) qt = &qh; t->next = NULL; stack = t; }
        if (t->waiting) return;
        if (t->i >= t->n) {
            stack = t->next;
            cli *c = t->rgen ? by_gen(t->rgen) : NULL;
            if (c) csend(c, (ahkb_msg){D_DONE, t->rid, t->n});
            free(t->e); free(t); continue;
        }
        ev *e = &t->e[t->i];
        if (e->kind == E_MOTION && motion_blocked(e)) e->suppress = 1;
        if (e->kind == E_KEY && t->stage_seq < 0) {
            /* numpad VKs depend on NumLock at the time the event is processed */
            int nav = !e->inj ? kp_nav(e->code) : 0;
            if (nav && !toggled[0x90]) e->vk = nav;
        }
        dev *d = e->dev >= 0 ? &D[e->dev] : NULL;
        int routed = e->inj || (d && d->grabbed);
        if (!e->suppress && routed && hookable(e)) {
            int kind = e->kind != E_KEY; cli *c = next_hook(kind, t->stage_seq);
            if (c) {
                t->stage_seq = c->hook_seq[kind]; t->waiting = 1; t->hooked = 1; t->wgen = c->gen; t->deadline = now_ms() + HOOK_TIMEOUT_MS;
                if (!++callno) callno = 1;
                t->call = callno; ahkb_msg m; hook_msg(e, t->call, &m); csend(c, m);
                return;
            }
        }
        if (!e->suppress) { int g = lay_need(e, routed); if (g >= 0 && !lay_goto(g)) { paced = 1; return; } }
        if (!e->suppress && e->inj && !pace()) { paced = 1; return; }
        if (!e->suppress && e->kind == E_UNI) tokens -= 3; /* one event, up to six key changes */
        if (!e->suppress) deliver(e);
        if (t->hooked) { t->hooked = 0; for (int i = 0; i < MAXCLI; i++) if (C[i].fd >= 0 && (C[i].hook[0] || C[i].hook[1])) csend(&C[i], (ahkb_msg){.type = D_SETTLED}); }
        t->i++; t->stage_seq = -1;
    }
}
static void verdict(int gen, uint32_t call, int sup) {
    for (task *t = stack; t; t = t->next) if (t->waiting && t->wgen == gen && (!call || t->call == call)) {
        t->waiting = 0; if (sup) t->e[t->i].suppress = 1; if (call) break;
    }
}

/* ---- native windows, as reported by the KWin script ---- */
#define BUSNAME "io.github.ahklinux.Bridge"
#define SCRIPT  "ahk-linux-bridge"
extern const char KWIN_JS[];
__asm__(".pushsection .rodata\n.globl KWIN_JS\nKWIN_JS:\n.incbin \"daemon/kwin.js\"\n.byte 0\n.popsection\n"); /* path from the repo root */
#define MAXWIN 256
static struct win { int slot, own, pid, min, g[8]; char uid[64], title[512], class[128], exe[512]; } W[MAXWIN];
static int nslot, active_slot, desk_w, desk_h, win_ok;
static char opname[64]; static unsigned opno;

static void win_send(cli *c, struct win *w, int alive) {
    char b[sizeof(ahkb_msg) + sizeof w->g + sizeof w->title + sizeof w->class + sizeof w->exe]; size_t n = sizeof(ahkb_msg);
    if (w->own) return;
    ahkb_msg m = {.type = D_WIN, .id = w->slot, .a = alive, .b = w->pid, .d = w->min};
    if (alive) {
        memcpy(b + n, w->g, sizeof w->g); n += sizeof w->g;
        const char *s[] = {w->title, w->class, w->exe};
        for (int i = 0; i < 3; i++) { size_t l = strlen(s[i]) + 1; memcpy(b + n, s[i], l); n += l; }
    }
    m.c = n - sizeof m; memcpy(b, &m, sizeof m);
    if (send(c->fd, b, n, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)n) shutdown(c->fd, SHUT_RDWR);
}
static void win_bcast(struct win *w, int alive) { for (int i = 0; i < MAXCLI; i++) if (C[i].fd >= 0 && C[i].authed) win_send(&C[i], w, alive); }
static void active_send(cli *c) { if (win_ok) csend(c, (ahkb_msg){.type = D_ACTIVE, .id = active_slot, .a = desk_w, .b = desk_h}); }
static void win_snapshot(cli *c) { for (int i = 0; i < MAXWIN; i++) if (W[i].slot) win_send(c, &W[i], 1); active_send(c); }
static struct win *win_find(const char *uid) { for (int i = 0; i < MAXWIN; i++) if (W[i].slot && !strcmp(W[i].uid, uid)) return &W[i]; return NULL; }
/* Windows programs under Wine are named by their .exe, everything else by its binary. */
static void exe_of(int pid, char *o, size_t n) {
    char p[64], b[512] = ""; ssize_t r;
    snprintf(p, sizeof p, "/proc/%d/cmdline", pid);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) { r = read(fd, b, sizeof b - 1); close(fd); if (r > 0) b[r] = 0; }
    size_t l = strlen(b);
    if (l < 4 || strcasecmp(b + l - 4, ".exe")) {
        snprintf(p, sizeof p, "/proc/%d/exe", pid);
        r = readlink(p, b, sizeof b - 1); b[r > 0 ? r : 0] = 0;
    }
    snprintf(o, n, "%s", b);
    for (char *c = o; *c; c++) if (*c == '/') *c = '\\';
}
/* split a D-Bus string on 0x1f; KWin sends every field as text */
static int fields(sd_bus_message *m, char **f, int max, char *buf, size_t cap) {
    const char *s; int n = 0;
    if (sd_bus_message_read(m, "s", &s) < 0 || strlen(s) >= cap) return 0;
    strcpy(buf, s);
    for (char *p = buf; n < max; ) { f[n++] = p; p = strchr(p, 0x1f); if (!p) break; *p++ = 0; }
    return n;
}
static void put_utf8(char *d, size_t n, const char *s) { /* truncate on a character boundary */
    size_t l = strlen(s); if (l >= n) { l = n - 1; while (l && (s[l] & 0xC0) == 0x80) l--; } memcpy(d, s, l); d[l] = 0;
}
static int on_window(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e; static char buf[8192]; char *f[13]; struct win *w;
    if (fields(m, f, 13, buf, sizeof buf) == 13) {
        if (!(w = win_find(f[0]))) {
            for (int i = 0; i < MAXWIN && !w; i++) if (!W[i].slot) w = &W[i];
            if (!w) return sd_bus_reply_method_return(m, "");
            memset(w, 0, sizeof *w); w->slot = ++nslot; put_utf8(w->uid, sizeof w->uid, f[0]);
        }
        int pid = atoi(f[3]);
        if (pid != w->pid) {
            w->pid = pid; exe_of(pid, w->exe, sizeof w->exe);
            const char *b = strrchr(w->exe, '\\'); b = b ? b + 1 : w->exe;
            w->own = !strncasecmp(b, "AutoHotkey", 10) && strlen(b) > 4 && !strcasecmp(b + strlen(b) - 4, ".exe");
        }
        put_utf8(w->title, sizeof w->title, f[1]); put_utf8(w->class, sizeof w->class, f[2]);
        w->min = atoi(f[4]); for (int i = 0; i < 8; i++) w->g[i] = atoi(f[5 + i]);
        win_bcast(w, 1);
    }
    return sd_bus_reply_method_return(m, "");
}
static void set_active(int slot) { active_slot = slot; for (int i = 0; i < MAXCLI; i++) if (C[i].fd >= 0 && C[i].authed) active_send(&C[i]); }
static int on_active(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e; static char buf[512]; char *f[3];
    if (fields(m, f, 3, buf, sizeof buf) == 3) {
        struct win *w = *f[0] ? win_find(f[0]) : NULL;
        win_ok = 1; desk_w = atoi(f[1]); desk_h = atoi(f[2]);
        set_active(!w ? 0 : w->own ? -1 : w->slot);
        lg("active window: %s [%s] %s", w ? w->title : "(none)", w ? w->class : "", w ? w->exe : "");
    }
    return sd_bus_reply_method_return(m, "");
}
static int on_gone(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)u; (void)e; static char buf[512]; char *f[1];
    struct win *w = fields(m, f, 1, buf, sizeof buf) == 1 ? win_find(f[0]) : NULL;
    if (w) { win_bcast(w, 0); if (w->slot == active_slot) set_active(0); w->slot = 0; }
    return sd_bus_reply_method_return(m, "");
}
static const sd_bus_vtable vt[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Window", "s", "", on_window, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Active", "s", "", on_active, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Gone", "s", "", on_gone, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END
};
static int on_loaded(sd_bus_message *m, void *u, sd_bus_error *e) {
    (void)e; (void)u; int id; char p[64];
    if (sd_bus_message_is_method_error(m, NULL) || sd_bus_message_read(m, "i", &id) < 0 || id < 0) { lg("KWin script not loaded; native windows unavailable"); return 0; }
    snprintf(p, sizeof p, "/Scripting/Script%d", id);
    sd_bus_call_method_async(bus, NULL, "org.kde.KWin", p, "org.kde.kwin.Script", "run", NULL, NULL, "");
    return 0;
}
static void unload(const char *name) { sd_bus_call_method_async(bus, NULL, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "unloadScript", NULL, NULL, "s", name); }
/* run() evaluates asynchronously, so a script stays loaded until the next one of the same name or exit */
static void script_load(const char *name, const char *file, const char *text) {
    FILE *f = fopen(file, "w"); char p[4096 + 64], cwd[4096];
    if (!f || !getcwd(cwd, sizeof cwd)) { if (f) fclose(f); return; }
    fputs(text, f); fclose(f);
    snprintf(p, sizeof p, "%s/%s", cwd, file);
    unload(name);
    sd_bus_call_method_async(bus, NULL, "org.kde.KWin", "/Scripting", "org.kde.kwin.Scripting", "loadScript", on_loaded, NULL, "ss", p, name);
}
static int on_kwin(sd_bus_message *m, void *u, sd_bus_error *e) { /* KWin (re)started */
    (void)u; (void)e; const char *n, *o, *w;
    if (sd_bus_message_read(m, "sss", &n, &o, &w) < 0) return 0;
    for (int i = 0; i < MAXWIN; i++) if (W[i].slot) { win_bcast(&W[i], 0); W[i].slot = 0; }
    if (win_ok) set_active(0);
    if (*w) { script_load(SCRIPT, "kwin.js", KWIN_JS); lay_query(); }
    return 0;
}
static void bus_open(void) {
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") || sd_bus_open_user(&bus) < 0) { bus = NULL; return; }
    if (sd_bus_add_object_vtable(bus, NULL, "/", BUSNAME, vt, NULL) < 0 || sd_bus_request_name(bus, BUSNAME, 0) < 0) {
        lg("bus name unavailable; native windows disabled"); bus = sd_bus_flush_close_unref(bus); return;
    }
    sd_bus_add_match_async(bus, NULL, "type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged',arg0='org.kde.KWin'", on_kwin, NULL, NULL);
    script_load(SCRIPT, "kwin.js", KWIN_JS);
    sd_bus_add_match_async(bus, NULL, "type='signal',path='/Layouts',interface='org.kde.KeyboardLayouts',member='layoutChanged'", on_layout, NULL, NULL);
    sd_bus_add_match_async(bus, NULL, "type='signal',path='/Layouts',interface='org.kde.KeyboardLayouts',member='layoutListChanged'", on_laylist, NULL, NULL);
    ep_add(sd_bus_get_fd(bus), T_BUS);
}
static void bus_close(void) {
    if (!bus) return;
    unload(SCRIPT);
    if (lay_real != lay_cur) sd_bus_call_method_async(bus, NULL, "org.kde.keyboard", "/Layouts", "org.kde.KeyboardLayouts", "setLayout", NULL, NULL, "u", lay_cur);
    if (*opname) unload(opname);
    sd_bus_flush_close_unref(bus);
}
static int winop(int op, int slot) {
    static const char *act[] = {"", "w.minimized=false;workspace.activeWindow=w", "w.minimized=true", "w.setMaximize(true,true)", "w.minimized=false;w.setMaximize(false,false)"};
    char js[512];
    if (!bus || op < 1 || op > 4) return 0;
    for (int i = 0; i < MAXWIN; i++) if (W[i].slot == slot && slot) {
        for (char *c = W[i].uid; *c; c++) if (!strchr("0123456789abcdefABCDEF-{}", *c)) return 0;
        snprintf(js, sizeof js, "for(const w of workspace.windowList())if(String(w.internalId)===\"%s\"){%s}\n", W[i].uid, act[op]);
        /* KWin finishes unloading late and refuses a name still in use, so each operation gets its own */
        char old[64]; snprintf(old, sizeof old, "%s", opname);
        snprintf(opname, sizeof opname, SCRIPT "-op%u", ++opno);
        script_load(opname, "op.js", js);
        if (*old) unload(old);
        return 1;
    }
    return 0;
}

/* ---- physical input ---- */
static void dev_read(int i) {
    dev *d = &D[i]; struct input_event b[64]; ssize_t r;
    while (d->fd >= 0 && (r = read(d->fd, b, sizeof b)) > 0) for (size_t k = 0; k < r / sizeof *b; k++) {
        struct input_event *e = &b[k];
        if (e->type == EV_KEY && e->code < KEY_CNT) {
            raw_down[e->code] = !!e->value;
            if (raw_down[KEY_LEFTCTRL] && raw_down[KEY_RIGHTCTRL] && raw_down[KEY_ESC]) { fprintf(stderr, "ahkd: panic chord, exiting\n"); _exit(3); }
            if (e->code >= BTN_MOUSE && e->code <= BTN_TASK) { if (e->value != 2) q1((ev){.kind = E_BTN, .dev = i, .code = e->code, .val = e->value}); }
            else q1((ev){.kind = E_KEY, .dev = i, .code = e->code, .val = e->value, .vk = K[e->code].vk, .sc = K[e->code].sc, .ext = K[e->code].ext});
        } else if (e->type == EV_REL) switch (e->code) {
            case REL_X: d->rx += e->value; break; case REL_Y: d->ry += e->value; break;
            case REL_WHEEL: d->rw += e->value; break; case REL_HWHEEL: d->rhw += e->value; break;
        } else if (e->type == EV_LED && e->code <= LED_SCROLLL && !d->grabbed) {
            int vk = e->code == LED_NUML ? 0x90 : e->code == LED_CAPSL ? 0x14 : 0x91; toggled[vk] = !!e->value; bcast_state(vk, 0);
        } else if (e->type == EV_SYN && e->code == SYN_REPORT) {
            if (d->rx || d->ry) q1((ev){.kind = E_MOTION, .dev = i, .x = d->rx, .y = d->ry});
            if (d->rw) q1((ev){.kind = E_WHEEL, .dev = i, .val = d->rw * 120});
            if (d->rhw) q1((ev){.kind = E_HWHEEL, .dev = i, .val = d->rhw * 120});
            d->rx = d->ry = d->rw = d->rhw = 0;
        }
    }
    if (d->fd >= 0 && r < 0 && errno != EAGAIN) dev_close(d);
}
static void led_read(void) {
    struct input_event e;
    while (read(okbd, &e, sizeof e) == sizeof e) if (e.type == EV_LED && e.code <= LED_SCROLLL) {
        int vk = e.code == LED_NUML ? 0x90 : e.code == LED_CAPSL ? 0x14 : 0x91;
        toggled[vk] = !!e.value; bcast_state(vk, 0);
        for (int i = 0; i < MAXDEV; i++) if (D[i].fd >= 0 && D[i].kbd && D[i].grabbed) { emit(D[i].fd, EV_LED, e.code, e.value); syn(D[i].fd); }
    }
}

/* ---- clients ---- */
static void inject(task *t, ahkb_msg *m) {
    ev e = {.inj = 1, .dev = -1, .extra = m->extra};
    if (m->type == C_KEY) {
        int up = m->c & 2, ext = m->c & 1;
        if (m->c & 4) {
            if (up) return;
            e.kind = E_UNI; e.vk = m->b & 0xffff; e.code = e.vk == '\r' || e.vk == '\n' ? KEY_ENTER : e.vk == '\t' ? KEY_TAB : 0;
            if (!nlay && !e.code) { fprintf(stderr, "ahkd: cannot type U+%04X\n", e.vk); return; }
            tpush(t, e); return;
        }
        int code = vk_to_code((m->c & 8) ? 0 : m->a, m->b, ext);
        e.kind = E_KEY; e.code = code; e.val = !up; e.vk = m->a; e.sc = m->b & 0xff; e.ext = ext;
        if (code && (m->c & 8)) e.vk = K[code].vk;
        if (e.vk == 0x10 || e.vk == 0x11 || e.vk == 0x12) e.vk = K[code].vk;
        tpush(t, e); return;
    }
    uint32_t f = m->c; int dx = (int)m->a, dy = (int)m->b, dd = (int)m->d;
    if (f & 1) { e.kind = f & 0x8000 ? E_ABS : E_REL; e.x = dx; e.y = dy; tpush(t, e); }
    static const struct { uint32_t f; int c, v; } bt[] = {{2, BTN_LEFT, 1}, {4, BTN_LEFT, 0}, {8, BTN_RIGHT, 1}, {0x10, BTN_RIGHT, 0}, {0x20, BTN_MIDDLE, 1}, {0x40, BTN_MIDDLE, 0}};
    for (int i = 0; i < 6; i++) if (f & bt[i].f) { e.kind = E_BTN; e.code = bt[i].c; e.val = bt[i].v; tpush(t, e); }
    if (f & 0x180) { e.kind = E_BTN; e.code = dd & 2 ? BTN_EXTRA : BTN_SIDE; e.val = !!(f & 0x80); tpush(t, e); }
    if (f & 0x800) { e.kind = E_WHEEL; e.val = dd; tpush(t, e); }
    if (f & 0x1000) { e.kind = E_HWHEEL; e.val = dd; tpush(t, e); }
}
static void handle(cli *c, ahkb_msg *m) {
    if (c->batch_left) {
        inject(c->batch, m);
        if (--c->batch_left == 0) {
            task *t = c->batch; c->batch = NULL;
            /* A hook calling SendInput from inside its callback must complete before its own verdict. */
            if (t->stage_seq == -2) { t->stage_seq = -1; t->next = stack; stack = t; } else tq(t);
        }
        return;
    }
    switch (m->type) {
    case C_HOOK: c->hook[m->a & 1] = !!m->b; c->hook_seq[m->a & 1] = ++seqno; if (!m->b) { c->blockmove = 0; } regrab(); csend(c, (ahkb_msg){D_DONE, m->id, 1}); break;
    case C_BATCH:
        if (!m->a || m->a > 100000) { csend(c, (ahkb_msg){D_DONE, m->id, 0}); break; }
        c->batch = tnew(); c->batch->rgen = c->gen; c->batch->rid = m->id; c->batch_left = m->a;
        if (m->b) { for (task *t = stack; t; t = t->next) if (t->waiting && t->wgen == c->gen) { c->batch->stage_seq = -2; break; } }
        break;
    case C_VERDICT: if (!m->id) c->blockmove = !!m->a; else verdict(c->gen, m->id, m->a); break;
    case C_HKREG: {
        int ok = 1, f = -1;
        for (int i = 0; i < MAXHK; i++) { if (!HK[i].used) { if (f < 0) f = i; } else if (HK[i].vk == m->c && (HK[i].mods & 15) == (m->b & 15)) ok = 0; }
        if (f < 0) ok = 0;
        if (ok) { HK[f].used = 1; HK[f].gen = c->gen; HK[f].cookie = m->a; HK[f].mods = m->b; HK[f].vk = m->c; regrab(); }
        csend(c, (ahkb_msg){D_DONE, m->id, ok}); break; }
    case C_HKUNREG: {
        int ok = 0;
        for (int i = 0; i < MAXHK; i++) if (HK[i].used && HK[i].gen == c->gen && HK[i].cookie == m->a) { HK[i].used = 0; ok = 1; }
        regrab(); csend(c, (ahkb_msg){D_DONE, m->id, ok}); break; }
    case C_WINOP: csend(c, (ahkb_msg){D_DONE, m->id, winop(m->a, m->b)}); break;
    default: lg("bad message %u", m->type); shutdown(c->fd, SHUT_RDWR);
    }
}
static void drop_client(cli *c) {
    lg("client gone");
    close(c->fd); c->fd = -1;
    for (int i = 0; i < MAXHK; i++) if (HK[i].used && HK[i].gen == c->gen) HK[i].used = 0;
    verdict(c->gen, 0, 0);
    if (c->batch) { free(c->batch->e); free(c->batch); c->batch = NULL; }
    regrab();
}
static void cli_read(cli *c) {
    ssize_t r = recv(c->fd, c->rx + c->n, sizeof c->rx - c->n, MSG_DONTWAIT);
    if (r <= 0) { if (r < 0 && errno == EAGAIN) return; drop_client(c); return; }
    c->n += r;
    size_t o = 0;
    if (!c->authed) {
        if (c->n < AHKB_TOKEN_LEN) return;
        uint8_t x = 0; for (int i = 0; i < AHKB_TOKEN_LEN; i++) x |= c->rx[i] ^ token[i];
        if (x) { drop_client(c); return; }
        c->authed = 1; o = AHKB_TOKEN_LEN;
        for (int v = 1; v < 256; v++) if (state[v] || toggled[v]) csend(c, (ahkb_msg){D_STATE, 0, v, (state[v] ? 1 : 0) | (toggled[v] ? 2 : 0)});
        lay_send(c, 0); lay_send(c, 1);
        win_snapshot(c);
        lg("client connected");
    }
    while (c->fd >= 0 && c->n - o >= sizeof(ahkb_msg)) { ahkb_msg m; memcpy(&m, c->rx + o, sizeof m); o += sizeof m; handle(c, &m); }
    memmove(c->rx, c->rx + o, c->n - o); c->n -= o;
}
/* If the main loop wedges for 3s, die: the kernel then drops every grab and the keyboard is usable again. */
static volatile sig_atomic_t quit;
static void on_term(int s) { (void)s; quit = 1; }
static void watchdog(int s) { (void)s; static int last; if (beat == last) { if (++missed >= 3) _exit(4); } else missed = 0; last = beat; }

int main(int argc, char **argv) {
    const char *dir = NULL;
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-v")) verbose = 1; else dir = argv[i]; }
    if (!dir) { fprintf(stderr, "usage: ahkd [-v] RUNTIME_DIR\n"); return 2; }
    only = getenv("AHKB_ONLY");
    umask(077); mkdir(dir, 0700);
    if (chdir(dir) < 0) die(dir);
    int lock = open("lock", O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB) < 0) { fprintf(stderr, "ahkd: already running\n"); return 0; }
    signal(SIGPIPE, SIG_IGN);
    ktab();
    for (int i = 0; i < MAXDEV; i++) D[i].fd = D[i].clone = -1;
    for (int i = 0; i < MAXCLI; i++) C[i].fd = -1;
    ep = epoll_create1(EPOLL_CLOEXEC);
    okbd = ui_open("AHK Bridge Keyboard", 0x4148, 0x4b01, 1, BUS_VIRTUAL, 0);
    omouse = ui_open("AHK Bridge Mouse", 0x4148, 0x4b02, 1, BUS_VIRTUAL, 1);
    oabs = ui_open("AHK Bridge Pointer", 0x4148, 0x4b03, 1, BUS_VIRTUAL, 2);
    ep_add(okbd, T_OKBD);
    ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC); inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_ATTRIB); ep_add(ino, T_INO);
    dev_scan();
    lsock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_in sa = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)}; socklen_t sl = sizeof sa;
    if (bind(lsock, (void *)&sa, sl) < 0 || listen(lsock, 8) < 0) die("listen");
    getsockname(lsock, (void *)&sa, &sl); ep_add(lsock, T_LISTEN);
    if (getrandom(token, sizeof token, 0) != sizeof token) die("getrandom");
    FILE *f = fopen("endpoint.tmp", "w"); if (!f) die("endpoint");
    fprintf(f, "%d ", ntohs(sa.sin_port)); for (int i = 0; i < AHKB_TOKEN_LEN; i++) fprintf(f, "%02x", token[i]);
    fprintf(f, " %d", AHKB_PROTO);
    fclose(f); rename("endpoint.tmp", "endpoint");
    bus_open(); lay_query();
    signal(SIGTERM, on_term); signal(SIGINT, on_term);
    signal(SIGALRM, watchdog); struct itimerval it = {{1, 0}, {1, 0}}; setitimer(ITIMER_REAL, &it, NULL);
    idle_since = now_ms() + 15000; /* generous first-connect window: Wine prefix creation is slow */
    for (;;) {
        int pend = regrab(), to = pend ? 50 : 500;
        if (stack && stack->waiting) { int64_t l = stack->deadline - now_ms(); to = l < 0 ? 0 : l < to ? l : to; }
        if (quit) { unlink("endpoint"); bus_close(); return 0; }
        if (bus) { int r; while ((r = sd_bus_process(bus, NULL)) > 0); if (r < 0) { lg("bus lost"); bus = sd_bus_close_unref(bus); } else sd_bus_flush(bus); }
        if (lay_real != lay_cur && !stack && !qh && (lay_wait || now_ms() - lay_typed > LAY_RESTORE_MS)) lay_goto(lay_cur);
        if (lay_real != lay_cur && to > 10) to = 10;
        if (bus) sd_bus_flush(bus);
        if (paced) to = 1;
        struct epoll_event e[32]; int n = epoll_wait(ep, e, 32, to);
        beat++;
        for (int i = 0; i < n; i++) {
            uint64_t t = e[i].data.u64;
            if (t == T_LISTEN) {
                int s = accept4(lsock, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK), one = 1, ok = 0;
                if (s < 0) continue;
                setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                for (int j = 0; j < MAXCLI && !ok; j++) if (C[j].fd < 0) { memset(&C[j], 0, sizeof C[j]); C[j].fd = s; C[j].gen = ++gens; ep_add(s, T_CLI + j); ok = 1; }
                if (!ok) close(s);
            } else if (t == T_INO) { char b[4096]; while (read(ino, b, sizeof b) > 0); dev_scan(); }
            else if (t == T_OKBD) led_read();
            else if (t >= T_CLI) { if (C[t - T_CLI].fd >= 0) cli_read(&C[t - T_CLI]); }
            else if (t >= T_DEV) { if (D[t - T_DEV].fd >= 0) dev_read(t - T_DEV); }
        }
        if (stack && stack->waiting && now_ms() >= stack->deadline) { lg("hook timeout"); stack->waiting = 0; }
        pump();
        int any = 0; for (int i = 0; i < MAXCLI; i++) any |= C[i].fd >= 0;
        if (any) idle_since = now_ms(); else if (now_ms() - idle_since > IDLE_EXIT_MS) { unlink("endpoint"); bus_close(); return 0; }
    }
}
