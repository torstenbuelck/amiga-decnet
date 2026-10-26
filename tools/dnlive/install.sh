#!/bin/sh
# install.sh - installs dnlive on a Raspberry Pi OS (Bookworm) desktop:
#   /opt/dnlive               the service and its page
#   dnlive.service            starts at boot, serves http://127.0.0.1:8023/
#   ~/.config/autostart/      the page full screen after login
# Usage, in the unpacked folder:  sudo ./install.sh [--lan IF] [--wan IF]
#                                 sudo ./install.sh --remove
set -e
[ "$(id -u)" = 0 ] || { echo "run it with sudo"; exit 1; }
USER_HOME=$(getent passwd "${SUDO_USER:-pi}" | cut -d: -f6)
AUTOSTART="$USER_HOME/.config/autostart/dnlive.desktop"

if [ "$1" = "--remove" ]; then
	systemctl disable --now dnlive.service 2>/dev/null || true
	rm -f /etc/systemd/system/dnlive.service "$AUTOSTART"
	rm -rf /opt/dnlive
	systemctl daemon-reload
	echo "dnlive removed (log out and in again to end the browser)"
	exit 0
fi

HERE=$(dirname "$(readlink -f "$0")")
install -d /opt/dnlive
install -m 755 "$HERE/dnlive.py" "$HERE/kiosk.sh" /opt/dnlive/
install -m 644 "$HERE/index.html" "$HERE/nodes" /opt/dnlive/
sed "s|^ExecStart=.*|ExecStart=/usr/bin/python3 /opt/dnlive/dnlive.py $*|" \
	"$HERE/dnlive.service" > /etc/systemd/system/dnlive.service
systemctl daemon-reload
systemctl enable dnlive.service
systemctl restart dnlive.service

install -d -o "${SUDO_USER:-pi}" "$(dirname "$AUTOSTART")"
cat > "$AUTOSTART" <<EOD
[Desktop Entry]
Type=Application
Name=DECnet live
Exec=/opt/dnlive/kiosk.sh
EOD
chown "${SUDO_USER:-pi}" "$AUTOSTART"

# keep the screen on (also under Wayland)
if command -v raspi-config >/dev/null; then
	raspi-config nonint do_blanking 1 || true
fi
echo "dnlive installed: service running, page full screen after the next login"
echo "test now: http://127.0.0.1:8023/ in the browser"
