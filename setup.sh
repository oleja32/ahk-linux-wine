#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Guided setup: shows what it is going to do, then installs what is missing, sets permissions and installs ahk.
# Asks before it starts and before commands that need sudo. Run it again at any time; it skips what is already done.
set -u
cd "$(dirname "$0")" || exit 1
[ "$(id -u)" != 0 ] || { echo "Run this as your normal user, without sudo."; exit 1; }
[ "$(uname -m)" = x86_64 ] || { echo "Only 64-bit x86 computers are supported."; exit 1; }
B=; G=; N=
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then B=$'\e[1m'; G=$'\e[32m'; N=$'\e[0m'; fi
step() { printf '\n%s[%s/3] %s%s\n' "$B" "$1" "$2" "$N"; }
good() { printf '  %s%s%s\n' "$G" "$*" "$N"; }
ask() { # "why" "command" pairs: show all of them, then run them one by one if the user agrees
    local i n=$(($# / 2)) a all=("$@")
    echo "  These commands need your password:"
    for ((i = 0; i < n; i++)); do printf '\n    %s. %s\n       %s\n' $((i + 1)) "${all[2 * i]}" "${all[2 * i + 1]}"; done
    printf '\n'
    read -r -p "  Run them now? [y/N] " a || a=n
    case $a in [yY]|[yY][eE][sS]) ;; *) return 1;; esac
    for ((i = 0; i < n; i++)); do
        printf '  (%s/%s) %s\n' $((i + 1)) $n "${all[2 * i]}"
        bash -c "${all[2 * i + 1]}" || { echo "  That command failed: ${all[2 * i + 1]}"; return 1; }
    done
}

# look first, so that the plan below says what will really happen
H="$HOME/.local/share/ahk-bridge"
new=$(cut -d' ' -f1 VERSION); old=$(cut -d' ' -f1 "$H/VERSION" 2>/dev/null)
missing= p= build=
for c in wine python3 unzip curl; do command -v $c >/dev/null || missing="$missing $c"; done
if [ ! -d prebuilt ]; then build=1; for c in gcc make clang lld-link; do command -v $c >/dev/null || missing="$missing $c"; done; fi
if [ -n "$missing" ]; then
    id=$( . /etc/os-release 2>/dev/null; echo " ${ID:-} ${ID_LIKE:-} ")
    case "$id" in
    *" arch "*)                  p="pacman -S --needed wine python unzip curl${build:+ gcc make clang lld libxkbcommon}";;
    *" debian "*|*" ubuntu "*)   p="apt install wine python3 unzip curl libxkbcommon0";;
    *" fedora "*)                p="dnf install wine python3 unzip curl libxkbcommon";;
    *suse*)                      p="zypper install wine python3 unzip curl libxkbcommon0";;
    esac
    case "$build$id" in 1*" arch "*|" "*) ;; *) echo "Missing:$missing"; echo "Building from source is only supported on Arch-based systems; use the release archive instead."; exit 1;; esac
    [ -n "$p" ] || { echo "Missing:$missing"; echo "Unknown distribution: install these yourself, then run ./setup.sh again."; exit 1; }
fi
allowed() { [ -w /dev/uinput ] || return 1; for e in /dev/input/event*; do [ -r "$e" ] && return 0; done; return 1; }
get=
for v in $(sed -n 's/^V[12]=\([0-9.]*\);.*/\1/p' install.sh); do [ -f "$H/dl/AutoHotkey_$v.zip" ] || get="$get${get:+ and }$v"; done

