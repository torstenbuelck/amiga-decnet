#!/bin/sh
# kiosk.sh - shows the dnlive page full screen (desktop autostart).
# Waits for the service, keeps the screen on, restarts the browser if
# it ends.
URL=http://127.0.0.1:8023/
for i in $(seq 60); do
	curl -s -o /dev/null "$URL" && break
	sleep 2
done
# screen saver and power saving off (X11; under Wayland install.sh
# switches blanking off with raspi-config)
if [ -n "$DISPLAY" ] && command -v xset >/dev/null; then
	xset s off -dpms s noblank 2>/dev/null
fi
BROWSER=$(command -v chromium-browser || command -v chromium)
while true; do
	"$BROWSER" --kiosk --incognito --noerrdialogs --disable-infobars \
		--no-first-run --disable-features=Translate \
		--check-for-update-interval=31536000 --password-store=basic "$URL"
	sleep 5
done
