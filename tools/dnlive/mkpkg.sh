#!/bin/sh
# mkpkg.sh - builds dist/dnlive.tar.gz: the files plus a node list from
# config/nodes with names and addresses only (no users, no passwords)
set -e
DIR=$(dirname "$(readlink -f "$0")")
TOP=$(readlink -f "$DIR/../..")
TMP=$(mktemp -d)
mkdir "$TMP/dnlive"
cp "$DIR"/dnlive.py "$DIR"/index.html "$DIR"/kiosk.sh "$DIR"/install.sh \
	"$DIR"/dnlive.service "$DIR"/README "$TMP/dnlive/"
{
	echo "; DECnet node names for dnlive: NAME ADDRESS"
	awk '!/^;/ && NF >= 2 && $2 ~ /^[0-9]+\.[0-9]+$/ { printf "%-7s %s\n", $1, $2 }' \
		"$TOP/config/nodes"
} > "$TMP/dnlive/nodes"
mkdir -p "$TOP/dist"
tar -C "$TMP" -czf "$TOP/dist/dnlive.tar.gz" dnlive
rm -rf "$TMP"
echo "$TOP/dist/dnlive.tar.gz"
