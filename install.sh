#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Installs into ~/.local only. Re-run to upgrade.
set -eu
cd "$(dirname "$0")"
H="${AHKB_HOME:-$HOME/.local/share/ahk-bridge}"
V1=1.1.37.02; V1SUM=6f3663f7cdd25063c8c8728f5d9b07813ced8780522fd1f124ba539e2854215f
V2=2.0.32;    V2SUM=384a4c125e8d7b5b77ddbd5a593ec761d3e9e96e97a95931cf56e9a077fda114
# a release archive ships ready-made binaries; a git checkout builds them
B=prebuilt; [ -d $B ] || { echo "  Building..."; make -s; B=build; }
mkdir -p "$H/bin" "$H/v1" "$H/v2" "$H/dl"
get() { # version sha256
  f="$H/dl/AutoHotkey_$1.zip"
  if [ -f "$f" ] && echo "$2  $f" | sha256sum -c --status; then echo "  AutoHotkey $1 is already downloaded"; return; fi
  echo "  Downloading AutoHotkey $1..."
  # a progress bar in a terminal, silent in a log; an unfinished download never gets the final name
  if [ -t 2 ]; then bar=--progress-bar; else bar=-sS; fi
  COLUMNS=60 curl -fL $bar -o "$f.part" "https://github.com/AutoHotkey/AutoHotkey/releases/download/v$1/AutoHotkey_$1.zip" ||
    { rm -f "$f.part"; printf '\n  The download failed. Check your internet connection.\n'; exit 1; }
  echo "$2  $f.part" | sha256sum -c --status || { rm -f "$f.part"; echo "  The download is damaged (wrong checksum)."; exit 1; }
  mv "$f.part" "$f"
}
get $V1 $V1SUM; get $V2 $V2SUM
unpack() { # folder version: only when it changed, a running script has these files open
  [ "$(cat "$H/$1/.version" 2>/dev/null)" = "$2" ] && [ -f "$H/$1/$3" ] && return
  echo "  Unpacking AutoHotkey $2..."
  unzip -qo "$H/dl/AutoHotkey_$2.zip" -d "$H/$1"
  echo "$2" > "$H/$1/.version"
}
unpack v1 $V1 AutoHotkeyU64.exe
unpack v2 $V2 AutoHotkey64.exe
echo "  Copying the program files..."
# install replaces the file, so a script that is running keeps its old copy
install -m644 $B/version.dll "$H/v1/version.dll"; install -m644 $B/version.dll "$H/v2/version.dll"
rm -rf "$H/inc"; cp -r "$H/v2/UX/inc" "$H/inc"
cp launcher/detect.ahk "$H/"
install -m755 $B/ahkd "$H/bin/ahkd"
install -m755 launcher/ahk "$H/bin/ahk"
install -m644 VERSION "$H/VERSION"
[ -z "${AHKB_HOME:-}" ] || exit 0   # private copy (tests): leave the desktop alone
if [ -f "$H/prefix/system.reg" ]; then echo "  Checking the Wine folder..."; else echo "  Preparing Wine (this can take a minute)..."; fi
e=$("$H/bin/ahk" --prepare 2>&1) || { echo "$e"; echo "  Note: this did not work; it will be tried again when you start a script"; }

echo "  Adding the ahk command, double-click support and the file icon..."
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
Icon=application-x-autohotkey
NoDisplay=true
Terminal=false
MimeType=application/x-autohotkey;application/x-wine-extension-ahk;
X
cat > "$D/mime/packages/ahk-linux-wine.xml" <<X
<?xml version="1.0" encoding="UTF-8"?>
<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">
  <mime-type type="application/x-autohotkey">
    <comment>AutoHotkey script</comment>
    <icon name="application-x-autohotkey"/>
    <sub-class-of type="text/plain"/>
    <glob pattern="*.ahk" weight="60"/>
  </mime-type>
</mime-info>
X
# AutoHotkey's own icon for .ahk files, taken from the download
rm -f "$D"/icons/hicolor/*/mimetypes/application-x-autohotkey.png
python3 launcher/icons.py "$H/v1/AutoHotkeyU64.exe" "$D/icons/hicolor" application-x-autohotkey || echo "note: could not set the .ahk file icon"
touch "$D/icons/hicolor" 2>/dev/null || true
update-mime-database "$D/mime" 2>/dev/null || true
update-desktop-database "$D/applications" 2>/dev/null || true
for m in application/x-autohotkey application/x-wine-extension-ahk; do xdg-mime default ahk-linux-wine.desktop $m 2>/dev/null || true; done
# let KDE programs that are already open notice the icon
dbus-send --session --type=signal /KIconLoader org.kde.KIconLoader.iconChanged int32:0 2>/dev/null || true
