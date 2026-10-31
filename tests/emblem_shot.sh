#!/usr/bin/env bash
# Draws every emblem in each state with the real engine, on the build machine.
#   tests/emblem_shot.sh [OUT.png]
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
S=userland/shell
musl-gcc -static -O2 -g -std=gnu17 -w -Ibuild/ports/include -Ibuild/ports/include/lua -o build/host/emblem_shot \
    tests/emblem_shot.c $S/emblem.c $S/gfx.c $S/font.c $S/text.c $S/ui.c $S/wallpaper.c $S/design.c userland/lib/json.c \
    -Lbuild/ports/lib -llua -lm -Wl,--unresolved-symbols=ignore-all  # only the drawing runs
build/host/emblem_shot "${1:-build/host/emblems.png}"
