#!/bin/bash
# Packs cnetfix.lha next to this script: all eight cnet.device 1.9
# versions from aminet/cnetdevice.lha (Aminet driver/net/cnetdevice.lha),
# fixed with tools/cnetpatch.py, plus the patch tool, the source patch and
# the ReadMe.
set -eu
DIR=$(cd "$(dirname "$0")" && pwd)
LHA=${LHA:-/home/torsten/Developer/toolchains/lha-src/src/lha}
B=$(mktemp -d)
trap 'rm -rf "$B"' EXIT

(cd "$B" && "$LHA" -xq "$DIR/aminet/cnetdevice.lha")
P="$B/pkg/cnetfix"
mkdir -p "$P/Devs/Networks"
for f in "$B"/cnetdevice/Devs/Networks/cnet*; do
	python3 -I "$DIR/../../tools/cnetpatch.py" "$f" "$P/Devs/Networks/$(basename "$f")" >/dev/null
done
cp "$DIR/ReadMe" "$DIR/cnetfix.patch" "$DIR/../../tools/cnetpatch.py" "$P/"
# LiesMich is UTF-8 in the repository; the Amiga uses ISO-8859-1
iconv -f UTF-8 -t ISO-8859-1 "$DIR/LiesMich" >"$P/LiesMich"
chmod 644 "$P"/Devs/Networks/* "$P"/ReadMe "$P"/LiesMich "$P"/cnetfix.patch "$P"/cnetpatch.py
rm -f "$DIR/cnetfix.lha"
(cd "$B/pkg" && "$LHA" -aq "$DIR/cnetfix.lha" cnetfix)
(cd "$P/Devs/Networks" && md5sum *)
md5sum "$DIR/cnetfix.lha"
