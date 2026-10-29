#!/bin/sh
# Run on the desktop as the person (fw_cfg opt/os/autorun.sh, tests/portals_check.py):
# calls each portal from inside an app's sandbox, as apps do.
sleep 15
P="flatpak run --command=gdbus com.valware.prism"
D="--session --dest org.freedesktop.portal.Desktop --object-path /org/freedesktop/portal/desktop"
echo "PORTALCHECK settings: $($P call $D --method org.freedesktop.portal.Settings.ReadOne org.freedesktop.appearance color-scheme 2>&1 | tail -1)"
echo "PORTALCHECK notify: $($P call $D --method org.freedesktop.portal.Notification.AddNotification hello "{'title': <'Hello from a sandbox'>, 'body': <'Sent through the notification portal'>}" 2>&1 | tail -1)"
sleep 2
echo "PORTALCHECK openuri: $($P call $D --method org.freedesktop.portal.OpenURI.OpenURI '' 'https://example.com' '{}' 2>&1 | tail -1)"
echo "PULSECHECK $(pactl list short sinks 2>&1 | tr '\n' ' ')"
echo "PORTALCHECK done"
# once DOSBox is installed (the test installs it from the launcher): run it with its own words on the console
for i in $(seq 1 120); do [ -e /var/lib/flatpak/exports/share/applications/com.dosbox.DOSBox.desktop ] && break; sleep 5; done
sleep 20
echo "DOSBOXRUN start"
SDL_DEBUG=1 timeout 40 flatpak run com.dosbox.DOSBox 2>&1 | grep -v "document portal\|fuse" | head -40
echo "DOSBOXRUN end"
