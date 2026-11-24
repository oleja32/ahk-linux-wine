#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Reports what is missing to build and run ahk-linux-wine. Changes nothing.
bad=0; warn=0
ok()   { printf '  ok    %s\n' "$1"; }
no()   { printf '  MISS  %s\n        -> %s\n' "$1" "$2"; bad=$((bad+1)); }
meh()  { printf '  opt   %s\n        -> %s\n' "$1" "$2"; warn=$((warn+1)); }
need() { command -v "$1" >/dev/null && ok "$1" || no "$1" "$2"; }
echo "Build and run"
for c in wine python3 unzip curl; do need $c "run ./setup.sh"; done
if [ ! -d prebuilt ]; then   # a release archive needs no compiler
for c in gcc make clang lld-link; do need $c "run ./setup.sh"; done
L="${WINE_LIB:-/usr/lib/wine/x86_64-windows}"; I="${WINE_INC:-/usr/include/wine}"
[ -f "$L/libuser32.a" ] && ok "Wine import libraries" || no "Wine import libraries in $L" "other distros: make WINE_LIB=<dir with libuser32.a>"
[ -f "$I/windows/windows.h" ] && ok "Wine headers" || no "Wine headers in $I" "other distros: make WINE_INC=<dir containing windows/windows.h>"
[ -f /usr/include/systemd/sd-bus.h ] && ok "libsystemd headers" || no "libsystemd headers" "Arch: systemd-libs; Debian/Ubuntu: libsystemd-dev"
[ -f /usr/include/xkbcommon/xkbcommon.h ] && ok "libxkbcommon headers" || no "libxkbcommon headers" "Arch: libxkbcommon; Debian/Ubuntu: libxkbcommon-dev"
fi
echo "Permissions"
[ -w /dev/uinput ] && ok "/dev/uinput writable" || no "/dev/uinput not writable" "run ./setup.sh"
r=0; for e in /dev/input/event*; do [ -r "$e" ] && r=1; done
[ $r = 1 ] && ok "/dev/input readable" || no "/dev/input not readable" "run ./setup.sh"
echo "Desktop"
case "${XDG_CURRENT_DESKTOP:-}" in *KDE*) ok "KDE Plasma (window matching works)";; *) meh "not KDE Plasma" "hotkeys and Send work; #IfWinActive on native windows does not";; esac
echo; [ $bad = 0 ] && echo "Ready" || echo "$bad required item(s) missing"
[ $bad = 0 ]
