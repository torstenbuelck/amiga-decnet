#!/usr/bin/env python3
"""Fixes cnet.device / cnet16.device 1.9 (Aminet driver/net/cnetdevice.lha,
public domain) so that it takes DECnet station addresses.

S2_CONFIGINTERFACE checks the new address with

    move.l  ios2_SrcAddr(a1),d0     20 29 00 28
    ble     .badaddr                6F xx

A signed test: every address whose first byte is 0x80 or more (DECnet's
AA-00-04-00-xx-xx among them) counts as invalid, the unit stays
unconfigured and refuses all reads and writes. The fix turns BLE into BEQ
(67 xx), so only an all-zero first longword is refused. The date in the
$VER string becomes 04.11.26 (same length) to tell the fixed file apart.

Usage: cnetpatch.py IN OUT
"""
import re
import sys

PATTERN = re.compile(rb"\x20\x29\x00\x28\x6f")
OLD_DATE = b"(8.8.2007)"
NEW_DATE = b"(04.11.26)"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = bytearray(open(sys.argv[1], "rb").read())
    hits = [m.start() for m in PATTERN.finditer(data)]
    if len(hits) != 1:
        sys.exit("%s: expected one 'move.l ios2_SrcAddr,d0; ble', found %d"
                 % (sys.argv[1], len(hits)))
    data[hits[0] + 4] = 0x67
    if data.count(OLD_DATE) != 1:
        sys.exit("%s: version date %s not found once" % (sys.argv[1], OLD_DATE.decode()))
    data[:] = data.replace(OLD_DATE, NEW_DATE)
    open(sys.argv[2], "wb").write(data)
    print("%s: fixed at offset 0x%x -> %s" % (sys.argv[1], hits[0] + 4, sys.argv[2]))


if __name__ == "__main__":
    main()
