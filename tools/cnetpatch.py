#!/usr/bin/env python3
"""Fixes cnet.device / cnet16.device 1.9 (Aminet driver/net/cnetdevice.lha,
public domain) so that it carries DECnet. Two bugs, one byte each:

1. S2_CONFIGINTERFACE checks the new address with

       move.l  ios2_SrcAddr(a1),d0     20 29 00 28
       ble     .badaddr                6F xx

   A signed test: every address whose first byte is 0x80 or more (DECnet's
   AA-00-04-00-xx-xx among them) counts as invalid, the unit stays
   unconfigured and refuses all reads and writes. The fix turns BLE into
   BEQ (67 xx), so only an all-zero first longword is refused.

2. When the last opener closes the device, devclosedown stops and resets
   the controller but keeps the "configured" flag:

       and.w   #~(ONLINE|MINE|PROM),dd_flags(a1)    02 69 FF 37 00 22

   The next OpenDevice initialises the controller again with the card's
   own address, while S2_CONFIGINTERFACE is refused as "already
   configured". The card then sends with the DECnet address but never
   receives frames sent to it (dncheck or DECnet-Stop, then DECnet-Start).
   The fix clears CONFIGURED as well (FF 33), so the next opener sets the
   address again.

The date in the $VER string becomes 15.11.26 (same length) to tell the
fixed file apart. Found and tested 2026-11 on an A1200 with a D-Link
DE-660+.

Usage: cnetpatch.py IN OUT
"""
import re
import sys

PATCHES = (
    ("move.l ios2_SrcAddr,d0; ble", re.compile(rb"\x20\x29\x00\x28\x6f"), 4, 0x67),
    ("and.w #~(ONLINE|MINE|PROM),dd_flags", re.compile(rb"\x02\x69\xff\x37\x00\x22"), 3, 0x33),
)
OLD_DATE = b"(8.8.2007)"
NEW_DATE = b"(15.11.26)"


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    data = bytearray(open(sys.argv[1], "rb").read())
    fixed = []
    for name, pattern, offset, value in PATCHES:
        hits = [m.start() for m in pattern.finditer(data)]
        if len(hits) != 1:
            sys.exit("%s: expected one '%s', found %d" % (sys.argv[1], name, len(hits)))
        data[hits[0] + offset] = value
        fixed.append("0x%x" % (hits[0] + offset))
    if data.count(OLD_DATE) != 1:
        sys.exit("%s: version date %s not found once" % (sys.argv[1], OLD_DATE.decode()))
    data[:] = data.replace(OLD_DATE, NEW_DATE)
    open(sys.argv[2], "wb").write(data)
    print("%s: fixed at offsets %s -> %s" % (sys.argv[1], ", ".join(fixed), sys.argv[2]))


if __name__ == "__main__":
    main()
