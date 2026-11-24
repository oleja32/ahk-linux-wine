#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Guided setup: installs what is missing, sets permissions, installs ahk. Asks before every command that needs sudo.
# Run it again at any time; it skips what is already done.
set -u
cd "$(dirname "$0")" || exit 1
[ "$(id -u)" != 0 ] || { echo "Run this as your normal user, without sudo."; exit 1; }
[ "$(uname -m)" = x86_64 ] || { echo "Only 64-bit x86 computers are supported."; exit 1; }
say() { printf '\n== %s\n' "$*"; }
ask() { # show commands, run them one by one if the user agrees
    echo "These commands need your password:"; printf '    %s\n' "$@"
    read -r -p "Run them now? [y/N] " a || a=n
    case $a in y|Y|yes) ;; *) return 1;; esac
    for c in "$@"; do bash -c "$c" || { echo "That command failed: $c"; return 1; }; done
}

say "1/3 Programs"
missing=
for c in wine python3 unzip curl; do command -v $c >/dev/null || missing="$missing $c"; done
if [ ! -d prebuilt ]; then for c in gcc make clang lld-link; do command -v $c >/dev/null || missing="$missing $c"; done; fi
id=$( . /etc/os-release 2>/dev/null; echo " ${ID:-} ${ID_LIKE:-} ")
if [ -n "$missing" ]; then
    echo "Missing:$missing"
    build=; [ -d prebuilt ] || build=1
    case "$id" in
    *" arch "*)                  p="pacman -S --needed wine python unzip curl${build:+ gcc make clang lld libxkbcommon}";;
    *" debian "*|*" ubuntu "*)   p="apt install wine python3 unzip curl libxkbcommon0";;
    *" fedora "*)                p="dnf install wine python3 unzip curl libxkbcommon";;
    *suse*)                      p="zypper install wine python3 unzip curl libxkbcommon0";;
    *)                           p=;;
    esac
    case "$build$id" in 1*" arch "*|" "*) ;; *) echo "Building from source is only supported on Arch-based systems; use the release archive instead."; exit 1;; esac
    [ -n "$p" ] || { echo "Unknown distribution: install these yourself, then run ./setup.sh again."; exit 1; }
    echo "If the package manager asks you to choose between numbered options, just press Enter."
    ask "sudo $p" || { echo "Install them, then run ./setup.sh again."; exit 1; }
    hash -r
    for c in $missing; do command -v "$c" >/dev/null || { echo "$c is still missing."; exit 1; }; done
else echo "Everything needed is installed."; fi

say "2/3 Permissions"
allowed() { [ -w /dev/uinput ] || return 1; for e in /dev/input/event*; do [ -r "$e" ] && return 0; done; return 1; }
if allowed; then echo "Your user can already use the keyboard and mouse devices."
else
    echo "To see hotkeys and press keys, ahk needs access to your keyboard and mouse devices."
    echo "Note: after this, any program you run can read your key presses, as on Windows."
    ask "printf '%s\\n' 'KERNEL==\"uinput\", SUBSYSTEM==\"misc\", TAG+=\"uaccess\", OPTIONS+=\"static_node=uinput\"' 'KERNEL==\"event*\", SUBSYSTEM==\"input\", TAG+=\"uaccess\"' | sudo tee /etc/udev/rules.d/70-ahk-linux-wine.rules >/dev/null" \
        "echo uinput | sudo tee /etc/modules-load.d/ahk-linux-wine.conf >/dev/null" \
        "sudo modprobe uinput" \
        "sudo udevadm control --reload" \
        "sudo udevadm trigger --subsystem-match=input --subsystem-match=misc --settle" || { echo "Not changed. Run ./setup.sh again when you are ready."; exit 1; }
    sleep 1
    allowed || echo "Not active yet: restart the computer once before using ahk."
fi

say "3/3 Installing for your user (downloads AutoHotkey from its official GitHub releases)"
./install.sh || { echo "Installation failed, see the message above."; exit 1; }
case "${XDG_CURRENT_DESKTOP:-}" in *KDE*) ;; *) echo "Note: limiting macros to one window needs KDE Plasma; hotkeys and Send should still work.";; esac
case ":$PATH:" in
*":$HOME/.local/bin:"*) ahk=ahk;;
*) ahk="$HOME/.local/bin/ahk"; echo "Note: $HOME/.local/bin is not in your PATH, so type the full path shown below, or log out and in.";;
esac
cat <<X

Done. Double-click a .ahk file to run it, or type:  $ahk yourscript.ahk
Close everything:  $ahk --stop        Emergency release:  Left Ctrl + Right Ctrl + Esc
X
