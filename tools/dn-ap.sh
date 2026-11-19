#!/bin/bash
# dn-ap.sh - temporary WLAN access point bridged to the DECnet strand.
#
# Runtime only: every NetworkManager profile created here is in-memory
# (save no / --temporary), nothing is written to disk, no packages needed.
# A reboot always brings back the normal configuration.
#
# Roles while active:
#   STICK (wlan1, USB stick)  client in the house WLAN (clone of the profile
#                             that was active on APIF) - keeps us online
#   APIF  (wlan0, internal)   2.4 GHz access point, port of BR
#   ETH   (eth0)              DECnet strand, port of BR
#   BR    (br0)               bridge, no IP; DECnet tools on the Pi use -i br0
#
# Usage: sudo tools/dn-ap.sh start|confirm|stop|status
#   start    bring it up; reverts by itself after TIMEOUT s unless confirmed
#   confirm  cancel the automatic revert
#   stop     tear down and return to the normal setup
#
# Config file (default ~/.config/dn-ap.conf of the sudo user, or DN_AP_CONF):
#   SSID=...   PSK=... (8-63 chars)   CHAN=1..13
# The values are never printed.

STICK=${STICK:-wlan1}
APIF=${APIF:-wlan0}
ETH=${ETH:-eth0}
BR=${BR:-br0}
TIMEOUT=${TIMEOUT:-180}

C_STICK=dn-stick
C_BR=dn-br0
C_ETH=dn-br0-eth0
C_AP=dn-ap
TIMER=dn-ap-deadman
STATE=/run/dn-ap.state
LOG=/run/dn-ap.log
SELF=$(readlink -f "$0")

log() { printf '%s %s\n' "$(date +%T)" "$*" | tee -a "$LOG"; }
die() { log "ERROR: $*"; exit 1; }

[ "$(id -u)" = 0 ] || exec sudo -- "$SELF" "$@"

