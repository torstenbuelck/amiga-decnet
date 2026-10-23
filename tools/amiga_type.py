#!/usr/bin/env python3
"""Type text into Amiberry via its IPC socket (SEND_KEY with Amiga rawkeys),
independent of host window focus.

Usage: amiga_type.py [--keymap us|d] TEXT...   (args joined by spaces)
Escapes in TEXT: \\n = Return, \\e = Esc, \\b = Backspace, \\t = Tab,
\\c = Ctrl-C, \\x = Ctrl-X (clear the shell line), \\E = Right-Amiga+E (Workbench "Execute Command").
"""
import socket
import sys
import time

SOCK = "/run/user/1000/amiberry.sock"
SHIFT, CTRL, LALT, RAMIGA = 0x60, 0x63, 0x64, 0x67

# Rawkeys by key position (same on all Amiga keyboards).
LETTERS = {c: k for c, k in zip("qwertyuiop", range(0x10, 0x1a))}
LETTERS.update({c: k for c, k in zip("asdfghjkl", range(0x20, 0x29))})
LETTERS.update({c: k for c, k in zip("zxcvbnm", range(0x31, 0x38))})
DIGITS = {c: k for c, k in zip("1234567890", range(0x01, 0x0b))}

# char -> (rawkey, shift) for the symbol keys, per keymap.
US = {
    "`": (0x00, 0), "~": (0x00, 1), "-": (0x0b, 0), "_": (0x0b, 1),
    "=": (0x0c, 0), "+": (0x0c, 1), "\\": (0x0d, 0), "|": (0x0d, 1),
    "[": (0x1a, 0), "{": (0x1a, 1), "]": (0x1b, 0), "}": (0x1b, 1),
    ";": (0x29, 0), ":": (0x29, 1), "'": (0x2a, 0), '"': (0x2a, 1),
    ",": (0x38, 0), "<": (0x38, 1), ".": (0x39, 0), ">": (0x39, 1),
    "/": (0x3a, 0), "?": (0x3a, 1),
    "!": (0x01, 1), "@": (0x02, 1), "#": (0x03, 1), "$": (0x04, 1),
    "%": (0x05, 1), "^": (0x06, 1), "&": (0x07, 1), "*": (0x08, 1),
    "(": (0x09, 1), ")": (0x0a, 1),
}
D = {
    "`": (0x00, 0), "~": (0x00, 1), "?": (0x0b, 1),
    "\\": (0x0d, 0), "|": (0x0d, 1), "+": (0x1b, 0), "*": (0x1b, 1),
    "#": (0x2b, 0), "^": (0x2b, 1), "<": (0x30, 0), ">": (0x30, 1),
    ",": (0x38, 0), ";": (0x38, 1), ".": (0x39, 0), ":": (0x39, 1),
    "-": (0x3a, 0), "_": (0x3a, 1),
    "!": (0x01, 1), '"': (0x02, 1), "$": (0x04, 1), "%": (0x05, 1),
    "&": (0x06, 1), "/": (0x07, 1), "(": (0x08, 1), ")": (0x09, 1),
    "=": (0x0a, 1), "'": (0x2b, 1),
    # Alt combinations (verified 2026-10-21 on the Amiga): 2 = Alt, 3 = Alt+Shift
    "[": (0x1a, 2), "]": (0x1b, 2), "{": (0x1a, 3), "}": (0x1b, 3),
}
SPECIAL = {"n": [(0x44, ())], "e": [(0x45, ())], "b": [(0x41, ())],
           "t": [(0x42, ())], "c": [(0x33, (CTRL,))], "x": [(0x32, (CTRL,))],
           "E": [(0x12, (RAMIGA,))]}


def ipc(*args):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.sendall(("\t".join(str(a) for a in args) + "\n").encode())
    s.settimeout(2)
    out = s.recv(4096).decode(errors="replace")
    s.close()
    if not out.startswith("OK"):
        raise RuntimeError(out.strip())


def press(key, mods=()):
    for m in mods:
        ipc("SEND_KEY", m, 1)
    ipc("SEND_KEY", key, 1)
    time.sleep(0.03)
    ipc("SEND_KEY", key, 0)
    for m in reversed(mods):
        ipc("SEND_KEY", m, 0)
    time.sleep(0.03)


def keys_for(ch, keymap):
    lower = ch.lower()
    if lower in LETTERS:
        k = LETTERS[lower]
        if keymap == "d" and lower in "yz":
            k = LETTERS["z" if lower == "y" else "y"]
        return k, ch != lower
    if ch in DIGITS:
        return DIGITS[ch], False
    if ch == " ":
        return 0x40, False
    table = D if keymap == "d" else US
    if ch in table:
        return table[ch]
    raise ValueError("cannot type %r with keymap %s" % (ch, keymap))


def type_text(text, keymap):
    i = 0
    while i < len(text):
        ch = text[i]
        if ch == "\\" and i + 1 < len(text) and text[i + 1] in SPECIAL:
            for key, mods in SPECIAL[text[i + 1]]:
                press(key, mods)
            i += 2
            continue
        key, mod = keys_for(ch, keymap)
        mods = {0: (), 1: (SHIFT,), 2: (LALT,), 3: (LALT, SHIFT)}[mod]
        press(key, mods)
        i += 1


def main():
    args = sys.argv[1:]
    keymap = "d"   # the Amiga uses the German keymap
    if len(args) >= 2 and args[0] == "--keymap":
        keymap, args = args[1], args[2:]
    type_text(" ".join(args), keymap)


if __name__ == "__main__":
    main()
