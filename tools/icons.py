#!/usr/bin/env python3
"""Draw the DECnet icons and write them as .info files (classic 4-colour
image for OS 2.04/3.1 plus OS 3.5 GlowIcon colour images).

Every icon is drawn once with colour roles; the roles map to a colour
palette for the GlowIcon and to the four Workbench pens for the classic
image. The selected GlowIcon gets the usual yellow glow.

Usage: icons.py OUTDIR [--png PREVIEW.png]

Writes OUTDIR/{Install-DECnet,ReadMe,DECnet,Disk,Node,Dir,Text,Picture,File}.info.

Copyright (C) 2026  Torsten
SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkicon  # noqa: E402

W = H = 46

# role -> (GlowIcon colour, classic pen 0 grey / 1 black / 2 white / 3 blue)
ROLES = {
    # Colours close to the pens a 16-colour Workbench has anyway (grey,
    # black, white, Workbench blue, the beige/cream and red/green of the
    # system GlowIcons), so the icons survive the remapping to few pens.
    "line":    ((0, 0, 0), 1),
    "white":   ((255, 255, 255), 2),
    "light":   ((238, 238, 238), 2),
    "mid":     ((170, 170, 170), 0),
    "dark":    ((102, 102, 102), 1),
    "beige1":  ((238, 238, 204), 2),      # VAX cabinet
    "beige2":  ((204, 204, 153), 2),
    "beige3":  ((170, 170, 170), 0),
    "beige4":  ((0, 0, 0), 1),
    "band":    ((34, 34, 51), 1),         # near black DEC trim (dark blue
                                          # becomes green on 16 colours)
    "band2":   ((102, 136, 187), 3),      # Workbench blue
    "maroon":  ((170, 17, 34), 3),        # DEC logo blocks
    "green":   ((51, 221, 85), 2),
    "dgreen":  ((0, 136, 68), 1),
    "red":     ((238, 51, 51), 1),
    "paper":   ((255, 255, 255), 2),
    "paper2":  ((204, 204, 204), 0),
    "text":    ((102, 102, 102), 1),
    "sun":     ((238, 153, 0), 2),
}
GLOW = [(255, 200, 0), (255, 236, 120)]   # inner and outer glow ring

FONT = {  # 3x5
    "D": ["110", "101", "101", "101", "110"],
    "E": ["111", "100", "110", "100", "111"],
    "C": ["011", "100", "100", "100", "011"],
    "n": ["000", "110", "101", "101", "101"],
    "e": ["000", "011", "111", "100", "011"],
    "t": ["010", "111", "010", "010", "001"],
    "V": ["101", "101", "101", "101", "010"],
    "A": ["010", "101", "111", "101", "101"],
    "X": ["101", "101", "010", "101", "101"],
    "0": ["010", "101", "101", "101", "010"],
    "1": ["010", "110", "010", "010", "111"],
    "[": ["110", "100", "100", "100", "110"],
    "]": ["011", "001", "001", "001", "011"],
}


class Canvas:
    """Draws with roles: im holds role numbers (0 = transparent)."""

    def __init__(self):
        self.names = list(ROLES)
        self.im = Image.new("L", (W, H), 0)
        self.d = ImageDraw.Draw(self.im)

    def r(self, role):
        return self.names.index(role) + 1

    def rect(self, x0, y0, x1, y1, fill, line=None):
        self.d.rectangle((x0, y0, x1, y1), fill=self.r(fill),
                         outline=self.r(line) if line else None)

    def poly(self, pts, fill, line=None):
        self.d.polygon(pts, fill=self.r(fill), outline=self.r(line) if line else None)

    def hline(self, x0, x1, y, role):
        self.d.line((x0, y, x1, y), fill=self.r(role))

    def vline(self, x, y0, y1, role):
        self.d.line((x, y0, x, y1), fill=self.r(role))

    def px(self, x, y, role):
        self.im.putpixel((x, y), self.r(role))

    def text(self, x, y, s, role):
        for ch in s:
            for dy, row in enumerate(FONT[ch]):
                for dx, bit in enumerate(row):
                    if bit == "1":
                        self.px(x + dx, y + dy, role)
            x += 4

    def dec_blocks(self, x, y, s, block="maroon", letter="white"):
        """The DEC logo look: each letter white in its own block, 5x7."""
        for ch in s:
            self.rect(x, y, x + 4, y + 6, block)
            self.text(x + 1, y + 1, ch, letter)
            x += 6


# ---------------------------------------------------------------- motifs

def drive(c, top=8, label="DECnet"):
    """Hard disk in the Workbench style, DEC logo blocks on the front."""
    f0, f1 = top + 7, top + 27              # front face
    c.poly([(2, f0), (8, top), (44, top), (38, f0)], "light", "line")
    c.poly([(38, f0), (44, top), (44, f1 - 6), (38, f1)], "dark", "line")
    c.rect(2, f0, 38, f1, "mid", "line")
    c.hline(3, 37, f0 + 1, "light")
    c.dec_blocks(4, f0 + 3, label)
    c.hline(6, 28, f1 - 5, "dark")         # drive slot
    c.hline(6, 28, f1 - 4, "light")
    c.rect(32, f1 - 5, 34, f1 - 4, "green")
    c.hline(3, 37, f1 - 1, "dark")


def vax(c):
    """VAX cabinet (BA-style pedestal): beige, dark trim, panel, vents."""
    x0, x1, y0, y1, dp = 12, 31, 7, 42, 5
    c.poly([(x0, y0), (x0 + dp, y0 - dp), (x1 + dp, y0 - dp), (x1, y0)], "beige1", "line")
    c.poly([(x1, y0), (x1 + dp, y0 - dp), (x1 + dp, y1 - dp), (x1, y1)], "beige3", "line")
    c.rect(x0, y0, x1, y1, "beige2", "line")
    c.vline(x0 + 1, y0 + 1, y1 - 1, "beige1")
    c.rect(x0 + 1, y0 + 2, x1 - 1, y0 + 8, "band")          # top trim with name
    c.text(x0 + 4, y0 + 3, "VAX", "white")
    c.rect(x0 + 3, y0 + 11, x1 - 3, y0 + 16, "band2", "line")  # control panel
    c.px(x0 + 5, y0 + 13, "green")
    c.px(x0 + 7, y0 + 13, "red")
    c.hline(x0 + 10, x1 - 5, y0 + 14, "white")
    for y in range(y0 + 20, y1 - 3, 2):                     # vents
        c.hline(x0 + 3, x1 - 3, y, "beige4")
    c.rect(x0, y1 - 2, x1, y1, "beige4", "line")            # plinth
    c.poly([(x1, y1 - 2), (x1 + dp, y1 - 2 - dp), (x1 + dp, y1 - dp), (x1, y1)], "beige4", "line")


PX0, PY0, PX1, PY1, PK = 9, 3, 36, 42, 7     # the page of all file icons


def blank_page(c):
    """Page with a folded corner and a shadow."""
    x0, y0, x1, y1, k = PX0, PY0, PX1, PY1, PK
    c.poly([(x0, y0), (x1 - k, y0), (x1, y0 + k), (x1, y1), (x0, y1)], "paper", "line")
    c.poly([(x1 - k, y0), (x1 - k, y0 + k), (x1, y0 + k)], "paper2", "line")
    c.vline(x1 + 1, y0 + k + 1, y1 + 1, "dark")             # shadow
    c.hline(x0 + 1, x1 + 1, y1 + 1, "dark")


def text_lines(c, top):
    for i, y in enumerate(range(top, PY1 - 3, 3)):
        c.hline(PX0 + 3, PX1 - 3 - (6 if i % 3 == 2 else 0), y, "text")


def page(c):
    """The ReadMe: text page with a DEC block header."""
    blank_page(c)
    c.dec_blocks(PX0 + 3, PY0 + 3, "DEC")
    text_lines(c, PY0 + 14)


def text_file(c):
    """A text file on a node (.TXT, .COM, .LIS ...)."""
    blank_page(c)
    text_lines(c, PY0 + 5)


def picture_file(c):
    """A picture on a node (.IFF, .GIF ...): landscape on the page."""
    blank_page(c)
    x0, y0, x1, y1 = PX0 + 3, PY0 + 9, PX1 - 3, PY0 + 28
    c.rect(x0, y0, x1, y1, "band2", "line")
    c.rect(x1 - 7, y0 + 2, x1 - 4, y0 + 5, "sun")
    c.poly([(x0 + 1, y1 - 1), (x0 + 1, y1 - 6), (x0 + 8, y1 - 11), (x0 + 16, y1 - 5),
            (x1 - 1, y1 - 8), (x1 - 1, y1 - 1)], "dgreen")
    text_lines(c, y1 + 4)


def data_file(c):
    """Any other file on a node: binary digits on the page."""
    blank_page(c)
    for row, y in enumerate(range(PY0 + 5, PY1 - 6, 7)):
        bits = "10110100"[row % 4:] + "10110100"[:row % 4]
        c.text(PX0 + 3, y, bits[:4], "text")


def vms_dir(c):
    """A directory on a node: beige drawer with a [ ] label."""
    x0, x1, y0, y1, dp = 7, 34, 13, 40, 5
    c.poly([(x0, y0), (x0 + dp, y0 - dp), (x1 + dp, y0 - dp), (x1, y0)], "beige1", "line")
    c.poly([(x1, y0), (x1 + dp, y0 - dp), (x1 + dp, y1 - dp), (x1, y1)], "beige3", "line")
    c.rect(x0, y0, x1, y1, "beige2", "line")
    c.rect(x0 + 2, y0 + 3, x1 - 2, y1 - 3, "beige1", "line")     # drawer front
    c.rect(x0 + 5, y0 + 6, x1 - 5, y0 + 12, "white", "line")     # label: [ ]
    c.text(x0 + 9, y0 + 7, "[", "band")
    c.text(x1 - 12, y0 + 7, "]", "band")
    c.rect(x0 + 10, y1 - 9, x1 - 10, y1 - 7, "band", "line")     # handle


def installer(c):
    """The drive with a green arrow pointing into it."""
    drive(c, top=17)
    pts = [(17, 1), (25, 1), (25, 9), (31, 9), (21, 19), (11, 9), (17, 9)]
    c.poly(pts, "green", "line")
    c.vline(18, 2, 9, "white")


def drawer(c):
    """Two-drawer cabinet in DEC blue with logo blocks."""
    x0, x1, y0, y1, dp = 8, 33, 9, 41, 5
    c.poly([(x0, y0), (x0 + dp, y0 - dp), (x1 + dp, y0 - dp), (x1, y0)], "band2", "line")
    c.poly([(x1, y0), (x1 + dp, y0 - dp), (x1 + dp, y1 - dp), (x1, y1)], "band", "line")
    c.rect(x0, y0, x1, y1, "band2", "line")
    for d0 in (y0 + 2, y0 + 17):
        c.rect(x0 + 2, d0, x1 - 2, d0 + 13, "band", "line")
        c.rect(x0 + 10, d0 + 9, x1 - 10, d0 + 10, "light")  # handle
    c.dec_blocks(x0 + 4, y0 + 4, "DEC", block="maroon")


# ---------------------------------------------------------------- output

def to_images(c):
    """GlowIcon images (normal, selected) with palette, classic image."""
    names = c.names
    src = list(c.im.getdata())
    mask = [v != 0 for v in src]

    def ring(m):
        out = [False] * (W * H)
        for y in range(H):
            for x in range(W):
                if m[y * W + x]:
                    continue
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        xx, yy = x + dx, y + dy
                        if 0 <= xx < W and 0 <= yy < H and m[yy * W + xx]:
                            out[y * W + x] = True
        return out

    r1 = ring(mask)
    r2 = ring([a or b for a, b in zip(mask, r1)])
    normal = [ROLES[names[v - 1]][0] if v else None for v in src]
    sel = list(normal)
    for i in range(W * H):
        if r1[i]:
            sel[i] = GLOW[0]
        elif r2[i]:
            sel[i] = GLOW[1]
    # index 0 is transparent; its colour must not be looked up for black
    colours = []
    for col in normal + sel:
        if col is not None and col not in colours:
            colours.append(col)
    palette = [(0, 0, 0)] + colours

    def index(img):
        flat = [colours.index(v) + 1 if v is not None else 0 for v in img]
        return [flat[y * W:(y + 1) * W] for y in range(H)]

    classic = [ROLES[names[v - 1]][1] if v else 0 for v in src]
    classic = [classic[y * W:(y + 1) * W] for y in range(H)]
    return index(normal), index(sel), palette, classic, normal, sel


ICONS = [
    # file, type, motif, default tool, tooltypes
    ("Install-DECnet", "project", installer, "Installer", ["APPNAME=DECnet", "MINUSER=AVERAGE"]),
    ("ReadMe", "project", page, "More", []),             # OS 2.04 has no MultiView
    ("DECnet", "drawer", drawer, None, []),
    ("Disk", "disk", drive, None, []),
    ("Node", "drawer", vax, None, []),
    # inside the nodes (dn-handler picks them by type and file name extension)
    ("Dir", "drawer", vms_dir, None, []),
    ("Text", "project", text_file, "MultiView", []),
    ("Picture", "project", picture_file, "MultiView", []),
    ("File", "project", data_file, None, []),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("outdir")
    ap.add_argument("--png")
    a = ap.parse_args()
    os.makedirs(a.outdir, exist_ok=True)
    previews = []
    for name, kind, motif, tool, tts in ICONS:
        c = Canvas()
        motif(c)
        gn, gs, pal, classic, rn, rs = to_images(c)
        data = mkicon.build(kind, [classic], tool, tts, 4096,
                            (50, 50, 400, 150) if kind in ("drawer", "disk") else None,
                            glow=([gn, gs], pal))
        open(os.path.join(a.outdir, name + ".info"), "wb").write(data)
        previews.append((classic, rn, rs))
        print("%-16s %5d bytes, %d colours" % (name + ".info", len(data), len(pal)))
    if a.png:
        s, gap = 3, 6
        out = Image.new("RGB", ((W * 3 + gap * 3) * s, (H + gap) * len(previews) * s), (170, 170, 170))
        cls = mkicon.PALETTE
        for row, (classic, rn, rs) in enumerate(previews):
            for col, img in enumerate((None, rn, rs)):
                for y in range(H):
                    for x in range(W):
                        v = classic[y][x] if col == 0 else img[y * W + x]
                        rgb = cls[v] if col == 0 else (v or (170, 170, 170))
                        for dy in range(s):
                            for dx in range(s):
                                out.putpixel((((W + gap) * col + x) * s + dx,
                                              ((H + gap) * row + y) * s + dy), rgb)
        out.save(a.png)


if __name__ == "__main__":
    main()
