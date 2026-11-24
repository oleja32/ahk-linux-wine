#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Installs into ~/.local only. Re-run to upgrade.
set -eu
cd "$(dirname "$0")"
H="${AHKB_HOME:-$HOME/.local/share/ahk-bridge}"
V1=1.1.37.02; V1SUM=6f3663f7cdd25063c8c8728f5d9b07813ced8780522fd1f124ba539e2854215f
V2=2.0.32;    V2SUM=384a4c125e8d7b5b77ddbd5a593ec761d3e9e96e97a95931cf56e9a077fda114
# a release archive ships ready-made binaries; a git checkout builds them
B=prebuilt; [ -d $B ] || { make; B=build; }
mkdir -p "$H/bin" "$H/v1" "$H/v2" "$H/dl"
get() { # version sha256
  f="$H/dl/AutoHotkey_$1.zip"
  [ -f "$f" ] || curl -fL -o "$f" "https://github.com/AutoHotkey/AutoHotkey/releases/download/v$1/AutoHotkey_$1.zip"
  echo "$2  $f" | sha256sum -c --quiet
}
get $V1 $V1SUM; get $V2 $V2SUM
unzip -qo "$H/dl/AutoHotkey_$V1.zip" -d "$H/v1"
unzip -qo "$H/dl/AutoHotkey_$V2.zip" -d "$H/v2"
# install replaces the file, so a script that is running keeps its old copy
install -m644 $B/version.dll "$H/v1/version.dll"; install -m644 $B/version.dll "$H/v2/version.dll"
rm -rf "$H/inc"; cp -r "$H/v2/UX/inc" "$H/inc"
cp launcher/detect.ahk "$H/"
install -m755 $B/ahkd "$H/bin/ahkd"
install -m755 launcher/ahk "$H/bin/ahk"
echo "installed to $H"
[ -z "${AHKB_HOME:-}" ] || exit 0   # private copy (tests): leave the desktop alone

mkdir -p "$HOME/.local/bin"
[ -e "$HOME/.local/bin/ahk" ] || ln -s "$H/bin/ahk" "$HOME/.local/bin/ahk"
# double-click support
D="$HOME/.local/share"; mkdir -p "$D/applications" "$D/mime/packages"
rm -f "$D/applications/ahk-linux.desktop" "$D/mime/packages/ahk-linux.xml"   # the name before 0.1
cat > "$D/applications/ahk-linux-wine.desktop" <<X
[Desktop Entry]
Type=Application
Name=AutoHotkey (ahk-linux-wine)
Exec="$H/bin/ahk" %f
Icon=input-keyboard
NoDisplay=true
Terminal=false
MimeType=application/x-autohotkey;application/x-wine-extension-ahk;
X
cat > "$D/mime/packages/ahk-linux-wine.xml" <<X
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-autohotkey">
    <comment>AutoHotkey script</comment>
    <sub-class-of type="text/plain"/>
    <glob pattern="*.ahk" weight="60"/>
  </mime-type>
</mime-info>
X
update-mime-database "$D/mime" || true
update-desktop-database "$D/applications" 2>/dev/null || true
for m in application/x-autohotkey application/x-wine-extension-ahk; do xdg-mime default ahk-linux-wine.desktop $m 2>/dev/null || true; done
