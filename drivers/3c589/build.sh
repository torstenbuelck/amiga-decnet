#!/bin/bash
# Builds 3c589.device 1.5auto3 from src/ with bebbo's amiga-gcc
# (/opt/amiga) and packs 3c589auto.lha next to this script: the driver, this source,
# the patch against Aminet driver/net/3c589.lha 1.5 and the GPL.
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
LHA=${LHA:-/home/torsten/Developer/toolchains/lha-src/src/lha}
export PATH=/opt/amiga/bin:$PATH
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

cp -r "$DIR/src" "$B/src"
cd "$B/src"
CF="-m68000 -msmall-code -O2 -w -fno-toplevel-reorder -noixemul -Iinc"
for f in device unit request; do m68k-amigaos-gcc $CF -c $f.c; done
m68k-amigaos-gcc -m68000 -noixemul -nostartfiles -nostdlib -s \
   device.o unit.o request.o -lamiga -lgcc -o "$B/3c589.device"
rm -f *.o

P="$B/pkg/3c589auto"
mkdir -p "$P/Devs/networks"
cp "$B/3c589.device" "$P/Devs/networks/"
cp "$DIR/ReadMe" "$DIR/GPL" "$DIR/1.5auto3.patch" "$P/"
# LiesMich is UTF-8 in the repository; the Amiga uses ISO-8859-1
iconv -f UTF-8 -t ISO-8859-1 "$DIR/LiesMich" >"$P/LiesMich"
cp -r "$B/src" "$P/Source"
rm -f "$DIR/3c589auto.lha"
(cd "$B/pkg" && "$LHA" -aq "$DIR/3c589auto.lha" 3c589auto)
md5sum "$P/Devs/networks/3c589.device" "$DIR/3c589auto.lha"
