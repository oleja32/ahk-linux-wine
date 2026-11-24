#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Removes what install.sh added under ~/.local. Your own scripts are not touched.
set -eu
H="$HOME/.local/share/ahk-bridge"; D="$HOME/.local/share"; L="$HOME/.local/bin/ahk"
[ -n "$(pgrep -u "$(id -u)" -x ahkd 2>/dev/null || true)" ] && { echo "Scripts are still running. Close them first:  ahk --stop"; exit 1; }
printf 'Remove ahk-linux-wine and its Wine folder (%s)? [y/N] ' "$H"; read -r a || a=n
[ "$a" = y ] || { echo "Nothing removed."; exit 0; }
[ "$(readlink "$L" 2>/dev/null)" != "$H/bin/ahk" ] || rm -f "$L"
rm -rf "$H"
rm -f "$D/applications/ahk-linux-wine.desktop" "$D/mime/packages/ahk-linux-wine.xml"
update-mime-database "$D/mime" 2>/dev/null || true
update-desktop-database "$D/applications" 2>/dev/null || true
echo "Removed."
for f in /etc/udev/rules.d/70-ahk-linux-wine.rules /etc/modules-load.d/ahk-linux-wine.conf; do
    [ ! -e "$f" ] || echo "To undo the permissions as well:  sudo rm $f"
done
