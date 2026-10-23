#!/bin/bash
# Builds the installable DECnet package: dist/DECnet/ (the drawer the user
# unpacks, with Install-DECnet) and dist/DECnet.lha.
#
# In Amiberry: cd Transfer:DECnet/dist/DECnet, then
# Installer Install-DECnet MINUSER AVERAGE
# (or double-click its icon).

set -eu

DIR=/home/torsten/Developer/Amiga/DECnet
OUT="$DIR/dist"
# LHa for UNIX (jca02266/lha), built in toolchains; Lhasa cannot create archives
LHA=${LHA:-/home/torsten/Developer/toolchains/lha-src/src/lha}
PKG="$OUT/DECnet"

make -C "$DIR/src/amiga" >/dev/null
make -C "$DIR/src/apps" >/dev/null
make -C "$DIR/src/handler" >/dev/null

rm -rf "$OUT"
mkdir -p "$PKG"/{C,L,S,Libs,Storage,Icons}

cp "$DIR/installer/Install-DECnet" "$DIR/installer/ReadMe" "$PKG/"
cp "$DIR"/src/amiga/{dnetd,dncheck,dnstartup} "$DIR"/src/apps/{dncopy,dndir,dnnodes} "$PKG/C/"
cp "$DIR/src/handler/dn-handler" "$PKG/L/"
cp "$DIR/src/amiga/decnet.library" "$PKG/Libs/"
cp "$DIR"/installer/S/{DECnet-Start.body,DECnet-Stop} "$PKG/S/"
cp "$DIR"/installer/Storage/{Mountlist,nodes.head} "$PKG/Storage/"
# icons: drawn by tools/icons.py (classic + GlowIcon)
ICONS="$OUT/icons.tmp"
python3 "$DIR/tools/icons.py" "$ICONS" >/dev/null
cp "$ICONS/DECnet.info" "$OUT/"                         # the drawer in the archive
cp "$ICONS"/{Install-DECnet,ReadMe}.info "$PKG/"
cp "$ICONS"/{Disk,Node,Dir,Text,Picture,File,DECnet}.info "$PKG/Icons/"   # DN: + installed drawer
rm -r "$ICONS"

# AmigaDOS wants LF line ends and no trailing CR; make sure.
for f in "$PKG/Install-DECnet" "$PKG/ReadMe" "$PKG"/S/* "$PKG"/Storage/*; do
	sed -i 's/\r$//' "$f"
done
# Installer and ReadMe are UTF-8 in the repository; the Amiga uses ISO-8859-1.
for f in "$PKG/Install-DECnet" "$PKG/ReadMe"; do
	iconv -f UTF-8 -t ISO-8859-1 "$f" >"$f.tmp" && mv "$f.tmp" "$f"
done

# lh5, generic level-0 headers with '/' as path separator: what old Amiga
# LhA versions (1.10 from 1991) understand. Files only; LhA creates the
# drawers itself.
(cd "$OUT" && "$LHA" -ao5gq2 -0 DECnet.lha DECnet.info $(cd "$OUT" && find DECnet -type f | sort))
python3 "$DIR/tools/lha_amigapath.py" "$OUT/DECnet.lha"
ls -l "$OUT/DECnet.lha"
