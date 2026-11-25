# ahk-linux-wine

Run your `.ahk` scripts on Linux. Hotkeys, hotstrings, remaps, Send and Click work everywhere on the desktop, including
on Wayland and in games.

It runs the real, unmodified AutoHotkey (v1.1.37.02 and v2.0.32) under Wine and connects its keyboard, mouse and window
functions to Linux. Scripts behave as they do on Windows, with
[some limits](#what-does-not-work-yet).

**Tested on:** CachyOS (Arch-based) with KDE Plasma 6.8 on Wayland. Other distributions are untested; reports are welcome.
Limiting a hotkey to one window needs KDE Plasma.

## Requirements

- **Wine** (tested with 11.22)
- **Python** 3.10 or newer
- **unzip** and **curl**
- **KDE Plasma** for limiting hotkeys to a window and for typing characters from other keyboard layouts; hotkeys and Send
  should work on other desktops too
- *Optional:* **libnotify** (`notify-send`), for update notices

`./setup.sh` offers to install Wine, Python, unzip and curl if they are missing (Arch, Debian, Ubuntu, Fedora and openSUSE
based systems). AutoHotkey itself is downloaded by setup. Building from source also needs gcc, make, clang, lld and the headers
of systemd and libxkbcommon.

## Install

Nothing is installed without asking you first.

1. Download `ahk-linux-wine.tar.gz` from the [latest release](https://github.com/oleja32/ahk-linux-wine/releases/latest).
2. Right-click the file and choose *Extract here* (in KDE: *Extract > Extract archive here*).
3. Open the new `ahk-linux-wine` folder, right-click an empty spot and choose *Open Terminal Here*.
4. Start the guided setup. It first lists what it is going to do, then shows every command that needs your password and asks
   before running it:

       ./setup.sh

<details>
<summary>Or build it from source (Arch-based systems)</summary>

    sudo pacman -S --needed git
    git clone https://github.com/oleja32/ahk-linux-wine
    cd ahk-linux-wine
    ./setup.sh

</details>

## Your first script

1. Create a text file called `test.ahk` with this inside:

       #Requires AutoHotkey v2.0
       F8::Send("Hello from AutoHotkey")

2. Double-click it, or type `ahk test.ahk` in a terminal.
   A green **H** icon appears in the system tray.
3. Click into any text field and press **F8**.
4. To stop it, right-click the **H** icon and choose **Exit**.

## Everyday use

| To do this | Do this |
|---|---|
| Run a script | Double-click it, or `ahk script.ahk` |
| Pause, reload or exit a script | Right-click its tray icon |
| Close all scripts | `ahk --stop` |
| Update | `ahk --update` |
| **Get your keyboard back in an emergency** | Hold **Left Ctrl + Right Ctrl + Esc** |
| Force a version | `ahk --v1 script.ahk` or `ahk --v2 script.ahk` |

The version is chosen from the script's `#Requires` line, or detected. A very short script can fit both versions and is then
run as v2; to be sure, put `#Requires AutoHotkey v1.1` or `#Requires AutoHotkey v2.0` on the first line.

## Limit a hotkey to one program

1. Open the program, run the command below in a terminal, and switch to the program within 3 seconds:

       sleep 3; ahk --windows

   The active window is marked with `*`. For example, for the Kate editor:

       * notes.txt - Kate
           ahk_class org.kde.kate
           ahk_exe kate

2. Put the name into your script, above the hotkeys:

       #IfWinActive ahk_exe kate         ; v1
       F8::Send Hello

       #HotIf WinActive("ahk_exe kate")  ; v2
       F8::Send("Hello")

Scripts written for Windows use Windows names such as `Game.exe`. Replace them with the names you see here.

## Keyboard layouts

Scripts can type any character that is on one of the layouts in your system settings, whichever is active. For a character that
is not on the active one, ahk-linux-wine switches the layout for a moment, so you may see KDE's layout notice (KDE Plasma only).
Hotkey names such as `]` or `t` mean the key on your first Latin layout, so scripts work the same on all of them.

## If something goes wrong

| Problem | Try this |
|---|---|
| Something is missing or has no permission | Run `./setup.sh` again; `./check.sh` only shows the state |
| `ahk: command not found` | Log out and in, or use `~/.local/bin/ahk` |
| A warning about an unassigned variable | It is probably a v1 script: add `#Requires AutoHotkey v1.1` as the first line |
| A hotkey does nothing | Does the `#IfWinActive` name match `ahk --windows`? |
| Some characters are not typed | Add a keyboard layout that has them in the system settings |
| A key or the mouse seems stuck | Left Ctrl + Right Ctrl + Esc, then start the script again |
| Odd mouse behaviour with input-remapper | Stop its preset for that mouse; both programs want to control it |
| "input daemon failed to start" | Look at the log file it names |

Still stuck? Open an issue with the output of `ahk --version` and `./check.sh`, your distribution and desktop, and the shortest script that shows
the problem.

## Update

    ahk --update

Once a day, when you start a script, ahk looks for a new version and shows a notice with an **Update** button. Your scripts keep
running during an update; restart them to use the new version. If an update cannot be installed, the old version is put back.

The check asks GitHub for the newest version number and sends nothing about you. To turn it off: `ahk --no-update-check`
(on again: `ahk --update-check`).

Coming from 0.1? Download and extract the newest release once and run `./setup.sh` in it. From then on, `ahk --update` works.

## Uninstall

Run `./uninstall.sh` in the folder you installed from, or in a freshly downloaded one. Your scripts are not touched.

## What does not work yet

- Screen reading outside Wine: PixelGetColor, PixelSearch, ImageSearch.
- Control*, WinText, WinClose, WinMove and messages to native windows.
- Characters that are on none of your layouts, emoji, and accents that need a dead key.
- MouseGetPos while the pointer is over a native Wayland window (MouseMove and Click work).
- A menu does not notice a touchpad click outside it (Esc or switching windows closes it).
- Touchpads are not hooked (they keep working normally). 32-bit AutoHotkey builds. SendPlay. BlockInput On.
- DllCall, COM and the registry behave as Wine implements them.

These do work on native windows: `#IfWinActive`, `#IfWinExist`, titles, `ahk_exe`, `ahk_class`, `ahk_group`, RegEx mode,
WinGetTitle/Class/Pos and WinActivate. `ahk_class` is KDE's window class, not the Windows one.

## Security

Setup lets programs you run use your keyboard and mouse devices, so any of them can read what you type. This is how ordinary
Windows works too, but Linux on Wayland is normally stricter. `./uninstall.sh` tells you how to undo it.
Run only scripts you trust: a script can do anything you can.

## Games

Automation may be against a game's rules and can get an account banned. Check them first; you are responsible for how you use
scripts. ahk-linux-wine does not touch game files or memory, and makes no attempt to hide from anti-cheat software.

## How it works

    AutoHotkey.exe (stock, under Wine)
      └ version.dll   replaces its imports: hooks, SendInput, RegisterHotKey, key state, window queries, menus, layouts
          └ ahkd      evdev in -> hook chain -> uinput out;  KWin script -> window list;  layouts (xkbcommon)

`version.dll` sits beside the exe and is loaded as its VERSION.dll import, so Reload and child scripts work.
ahkd grabs keyboards and mice only while a script has a hook or hotkey, and only when no key is held.
Hooks get 300 ms to answer, as on Windows. The kernel releases all grabs if ahkd dies; a watchdog kills it if it hangs.
Clients must present a random token stored in a 0700 runtime directory. Everything is installed under `~/.local`.

## Licence

ahk-linux-wine is free software under the GNU General Public License, version 2 or (at your option) any later version; see
[LICENSE](LICENSE). It comes with no warranty.

ahk-linux-wine is not affiliated with or endorsed by the AutoHotkey project. [AutoHotkey](https://www.autohotkey.com) is by Chris
Mallett, Steve Gray and contributors, under the GPL v2. It is not included here: setup downloads the official, unmodified
releases from [its GitHub page](https://github.com/AutoHotkey/AutoHotkey/releases) and checks their checksums.
Wine is a separate program (LGPL), installed from your distribution.
