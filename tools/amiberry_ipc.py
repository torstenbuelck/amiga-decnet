#!/usr/bin/env python3
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect("/run/user/1000/amiberry.sock")
s.sendall(("\t".join(sys.argv[1:]) + "\n").encode())
s.settimeout(2)
out = b""
try:
    while True:
        d = s.recv(65536)
        if not d: break
        out += d
except socket.timeout:
    pass
print(out.decode(errors="replace"))
