#!/usr/bin/env bash
# Prints the design lint for a Facet document, as the AI would be told it,
# and with OUT_DIR saves the pictures a seeing model would be shown.
#   tests/lint_design.sh DESIGN.xml [OUT_DIR]
# Builds tests/fm_shot.c natively with the real engine, wallpaper and icons.
# Exit status: 0 clean, 1 findings (or it doesn't parse).
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
S=userland/shell
SRCS="tests/fm_shot.c $S/fm/*.c $S/gfx.c $S/font.c $S/text.c $S/iconart.c $S/design.c $S/wallpaper.c $S/icons.c $S/ui.c $S/picture.c
      $S/stylist_look.c userland/lib/json.c"
# rebuilt only when a source is newer than the tool
if [ ! -x build/host/fm_shot ] || [ -n "$(find $SRCS $S/*.h $S/fm/*.h $S/*.inc -newer build/host/fm_shot 2>/dev/null)" ]; then
    musl-gcc -static -O2 -g -std=gnu17 -w -Ibuild/ports/include -Ibuild/ports/include/lua -o build/host/fm_shot \
        $SRCS -Lbuild/ports/lib -llua -lm
fi
set +e
export OS_THEMES="${OS_THEMES:-$PWD/userland/rootfs/usr/share/os/themes}"
[ -z "$2" ] || mkdir -p "$2"
build/host/fm_shot "$@"
