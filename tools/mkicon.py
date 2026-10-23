#!/usr/bin/env python3
"""Write AmigaOS 2.x/3.x .info icons (classic DiskObject, 4 colours).

Images are text files, one character per pixel, using the Workbench 2/3
default palette:
    .  colour 0  grey (background, transparent on Workbench)
    k  colour 1  black
    w  colour 2  white
    b  colour 3  blue
An image file may hold two images separated by a line "--": normal and
selected. Without a selected image, the icon is highlighted by complement.

Usage:
  mkicon.py OUT.info TYPE IMAGE.txt [--tool T] [--tt TOOLTYPE ...]
            [--stack N] [--drawer X,Y,W,H] [--png PREVIEW.png]
TYPE: disk drawer tool project garbage device kick

Copyright (C) 2026  Torsten
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import struct

TYPES = {"disk": 1, "drawer": 2, "tool": 3, "project": 4, "garbage": 5,
         "device": 6, "kick": 7}
PEN = {".": 0, "k": 1, "w": 2, "b": 3}
PALETTE = [(0xAA, 0xAA, 0xAA), (0, 0, 0), (0xFF, 0xFF, 0xFF), (0x66, 0x88, 0xBB)]
NO_ICON_POSITION = 0x80000000


def load_images(path):
    rows, images = [], []
    for line in open(path).read().splitlines():
        if line.strip() == "--":
            images.append(rows)
            rows = []
        elif line.strip():
            rows.append(line.rstrip())
    images.append(rows)
    out = []
    for rows in images:
        w = max(len(r) for r in rows)
        for r in rows:
            for c in r:
                if c not in PEN:
                    raise SystemExit("%s: bad pixel %r" % (path, c))
        out.append([[PEN[c] for c in r.ljust(w, ".")] for r in rows])
    if len(out) == 2 and (len(out[0]) != len(out[1]) or len(out[0][0]) != len(out[1][0])):
        raise SystemExit("%s: normal and selected image differ in size" % path)
    return out


def image_struct(pix, depth=2):
    h, w = len(pix), len(pix[0])
    words = (w + 15) // 16
    data = b""
    for plane in range(depth):
        for row in pix:
            bits = 0
            for x in range(words * 16):
                bits <<= 1
                if x < w and (row[x] >> plane) & 1:
                    bits |= 1
            data += bits.to_bytes(words * 2, "big")
    # LeftEdge TopEdge Width Height Depth ImageData PlanePick PlaneOnOff NextImage
    head = struct.pack(">hhhhhIBBI", 0, 0, w, h, depth, 1, (1 << depth) - 1, 0, 0)
    return head + data


class BitWriter:
    def __init__(self):
        self.bits = []

    def put(self, value, n):
        self.bits += [(value >> (n - 1 - i)) & 1 for i in range(n)]

    def data(self):
        b = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int("".join(map(str, b[i:i + 8])), 2) for i in range(0, len(b), 8))


def rle(values, depth):
    """GlowIcon RLE: ByteRun1 control bytes (8 bit) in a bit stream of
    depth-bit values."""
    w = BitWriter()
    i, n = 0, len(values)
    while i < n:
        run = 1
        while i + run < n and run < 128 and values[i + run] == values[i]:
            run += 1
        if run >= 2:
            w.put(257 - run, 8)
            w.put(values[i], depth)
            i += run
            continue
        j = i + 1                       # literal up to the next run of 2
        while j < n and j - i < 128 and not (j + 1 < n and values[j] == values[j + 1]):
            j += 1
        w.put(j - i - 1, 8)
        for v in values[i:j]:
            w.put(v, depth)
        i = j
    return w.data()


def chunk(cid, data):
    out = cid + struct.pack(">I", len(data)) + data
    return out + (b"\0" if len(data) & 1 else b"")


def glow_form(images, palette):
    """ColorIcon (OS 3.5 GlowIcons) FORM ICON. images: lists of rows of
    palette indices, index 0 transparent; palette: list of (r, g, b)."""
    w, h = len(images[0][0]), len(images[0])
    ncol = len(palette)
    depth = max(1, (ncol - 1).bit_length())
    pal = rle([c for rgb in palette for c in rgb], 8)
    face = struct.pack(">BBBBH", w - 1, h - 1, 0, 0, len(pal) - 1)
    out = b"ICON" + chunk(b"FACE", face)
    for img in images:
        data = rle([v for row in img for v in row], depth)
        head = struct.pack(">BBBBBBHH", 0, ncol - 1, 3, 1, 1, depth, len(data) - 1, len(pal) - 1)
        out += chunk(b"IMAG", head + data + pal)
    return b"FORM" + struct.pack(">I", len(out)) + out


def amiga_string(s):
    b = s.encode("latin-1") + b"\0"
    return struct.pack(">I", len(b)) + b


def build(kind, images, tool=None, tooltypes=(), stack=0, drawer=None, glow=None):
    normal = images[0]
    w, h = len(normal[0]), len(normal)
    # GADGIMAGE plus GADGHIMAGE (selected image) or Workbench's backfill complement,
    # as in the system's own icons
    flags = 0x0006 if len(images) > 1 else 0x0005
    has_drawer = kind in ("disk", "drawer", "garbage")
    gadget = struct.pack(">IhhHHHHHIIIIIHI",
                         0, 0, 0, w, h, flags, 0x0001, 0x0001,
                         1, 1 if len(images) > 1 else 0, 0, 0, 0, 0,
                         1)                                 # UserData 1 = OS 2.x icon
    obj = struct.pack(">HH", 0xE310, 1) + gadget + struct.pack(
        ">BBIIIIIII", TYPES[kind], 0,
        1 if tool else 0, 1 if tooltypes else 0,
        NO_ICON_POSITION, NO_ICON_POSITION,
        1 if has_drawer else 0, 0, stack)
    if has_drawer:
        x, y, dw, dh = drawer or (50, 50, 400, 150)
        # NewWindow: LeftEdge TopEdge Width Height DetailPen BlockPen IDCMPFlags Flags
        #            FirstGadget CheckMark Title Screen BitMap MinW MinH MaxW MaxH Type
        newwin = struct.pack(">hhhhBBIIIIIIIhhHHH",
                             x, y, dw, dh, 255, 255, 0, 0,
                             0, 0, 0, 0, 0, 90, 40, 0xFFFF, 0xFFFF, 1)
        obj += newwin + struct.pack(">ii", 0, 0)            # dd_CurrentX/Y
    for img in images:
        obj += image_struct(img)
    if tool:
        obj += amiga_string(tool)
    if tooltypes:
        obj += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            obj += amiga_string(t)
    if has_drawer:
        obj += struct.pack(">IH", 0, 0)                     # DrawerData2: flags, view modes (default)
    if glow:
        obj += glow_form(*glow)
    return obj


def preview(images, path, scale=3):
    from PIL import Image
    w, h = len(images[0][0]), len(images[0])
    out = Image.new("RGB", ((w + 4) * len(images) * scale, (h + 2) * scale), PALETTE[0])
    for n, img in enumerate(images):
        for y, row in enumerate(img):
            for x, p in enumerate(row):
                for dy in range(scale):
                    for dx in range(scale):
                        out.putpixel((((w + 4) * n + x + 1) * scale + dx, (y + 1) * scale + dy),
                                     PALETTE[p])
    out.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("type", choices=TYPES)
    ap.add_argument("image")
    ap.add_argument("--tool")
    ap.add_argument("--tt", nargs="*", default=[])
    ap.add_argument("--stack", type=int, default=4096)
    ap.add_argument("--drawer")
    ap.add_argument("--png")
    a = ap.parse_args()
    images = load_images(a.image)
    drawer = tuple(int(v) for v in a.drawer.split(",")) if a.drawer else None
    open(a.out, "wb").write(build(a.type, images, a.tool, a.tt, a.stack, drawer))
    if a.png:
        preview(images, a.png)


if __name__ == "__main__":
    main()
