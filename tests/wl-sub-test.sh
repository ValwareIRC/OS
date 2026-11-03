#!/usr/bin/env bash
# Subsurfaces in the compositor with a real Wayland app (tests/wl_sub_client.c): their places, their order (one
# below its parent, so hidden), synchronized frames waiting for the parent, and the window geometry crop.
# Native, on the build machine.   tests/wl-sub-test.sh
set -e
cd "$(dirname "$0")/.."
OUT=build/wl-sub-test
mkdir -p "$OUT"
rm -f "$OUT"/view-*.ppm
wayland-scanner client-header /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml "$OUT/xdg-shell-client-protocol.h"
wayland-scanner private-code /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml "$OUT/xdg-shell-protocol.c"
gcc -O1 -g -std=gnu17 -Wall -Wextra -fsanitize=address,undefined -o "$OUT/wl_test" tests/wl_test.c userland/shell/wl/server.c userland/shell/wl/comp.c
gcc -O1 -g -std=gnu17 -I"$OUT" -o "$OUT/sub_client" tests/wl_sub_client.c "$OUT/xdg-shell-protocol.c" -lwayland-client
RT=$(mktemp -d)
"$OUT/wl_test" "$RT" 4 "$OUT/view" 2> "$OUT/log.txt" &
SERVER=$!
sleep 0.5
XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=wayland-0 timeout 6 "$OUT/sub_client" > "$OUT/client.txt" 2>&1 || true
wait $SERVER || true
cat "$OUT/client.txt"
grep -E "SAVED|ERROR|runtime error" "$OUT/log.txt" || true
python3 - "$OUT"/view-*.ppm <<'PY'
import sys
d = open(sys.argv[1], 'rb').read().split(b'\n', 3)
w, h = map(int, d[1].split()); px = d[3]
at = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
# the view is the window geometry (10, 10, 200x150): surface coordinates minus 10
checks = {"the window geometry's size": (w, h) == (200, 150),
          "the window's own red": at(5, 5) == (255, 0, 0),
          "A (blue) at its place": at(25, 35) == (0, 0, 255),
          "B (green) at its place": at(85, 85) == (0, 255, 0),
          "B over A where they meet": at(65, 55) == (0, 255, 0),
          "A's new frame waits for the parent": at(22, 32) == (0, 0, 255),
          "C, below the opaque parent, hidden": at(12, 12) == (255, 0, 0)}
bad = [k for k, v in checks.items() if not v]
for k, v in checks.items():
    print(("ok   " if v else "FAIL ") + k)
print("subsurfaces: all ok" if not bad else "subsurfaces: FAILED")
sys.exit(1 if bad else 0)
PY
