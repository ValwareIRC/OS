#!/bin/sh
# Starts Prism (the Flatpak) with zypak's and Chromium's launch logging, prints
# that log after 75 s (window or not), then stops it; three times
# (tests/prism_trace.py).
sleep 10
for n in 1 2 3; do
    echo "PRISMTRACE attempt $n"
    flatpak run --env=ZYPAK_DEBUG=1 com.valware.prism --enable-logging=stderr --v=0 \
        '--vmodule=*zygote*=2,*child_process_launcher*=2,*render_process_host*=2,*renderer_main*=2,*render_thread_impl*=2,*child_thread_impl*=2,*child_process*=2' \
        >/tmp/prism-trace-$n.log 2>&1 &
    sleep 75
    grep -v "document portal\|fuse\|inotify\|udev\|Failed to connect to the bus\|drm\|wp_\|Server doesn" /tmp/prism-trace-$n.log | cut -c1-220 | head -200
    echo "PRISMTRACE end $n"
    timeout 10 flatpak kill com.valware.prism
    for p in $(pgrep -f /app/prism/prism); do kill -9 $p; done
    sleep 5
done
echo "PRISMTRACE done"
