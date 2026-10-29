#!/usr/bin/env bash
# Builds and runs tests/fm_dump.c natively (gcc + the engine + renderer).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
S=userland/shell
musl-gcc -static -O1 -g -std=gnu17 -w -Ibuild/ports/include -Ibuild/ports/include/lua -o build/host/fm_dump tests/fm_dump.c $S/fm/*.c $S/gfx.c $S/font.c $S/text.c $S/iconart.c userland/lib/json.c -Lbuild/ports/lib -llua -lm
build/host/fm_dump "$@"
