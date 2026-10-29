#!/bin/sh
# One Prism start, then its own start-up log (Prism 0.24.7+ logs boot to stderr) on the console
sleep 10
echo "PRISMONCE start"
PRISM_BOOT_LOG=1 flatpak run --env=PRISM_BOOT_LOG=1 com.valware.prism >/tmp/prism-once.log 2>&1 &
sleep 120
echo "PRISMBOOT begin"
grep -v "document portal\|fuse\|inotify\|udev\|Failed to connect to the bus\|drm\|wp_\|Server doesn\|zypak\|GLib" /tmp/prism-once.log | cut -c1-200 | tail -90
echo "PRISMBOOT end"
echo "PRISMONCE end"
