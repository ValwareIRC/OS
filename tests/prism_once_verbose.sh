#!/bin/sh
# One Prism start with Chromium's verbose log; its last lines go to the console (tests/prism_ab.py verbose)
sleep 10
echo "PRISMONCE start"
flatpak run com.valware.prism --enable-logging=stderr --v=1 >/tmp/prism-once.log 2>&1 &
sleep 150
echo "PRISMLOG lines $(wc -l < /tmp/prism-once.log)"
grep -v "document portal\|fuse\|inotify\|udev\|Failed to connect to the bus\|drm\|wp_\|Server doesn\|zypak" /tmp/prism-once.log | cut -c1-200 | tail -60
echo "PRISMONCE end"
