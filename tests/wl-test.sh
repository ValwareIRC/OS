#!/usr/bin/env bash
# Runs the OS's Wayland compositor natively with real clients (from Ubuntu's
# weston package) and saves what their windows showed.
#   tests/wl-test.sh [client ...]   (default: weston-simple-shm)
set -e
cd "$(dirname "$0")/.."
OUT=build/wl-test
mkdir -p "$OUT"
gcc -O1 -g -std=gnu17 -Wall -Wextra -o "$OUT/wl_test" tests/wl_test.c userland/shell/wl/server.c userland/shell/wl/comp.c
RT=$(mktemp -d)
"$OUT/wl_test" "$RT" 6 "$OUT/view" 2> "$OUT/log.txt" &
SERVER=$!
sleep 0.5
CLIENTS=${*:-weston-simple-shm}
for c in $CLIENTS; do
    XDG_RUNTIME_DIR=$RT WAYLAND_DISPLAY=wayland-0 timeout 9 $c > "$OUT/$(basename $c).out" 2>&1 &
done
wait $SERVER || true
sleep 0.3
cat "$OUT/log.txt"
for f in "$OUT"/view-*.ppm; do [ -f "$f" ] && python3 -c "
import sys,zlib,struct
d=open(sys.argv[1],'rb').read().split(b'\n',3); w,h=map(int,d[1].split()); px=d[3]
raw=b''.join(b'\x00'+px[y*w*3:(y+1)*w*3] for y in range(h))
ch=lambda t,b: struct.pack('>I',len(b))+t+b+struct.pack('>I',zlib.crc32(t+b))
open(sys.argv[1][:-4]+'.png','wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
" "$f"; done
ls "$OUT"
