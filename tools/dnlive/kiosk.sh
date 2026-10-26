#!/bin/sh
# kiosk.sh - shows the dnlive page full screen (desktop autostart).
# Waits for the service, keeps the screen on, opens the browser again
# if it crashes (not after Esc or Ctrl+W).
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
BROWSER=${DNLIVE_BROWSER:-$(command -v chromium-browser || command -v chromium)}
# Esc on the page (dnlive answers /quit-pending once with "yes") or
# Ctrl+W (the browser ends with exit code 0): stay closed; after a
# crash (any other code) open it again
while true; do
	"$BROWSER" --kiosk --incognito --noerrdialogs --disable-infobars \
		--no-first-run --disable-features=Translate \
		--check-for-update-interval=31536000 --password-store=basic "$URL" &
	PID=$!
	QUIT=0
	while kill -0 $PID 2>/dev/null; do
		sleep 1
		if [ "$(curl -s "${URL}quit-pending")" = yes ]; then
			QUIT=1
			kill $PID 2>/dev/null
			pkill -f 'chromium.*--kiosk' 2>/dev/null
		fi
	done
	wait $PID
	RC=$?
	if [ $QUIT = 1 ] || [ $RC = 0 ]; then
		break
	fi
	sleep 5
done
