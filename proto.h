/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Wire protocol between the in-process DLL (Windows side) and ahkd (Linux side). */
#ifndef AHKB_PROTO_H
#define AHKB_PROTO_H
typedef struct { unsigned int type, id, a, b, c, d; unsigned long long extra; } ahkb_msg; /* 32 bytes */
#define AHKB_TOKEN_LEN 32
enum { /* client -> daemon */
    C_HOOK = 1,     /* id; a=kind(0 kbd,1 mouse) b=on            -> D_DONE */
    C_BATCH,        /* id; a=count b=nested; count events follow -> D_DONE */
    C_KEY,          /* a=vk b=sc c=KEYEVENTF_* ; extra */
    C_MOUSE,        /* a=dx b=dy c=MOUSEEVENTF_* d=data ; extra */
    C_VERDICT,      /* id=call id; a=suppress */
    C_HKREG,        /* id; a=cookie b=mods c=vk                  -> D_DONE a=ok */
    C_HKUNREG,      /* id; a=cookie                              -> D_DONE */
    C_WINOP,        /* id; a=op b=slot                           -> D_DONE a=ok */
};
enum { /* daemon -> client */
    D_DONE = 101,   /* id; a=result */
    D_CALL,         /* id; a=kind b=wParam c=vk|sc<<16 or mouseData d=flags ; extra. id 0 = motion notice */
    D_STATE,        /* a=vk b=bit0 down, bit1 toggled, bit2 went-down */
    D_HOTKEY,       /* a=cookie b=mods c=vk */
    D_WIN = 109,        /* id=slot a=alive b=pid c=payload bytes d=minimized; payload: 8 ints (frame xywh, client xywh), then
                       title\0 class\0 exe path\0 in UTF-8 */
    D_ACTIVE = 110,      /* id=slot, 0 = no window, ~0 = one of AutoHotkey's own; a,b = desktop size in compositor units */
    D_LAYOUT = 112,     /* a=0 base layout (hotkey names), 1 active layout; b=active differs from base; c=payload bytes of ahkb_key */
    D_SETTLED = 111,      /* an event that went through hooks is fully processed */
};
typedef struct { unsigned int ch; unsigned short vk, mods; } ahkb_key; /* mods: 1 Shift, 2 AltGr; plainest key for a character first */
enum { W_ACTIVATE = 1, W_MINIMIZE, W_MAXIMIZE, W_RESTORE };
#endif
