; SPDX-License-Identifier: GPL-2.0-or-later
#Requires AutoHotkey v2.0
#NoTrayIcon
#SingleInstance Off
#Include inc\identify.ahk
try {
    r := IdentifyBySyntax(FileRead(A_Args[1], "UTF-8"))
    FileAppend(r.v, A_Args[2])
}
