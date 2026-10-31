#!/usr/bin/env bash
# The one model that comes with the system: Qwen3 8B (Apache-2.0), 4-bit
# (Q4_K_M), 5.0 GB. It is there for when there is no internet: capable
# enough to talk, design and call tools (Prism's agent falls back to it),
# and it fits the minimum computer (32 GB) with room to spare. Others are
# downloaded from inside the OS (Settings, or during onboarding). `make iso`
# packs build/mind/builtin.gguf when present.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=$ROOT/build/mind/builtin.gguf
URL=https://huggingface.co/Qwen/Qwen3-8B-GGUF/resolve/main/Qwen3-8B-Q4_K_M.gguf
SIZE=5027783488 # userland/shell/models.c says the same
mkdir -p "$(dirname "$OUT")"
if [ -s "$OUT" ] && [ "$(stat -c %s "$OUT")" = "$SIZE" ]; then
    echo "builtin model: already here"
    exit 0
fi
curl -fL --retry 3 -C - -o "$OUT.part" "$URL"
[ "$(stat -c %s "$OUT.part")" = "$SIZE" ] || { echo "builtin model: wrong size"; exit 1; }
mv "$OUT.part" "$OUT"
echo "builtin model: $OUT"
