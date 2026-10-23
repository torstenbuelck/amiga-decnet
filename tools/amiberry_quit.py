#!/usr/bin/env python3
"""Quit Amiberry safely: AmigaOS has no shutdown, so only send QUIT once the
hard disk and floppy LEDs have been idle for a while (pending FFS writes are
flushed shortly after the last access).

Usage: amiberry_quit.py [idle_seconds] [timeout_seconds]
"""
import socket
import sys
import time

SOCK = "/run/user/1000/amiberry.sock"
POLL = 0.1


def ipc(*args):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(SOCK)
    s.sendall(("\t".join(args) + "\n").encode())
    s.settimeout(2)
    out = b""
    try:
        while True:
            d = s.recv(65536)
            if not d:
                break
            out += d
    except socket.timeout:
        pass
    s.close()
    return out.decode(errors="replace").strip().split("\t")


def busy():
    reply = ipc("GET_LED_STATUS")
    if reply[0] != "OK":
        raise RuntimeError("GET_LED_STATUS failed: " + " ".join(reply))
    leds = dict(f.split("=", 1) for f in reply[1:] if "=" in f)
    if leds.get("hd", "0") != "0":
        return "hd=" + leds["hd"]
    for i in range(4):
        if leds.get("df%d" % i, "off") != "off":
            return "df%d=%s" % (i, leds["df%d" % i])
    return None


def main():
    idle = float(sys.argv[1]) if len(sys.argv) > 1 else 5.0
    timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0
    start = time.monotonic()
    quiet_since = time.monotonic()
    while True:
        now = time.monotonic()
        b = busy()
        if b:
            quiet_since = now
        elif now - quiet_since >= idle:
            break
        if now - start > timeout:
            print("Disk not idle for %.0f s within %.0f s (last: %s), not quitting." % (idle, timeout, b))
            sys.exit(1)
        time.sleep(POLL)
    print("Disks idle for %.0f s, sending QUIT." % idle)
    print(" ".join(ipc("QUIT")))


if __name__ == "__main__":
    main()
