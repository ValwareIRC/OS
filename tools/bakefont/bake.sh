#!/bin/sh
# Regenerates kernel/third_party/bootfont.h: Inter for the kernel's boot screen, at the sizes it uses.
# Needs the fonts (build/ports/fonts, from the ports build) and stb_truetype (build/ports/include).
set -e
cd "$(dirname "$0")/../.."
FONTS=build/ports/fonts
mkdir -p build/tools
cc -O2 -Ibuild/ports/include -o build/tools/bakefont tools/bakefont/bakefont.c -lm
OUT=kernel/third_party/bootfont.h
{
    echo "/* The boot screen's type: Inter (SIL Open Font License 1.1, Copyright 2016 The Inter Project Authors),"
    echo " * pre-rendered by tools/bakefont/bake.sh. */"
    echo "#pragma once"
    echo "#include <stdint.h>"
    echo "struct bootglyph { uint32_t off; uint8_t w, h; int8_t x, y; uint16_t adv; };"
    echo "struct bootfont { const unsigned char *bits; const struct bootglyph *g; int ascent, line; };"
    build/tools/bakefont $FONTS/Inter.ttf 15 bf_small
    build/tools/bakefont $FONTS/Inter.ttf 13 bf_hint
    build/tools/bakefont $FONTS/Inter-Medium.ttf 28 bf_title
} > $OUT
echo "wrote $OUT ($(wc -c < $OUT) bytes)"
