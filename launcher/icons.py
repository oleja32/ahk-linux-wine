#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Takes AutoHotkey's own icon out of its .exe:  icons.py AutoHotkeyU64.exe <icon theme folder> <icon name>"""
import os, struct, sys, zlib

ICON, GROUP, MAIN = 3, 14, 159  # resource types; AutoHotkey's main icon


def resources(d):
    pe = struct.unpack_from("<I", d, 60)[0]
    count, optsize = struct.unpack_from("<H", d, pe + 6)[0], struct.unpack_from("<H", d, pe + 20)[0]
    rva = struct.unpack_from("<I", d, pe + 24 + 112 + 16)[0]  # 64-bit optional header, data directory 2
    sections = [struct.unpack_from("<8sIIII", d, pe + 24 + optsize + 40 * i) for i in range(count)]
    def offset(r):
        return next(r - va + raw for _, vs, va, rs, raw in sections if va <= r < va + max(vs, rs))
    base = offset(rva)
    def entries(p):
        named, ids = struct.unpack_from("<HH", d, base + (p & 0x7FFFFFFF) + 12)
        return [struct.unpack_from("<II", d, base + (p & 0x7FFFFFFF) + 16 + 8 * i) for i in range(named + ids)]
    for kind, types in entries(0):
        if kind in (ICON, GROUP):
            for name, langs in entries(types):
                r, size = struct.unpack_from("<II", d, base + entries(langs)[0][1])
                yield (kind, name), d[offset(r):offset(r) + size]


def png(d):
    """A 32-bit icon bitmap (BGRA, bottom row first) as PNG."""
    head, w, h, _, bits, packed = struct.unpack_from("<IiiHHI", d)
    h //= 2  # the second half is the old 1-bit mask
    if bits != 32 or packed: return None
    rows = b""
    for y in range(h - 1, -1, -1):
        row = bytearray(d[head + y * w * 4:head + (y + 1) * w * 4])
        row[0::4], row[2::4] = row[2::4], row[0::4]
        rows += b"\0" + row
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b))
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


def main(exe, theme, name):
    with open(exe, "rb") as f:
        res = dict(resources(f.read()))
    group = res[GROUP, MAIN]
    for i in range(struct.unpack_from("<H", group, 4)[0]):
        e = struct.unpack_from("<BBBBHHIH", group, 6 + 14 * i)
        data = png(res[ICON, e[7]])
        if data:
            d = os.path.join(theme, f"{e[0]}x{e[0]}", "mimetypes")
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, name + ".png"), "wb") as f:
                f.write(data)


main(*sys.argv[1:])
