#!/usr/bin/env bash
# The compositor's clipboard with a real Wayland app (tests/wl_clip_client.c):
# the app must paste the OS's clipboard, and what it copies must become the
# OS's clipboard. Native, on the build machine.
#   tests/wl-clip-test.sh
set -e
cd "$(dirname "$0")/.."
OUT=build/wl-clip-test
mkdir -p "$OUT"
wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml "$OUT/xdg-shell-client-protocol.h"
wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml "$OUT/xdg-shell-protocol.c"
gcc -O1 -g -std=gnu17 -Wall -Wextra -o "$OUT/wl_test" tests/wl_test.c userland/shell/wl/server.c userland/shell/wl/comp.c
gcc -O1 -g -std=gnu17 -I"$OUT" -o "$OUT/clip_client" tests/wl_clip_client.c "$OUT/xdg-shell-protocol.c" -lwayland-client
RT=$(mktemp -d)
"$OUT/wl_test" "$RT" 6 "$OUT/view" 2> "$OUT/log.txt" &
SERVER=$!
sleep 0.5
XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=wayland-0 timeout 9 "$OUT/clip_client" > "$OUT/client.txt" 2>&1 || true
wait $SERVER || true
cat "$OUT/client.txt"
grep -E "clipboard:|POKED" "$OUT/log.txt" || true
grep -q "PASTE: hello from the OS" "$OUT/client.txt" && grep -q 'clipboard: now "copied in an app"' "$OUT/log.txt" &&
    echo "clipboard: both ways" || { echo "clipboard: FAILED"; exit 1; }
