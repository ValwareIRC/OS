#!/usr/bin/env bash
# The one model that comes with the system: Qwen3 0.6B (Apache-2.0), 8-bit,
# 640 MB. Small and simple on purpose: it is there for when there is no
# internet; anything better is downloaded from inside the OS (Settings, or
# during onboarding). `make iso` packs build/mind/builtin.gguf when present.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/mind/builtin.gguf
URL=https://huggingface.co/Qwen/Qwen3-0.6B-GGUF/resolve/main/Qwen3-0.6B-Q8_0.gguf
SIZE=639446688
mkdir -p "$(dirname "$OUT")"
if [ -s "$OUT" ] && [ "$(stat -c %s "$OUT")" = "$SIZE" ]; then
    echo "builtin model: already here"
    exit 0
fi
curl -fL --retry 3 -o "$OUT.part" "$URL"
[ "$(stat -c %s "$OUT.part")" = "$SIZE" ] || { echo "builtin model: wrong size"; exit 1; }
mv "$OUT.part" "$OUT"
echo "builtin model: $OUT"
