#!/bin/bash
# Launches Amiberry with the DECnet config, bridging its uaenet.device (SANA-II)
# onto the real eth0 interface so native (non-IP) DECnet frames reach the LAN.
#
# Amiberry has no raw-socket capability and cannot get one (setcap breaks its
# $ORIGIN library lookup), so it uses its TAP backend on a pre-created tap0
# owned by this user. tap0 and eth0 are enslaved to the bridge br0.
#
# wlan0 (used for this host's normal network/internet access) is never
# touched. eth0 is handed back to NetworkManager, tap0 and the bridge are
# removed on exit, however the script terminates (normal exit, Ctrl+C, kill).
#
# Amiberry can be remote-controlled while running: tools/amiberry_ipc.py.

set -u

DIR=/home/torsten/Developer/Amiga/DECnet
# override with CONFIG=... (e.g. an installer test config on a copied hardfile)
CONFIG=${CONFIG:-/home/torsten/Amiberry/Configurations/DECnet.uae}
LOG="$DIR/amiberry.log"
# Patched build (uaenet.device fixes and receive/address fixes for the
# emulated network cards, see amiberry-uaenet-decnet.patch);
# override with AMIBERRY=/usr/bin/amiberry to run the packaged one.
AMIBERRY=${AMIBERRY:-/home/torsten/Developer/toolchains/amiberry-src/build/amiberry}
BRIDGE=br0
PHYS_IF=eth0
TAP_IF=tap0

# Refuse a second instance: it would share the hard disk image and tear down
# the running instance's bridge on exit.
if pgrep -x amiberry >/dev/null; then
	echo "Amiberry is already running, not starting a second instance."
	exit 1
fi

cleanup() {
	echo "Tearing down DECnet bridge ($BRIDGE, $TAP_IF)..."
	sudo -n ip link delete "$TAP_IF" 2>/dev/null
	sudo -n ip link set "$PHYS_IF" nomaster 2>/dev/null
	sudo -n ip link set "$BRIDGE" down 2>/dev/null
	sudo -n ip link delete "$BRIDGE" type bridge 2>/dev/null
	nmcli device set "$PHYS_IF" managed yes 2>/dev/null
}
trap cleanup EXIT INT TERM

echo "Setting up DECnet bridge ($BRIDGE + $PHYS_IF + $TAP_IF)..."

# Clean up anything left over from a previous crashed run.
if ip link show "$TAP_IF" >/dev/null 2>&1; then
	sudo -n ip link delete "$TAP_IF" 2>/dev/null
fi
if ip link show "$BRIDGE" >/dev/null 2>&1; then
	sudo -n ip link set "$PHYS_IF" nomaster 2>/dev/null
	sudo -n ip link delete "$BRIDGE" type bridge 2>/dev/null
fi

nmcli device set "$PHYS_IF" managed no || { echo "Could not unmanage $PHYS_IF, aborting."; exit 1; }
# STP off and no forward delay, so frames flow as soon as the ports are up.
sudo -n ip link add name "$BRIDGE" type bridge stp_state 0 forward_delay 0 || { echo "Could not create $BRIDGE, aborting."; exit 1; }
sudo -n ip link set "$BRIDGE" up
sudo -n ip link set "$PHYS_IF" master "$BRIDGE"
sudo -n ip link set "$PHYS_IF" up
sudo -n ip tuntap add dev "$TAP_IF" mode tap user "$(id -un)" || { echo "Could not create $TAP_IF, aborting."; exit 1; }
sudo -n ip link set "$TAP_IF" master "$BRIDGE"
sudo -n ip link set "$TAP_IF" up

cd "$DIR"
echo "=== $(date '+%F %T') start ===" >> "$LOG"
echo "binary: $AMIBERRY" >> "$LOG"
"$AMIBERRY" -f "$CONFIG" -G "$@" >> "$LOG" 2>&1
