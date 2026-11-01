#!/usr/bin/env bash
# Pictures a Facet 2 app in a design, as its window would show it.
#   tests/fx_shot.sh APP.facet DESIGN.xml OUT.png [HOME]
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
S=userland/shell
musl-gcc -static -O2 -g -std=gnu17 -w -Ibuild/ports/include -Ibuild/ports/include/lua -o build/host/fx_shot \
    tests/fx_shot.c $S/fm/*.c $S/fx/*.c $S/gfx.c $S/font.c $S/text.c $S/iconart.c $S/design.c $S/wallpaper.c \
    $S/icons.c $S/ui.c $S/picture.c $S/image.c $S/stylist_look.c userland/lib/json.c -Lbuild/ports/lib -llua -lm
OS_THEMES="${OS_THEMES:-$PWD/userland/rootfs/usr/share/os/themes}" build/host/fx_shot "$@"
