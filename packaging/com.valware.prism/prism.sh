#!/bin/sh
# Starts Prism through zypak, so Chromium's sandbox is a Flatpak one.
export TMPDIR="${XDG_RUNTIME_DIR:-/tmp}/app/${FLATPAK_ID:-com.valware.prism}"
mkdir -p "$TMPDIR"
# No GPU here: draw in software instead of trying one and failing.
[ -e /dev/dri ] || set -- --disable-gpu --disable-gpu-compositing "$@"
# Wayland when there is a Wayland display (Electron's "auto" also wants XDG_SESSION_TYPE)
[ -n "$WAYLAND_DISPLAY" ] && set -- --ozone-platform=wayland "$@"
exec zypak-wrapper /app/prism/prism "$@"