printf '%sahk-linux-wine %s setup%s\n\nThis is what will happen:\n\n' "$B" "$new" "$N"
if [ -n "$missing" ]; then printf '  1. Programs      Install with your package manager:%s  (needs your password)\n' "$missing"
else printf '  1. Programs      Nothing to do, everything needed is there\n'; fi
if allowed; then printf '  2. Permissions   Nothing to do, already set\n'
else printf '  2. Permissions   Let your user use the keyboard and mouse devices  (needs your password)\n'; fi
if [ -n "$get" ]; then printf '  3. Install       Download AutoHotkey %s from its official GitHub page (about %s MB)\n' "$get" "$(case "$get" in *and*) echo 8;; 1*) echo 5;; *) echo 3;; esac)"
else printf '  3. Install       AutoHotkey is already downloaded\n'; fi
[ -z "$build" ] || printf '                   Build the program from source\n'
if [ -z "$old" ]; then printf '                   Put everything into %s\n' "~/.local/share/ahk-bridge"
elif [ "$old" = "$new" ]; then printf '                   Install version %s again into %s\n' "$new" "~/.local/share/ahk-bridge"
else printf '                   Replace version %s with %s in %s\n' "$old" "$new" "~/.local/share/ahk-bridge"; fi
[ -f "$H/prefix/system.reg" ] || printf '                   Prepare a Wine folder there (about 30 MB)\n'
printf '                   Add the ahk command, and open .ahk files with a double-click\n\n'
cat <<X
Your scripts and your other Wine programs are not changed. To remove it later, run ./uninstall.sh;
it also tells you how to undo the permissions.

X
read -r -p "Start? [Y/n] " a || a=n
case $a in ""|[yY]|[yY][eE][sS]) ;; *) echo "Nothing was changed."; exit 1;; esac

step 1 "Programs"
if [ -n "$missing" ]; then
    echo "  Missing:$missing"
    echo "  If the package manager asks you to choose between numbered options, just press Enter."
    ask "Install the missing programs" "sudo $p" || { echo "  Install them, then run ./setup.sh again."; exit 1; }
    hash -r
    for c in $missing; do command -v "$c" >/dev/null || { echo "  $c is still missing."; exit 1; }; done
    good "Installed."
else good "Everything needed is installed."; fi

step 2 "Permissions"
if allowed; then good "Your user can already use the keyboard and mouse devices."
else
    echo "  To see hotkeys and press keys, ahk needs access to your keyboard and mouse devices."
    echo "  Note: after this, any program you run can read your key presses, as on Windows."
    ask "Add a rule that gives you access (you can read it in that file)" "sudo install -m644 launcher/70-ahk-linux-wine.rules /etc/udev/rules.d/" \
        "Turn on virtual keyboards at every start" "echo uinput | sudo tee /etc/modules-load.d/ahk-linux-wine.conf >/dev/null" \
        "Turn them on now" "sudo modprobe uinput" \
        "Read the new rule" "sudo udevadm control --reload" \
        "Use it for the devices already connected" "sudo udevadm trigger --subsystem-match=input --subsystem-match=misc --settle" || { echo "  Not finished. Run ./setup.sh again when you are ready."; exit 1; }
    sleep 1
    if allowed; then good "Permissions are set."; else echo "  Not active yet: restart the computer once before using ahk."; fi
fi

step 3 "Install"
./install.sh || { echo "Installation failed, see the message above. You can run ./setup.sh again."; exit 1; }
printf '\n%sDone, ahk-linux-wine %s is ready.%s\n' "$B$G" "$new" "$N"
case "${XDG_CURRENT_DESKTOP:-}" in *KDE*) ;; *) echo "Note: limiting macros to one window needs KDE Plasma; hotkeys and Send should still work.";; esac
case ":$PATH:" in
*":$HOME/.local/bin:"*) ahk=ahk;;
*) ahk="$HOME/.local/bin/ahk"; echo "Note: $HOME/.local/bin is not in your PATH, so type the full path shown below, or log out and in.";;
esac
cat <<X

  Run a script         double-click a .ahk file, or type:  $ahk yourscript.ahk
  Close all scripts    $ahk --stop
  Update               $ahk --update
  Emergency release    Left Ctrl + Right Ctrl + Esc
X
