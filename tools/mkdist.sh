#!/bin/bash
# Builds the installable DECnet package: dist/DECnet/ (the drawer the user
# unpacks, with Install-DECnet) and dist/DECnet.lha, and the terminal
# add-on dist/DECnet-Term/ and dist/DECnet-Term.lha.
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
make -C "$DIR/src/term" >/dev/null
make -C "$DIR/src/maint" >/dev/null
make -C "$DIR/src/lat" latd >/dev/null

rm -rf "$OUT"
mkdir -p "$PKG"/{C,L,S,Libs,Storage,Icons}

cp "$DIR/installer/Install-DECnet" "$DIR/installer/ReadMe" "$DIR/installer/LiesMich" "$PKG/"
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
cp "$ICONS/ReadMe.info" "$PKG/LiesMich.info"
cp "$ICONS"/{Disk,Node,Dir,Text,Picture,File,DECnet}.info "$PKG/Icons/"   # DN: + installed drawer

# the terminal add-on
TPKG="$OUT/DECnet-Term"
mkdir -p "$TPKG/Devs" "$TPKG/C" "$TPKG/S"
cp "$DIR/installer/term/Install-DECnet-Term" "$DIR/installer/term/ReadMe" "$DIR/installer/term/LiesMich" "$TPKG/"
cp "$DIR/src/term/dnterm.device" "$TPKG/Devs/"
cp "$DIR/src/term/dntermpatch" "$DIR/src/maint/dnmaint" "$DIR/src/lat/latd" "$TPKG/C/"
cp "$DIR"/installer/term/S/{DECnet-Start-Term,DECnet-Stop-Term} "$TPKG/S/"
cp "$ICONS/DECnet-Term.info" "$OUT/"
cp "$ICONS"/{Install-DECnet-Term,ReadMe}.info "$TPKG/"
cp "$ICONS/ReadMe.info" "$TPKG/LiesMich.info"
rm -r "$ICONS"

# AmigaDOS wants LF line ends and no trailing CR; make sure.
for f in "$PKG/Install-DECnet" "$PKG/ReadMe" "$PKG/LiesMich" "$PKG"/S/* "$PKG"/Storage/* \
         "$TPKG/Install-DECnet-Term" "$TPKG/ReadMe" "$TPKG/LiesMich" "$TPKG"/S/*; do
	sed -i 's/\r$//' "$f"
done
# Installer and ReadMe are UTF-8 in the repository; the Amiga uses ISO-8859-1.
for f in "$PKG/Install-DECnet" "$PKG/ReadMe" "$PKG/LiesMich" \
         "$TPKG/Install-DECnet-Term" "$TPKG/ReadMe" "$TPKG/LiesMich"; do
	iconv -f UTF-8 -t ISO-8859-1 "$f" >"$f.tmp" && mv "$f.tmp" "$f"
done

# lh5, generic level-0 headers with '/' as path separator: what old Amiga
# LhA versions (1.10 from 1991) understand. Files only; LhA creates the
# drawers itself.
(cd "$OUT" && "$LHA" -ao5gq2 -0 DECnet.lha DECnet.info $(cd "$OUT" && find DECnet -type f | sort))
python3 "$DIR/tools/lha_amigapath.py" "$OUT/DECnet.lha"
(cd "$OUT" && "$LHA" -ao5gq2 -0 DECnet-Term.lha DECnet-Term.info $(cd "$OUT" && find DECnet-Term -type f | sort))
python3 "$DIR/tools/lha_amigapath.py" "$OUT/DECnet-Term.lha"
ls -l "$OUT/DECnet.lha" "$OUT/DECnet-Term.lha"
