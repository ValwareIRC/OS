#!/usr/bin/env bash
# Builds tests/icon_sheet.c on the host and writes the icon contact sheet
# as a PNG (default build/icon-sheet.png).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/icon-sheet.png}
mkdir -p "$ROOT/build"
gcc -std=gnu17 -O2 -Wall -Wextra -o "$ROOT/build/icon-sheet" "$ROOT/tests/icon_sheet.c" \
    "$ROOT/userland/shell/iconart.c" "$ROOT/userland/shell/gfx.c" -lm
"$ROOT/build/icon-sheet" > "$ROOT/build/icon-sheet.ppm"
python3 -I - "$ROOT/build/icon-sheet.ppm" "$OUT" <<'PY'
import sys, zlib, struct
data = open(sys.argv[1], "rb").read()
parts = data.split(b"\n", 3)
w, h = map(int, parts[1].split())
px = parts[3]
raw = b"".join(b"\0" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, d):
    return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
png += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
open(sys.argv[2], "wb").write(png)
PY
echo "wrote $OUT"
