#!/usr/bin/env python3
"""Make a level-0 LHA archive from LHa for UNIX readable by old Amiga LhA.

LHa for UNIX separates path components with 0xFF in level-0 file names;
Amiga LhA (e.g. 1.10 from 1991) expects '/'. This rewrites the separator
in place and fixes each header checksum.

Usage: lha_amigapath.py ARCHIVE
"""
import sys

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
pos = n = 0
while pos < len(data) and data[pos] != 0:
    size = data[pos]                     # header size, without the first 2 bytes
    if data[pos + 20] != 0:
        sys.exit("%s: not a level-0 header at %d" % (path, pos))
    packed = int.from_bytes(data[pos + 7:pos + 11], "little")
    namelen = data[pos + 21]
    for i in range(pos + 22, pos + 22 + namelen):
        if data[i] == 0xFF:
            data[i] = ord("/")
    data[pos + 1] = sum(data[pos + 2:pos + 2 + size]) & 0xFF
    pos += 2 + size + packed
    n += 1
open(path, "wb").write(data)
print("%s: %d headers fixed" % (path, n))