online_via() {   # interface -> 0 if the internet is reachable through it
	local code
	code=$(curl --interface "$1" -sS -m 10 -o /dev/null -w '%{http_code}' \
		https://api.anthropic.com/ 2>/dev/null)
	[ -n "$code" ] && [ "$code" != 000 ]
}

con_exists() { nmcli -t -f NAME connection show | grep -qxF "$1"; }
con_drop()   { con_exists "$1" && nmcli connection delete "$1" >/dev/null 2>&1; }

read_conf() {
	local home
	home=$(getent passwd "${SUDO_USER:-root}" | cut -d: -f6)
	CONF=${DN_AP_CONF:-$home/.config/dn-ap.conf}
	[ -r "$CONF" ] || die "config $CONF not readable"
	SSID=$(grep -m1 '^SSID=' "$CONF" | cut -d= -f2-)
	PSK=$(grep -m1 '^PSK=' "$CONF" | cut -d= -f2-)
	CHAN=$(grep -m1 '^CHAN=' "$CONF" | cut -d= -f2- | tr -d ' ')
	[ -n "$SSID" ] && [ ${#SSID} -le 32 ] || die "SSID missing or longer than 32"
	[ ${#PSK} -ge 8 ] && [ ${#PSK} -le 63 ] || die "PSK must have 8-63 characters"
	case $CHAN in [1-9]|1[0-3]) ;; *) die "CHAN must be 1..13" ;; esac
}

disarm() { systemctl stop "$TIMER.timer" >/dev/null 2>&1; }

do_stop() {
	[ "${1:-}" = deadman ] && log "dead-man timer expired - reverting" || disarm
	local orig_ap orig_eth
	orig_ap=$(sed -n 's/^ORIG_AP=//p' "$STATE" 2>/dev/null)
	orig_eth=$(sed -n 's/^ORIG_ETH=//p' "$STATE" 2>/dev/null)
	orig_ap=${orig_ap:-TUTIS}

	log "removing AP and bridge"
	con_drop "$C_AP"; con_drop "$C_ETH"; con_drop "$C_BR"
	ip link show "$BR" >/dev/null 2>&1 && ip link delete "$BR" 2>/dev/null

	if [ -n "$orig_eth" ] && con_exists "$orig_eth"; then
		nmcli connection up "$orig_eth" ifname "$ETH" >/dev/null 2>&1 &
	fi

	log "reconnecting '$orig_ap' on $APIF"
	if nmcli --wait 45 connection up "$orig_ap" ifname "$APIF" >/dev/null 2>&1 \
	   && online_via "$APIF"; then
		log "online via $APIF again, removing $C_STICK"
		con_drop "$C_STICK"
		rm -f "$STATE"
		log "stop done - normal setup restored"
	else
		log "WARNING: $APIF not back online, keeping $C_STICK on $STICK"
		log "(reboot restores everything)"
	fi
}

do_start() {
	nmcli -t -f NAME connection show --active | grep -qxF "$C_AP" \
		&& die "already running (use stop first)"
	ip link show "$STICK" >/dev/null 2>&1 || die "$STICK not present - stick plugged in at the back?"
	read_conf
	: > "$LOG"

	local orig_ap orig_eth
	orig_ap=$(nmcli -t -f NAME,DEVICE connection show --active | sed -n "s/:$APIF\$//p" | head -1)
	orig_eth=$(nmcli -t -f NAME,DEVICE connection show --active | sed -n "s/:$ETH\$//p" | head -1)
	[ -n "$orig_ap" ] || die "nothing active on $APIF - don't know which WLAN to clone"
	printf 'ORIG_AP=%s\nORIG_ETH=%s\n' "$orig_ap" "$orig_eth" > "$STATE"

	# 1. stick joins the house WLAN; only continue if we are online through it
	log "step 1: $STICK joins '$orig_ap' (temporary clone $C_STICK)"
	con_drop "$C_STICK"
	nmcli connection clone --temporary "$orig_ap" "$C_STICK" >/dev/null \
		|| die "clone of '$orig_ap' failed"
	nmcli connection modify --temporary "$C_STICK" \
		connection.interface-name "$STICK" connection.autoconnect no \
		|| { con_drop "$C_STICK"; die "modify $C_STICK failed"; }
	if ! nmcli --wait 45 connection up "$C_STICK" ifname "$STICK" >/dev/null; then
		con_drop "$C_STICK"; rm -f "$STATE"
		die "$STICK could not connect - nothing else changed"
	fi
	if ! online_via "$STICK"; then
		con_drop "$C_STICK"; rm -f "$STATE"
		die "no internet via $STICK - nothing else changed"
	fi
	log "online via $STICK"

	# 2. dead-man: from here on, revert automatically unless confirmed
	systemctl stop "$TIMER.timer" "$TIMER.service" >/dev/null 2>&1
	systemctl reset-failed "$TIMER.service" "$TIMER.timer" >/dev/null 2>&1
	systemd-run --quiet --unit="$TIMER" --on-active="$TIMEOUT" "$SELF" stop deadman \
		|| { do_stop; die "could not arm dead-man timer - reverted"; }
	log "dead-man armed: automatic revert in ${TIMEOUT}s unless 'confirm'"

	# 3. bridge with eth0
	log "step 2: bridge $BR with $ETH"
	nmcli connection add type bridge con-name "$C_BR" ifname "$BR" \
		bridge.stp no bridge.multicast-snooping no \
		ipv4.method disabled ipv6.method disabled \
		connection.autoconnect no save no >/dev/null \
	&& nmcli connection add type ethernet con-name "$C_ETH" ifname "$ETH" \
		master "$BR" slave-type bridge connection.autoconnect no save no >/dev/null \
	&& nmcli --wait 30 connection up "$C_BR" >/dev/null \
	&& nmcli --wait 30 connection up "$C_ETH" >/dev/null \
		|| { log "bridge setup failed"; do_stop; exit 1; }

	# 4. internal WLAN as access point, port of the bridge
	log "step 3: $APIF as AP on channel $CHAN"
	nmcli connection add type wifi con-name "$C_AP" ifname "$APIF" ssid "$SSID" \
		master "$BR" slave-type bridge \
		802-11-wireless.mode ap 802-11-wireless.band bg \
		802-11-wireless.channel "$CHAN" 802-11-wireless.powersave 2 \
		wifi-sec.key-mgmt wpa-psk wifi-sec.proto rsn \
		wifi-sec.pairwise ccmp wifi-sec.group ccmp wifi-sec.pmf 1 \
		connection.autoconnect no save no >/dev/null \
	&& nmcli --wait 45 connection up "$C_AP" passwd-file \
		<(printf '802-11-wireless-security.psk:%s\n' "$PSK") >/dev/null \
		|| { log "AP setup failed"; do_stop; exit 1; }

	online_via "$STICK" && log "still online via $STICK" \
		|| log "WARNING: internet via $STICK lost - dead-man will revert"
	log "AP is up. Run '$0 confirm' within ${TIMEOUT}s to keep it."
	do_status
}

do_status() {
	echo "--- devices"
	nmcli -f DEVICE,TYPE,STATE,CONNECTION device status
	echo "--- bridge ports"
	bridge link show 2>/dev/null | grep -E "master $BR" || echo "(no bridge)"
	echo "--- $APIF"
	iw dev "$APIF" info 2>/dev/null | grep -E 'type|channel'
	iw dev "$APIF" station dump 2>/dev/null | grep -c '^Station' | sed 's/^/stations: /'
	echo "--- dead-man"
	systemctl list-timers "$TIMER.timer" --no-legend 2>/dev/null | grep . || echo "(not armed)"
}

case ${1:-} in
start)   do_start ;;
confirm) disarm; log "dead-man cancelled - AP stays until 'stop' or reboot" ;;
stop)    do_stop "${2:-}" ;;
status)  do_status ;;
*)       echo "usage: $0 start|confirm|stop|status" >&2; exit 2 ;;
esac
