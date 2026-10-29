#!/usr/bin/env bash
# Fetches and builds third-party libraries for the OS user space with musl.
# Sources live outside the repo ($PORTS_DIR); headers and static libraries are
# installed into build/ports. Every port here must have a licence that allows
# a closed-source product (see the plan): Apache-2.0, MIT, BSD, public domain.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
PREFIX=${PREFIX:-$ROOT/build/ports}
PORTS_DIR=${PORTS_DIR:-$HOME/os-ports}
CC=${CC:-musl-gcc}
JOBS=$(nproc)
mkdir -p "$PREFIX/include" "$PREFIX/lib" "$PORTS_DIR"

fetch() { # url dir
    local url=$1 dir=$2
    [ -d "$PORTS_DIR/$dir" ] && return
    echo "fetching $url"
    (cd "$PORTS_DIR" && curl -sSL -o "$dir.tar" "$url" && tar xf "$dir.tar" && rm "$dir.tar")
}

# mbed TLS 3.6 LTS (Apache-2.0): TLS 1.2/1.3 for talking to AI providers.
MBEDTLS=mbedtls-3.6.2
if [ ! -f "$PREFIX/lib/libmbedtls.a" ]; then
    fetch https://github.com/Mbed-TLS/mbedtls/releases/download/$MBEDTLS/$MBEDTLS.tar.bz2 $MBEDTLS
    make -C "$PORTS_DIR/$MBEDTLS/library" -j"$JOBS" CC="$CC" CFLAGS="-O2 -fno-stack-protector" \
        libmbedcrypto.a libmbedx509.a libmbedtls.a >/dev/null
    cp "$PORTS_DIR/$MBEDTLS/library/"lib*.a "$PREFIX/lib/"
    cp -r "$PORTS_DIR/$MBEDTLS/include/mbedtls" "$PORTS_DIR/$MBEDTLS/include/psa" "$PREFIX/include/"
    echo "built $MBEDTLS"
fi

# Lua 5.4 (MIT): the scripting runtime the AI writes desktop surfaces in.
LUA=lua-5.4.7
if [ ! -f "$PREFIX/lib/liblua.a" ]; then
    fetch https://www.lua.org/ftp/$LUA.tar.gz $LUA
    make -s -C "$PORTS_DIR/$LUA" generic CC="$CC" MYCFLAGS="-O2" >/dev/null 2>&1
    cp "$PORTS_DIR/$LUA/src/liblua.a" "$PREFIX/lib/"
    mkdir -p "$PREFIX/include/lua"
    cp "$PORTS_DIR/$LUA/src/"{lua.h,luaconf.h,lualib.h,lauxlib.h,lua.hpp} "$PREFIX/include/lua/"
    echo "built $LUA"
fi

# stb_truetype (public domain / MIT): font rasterisation for the UI.
if [ ! -f "$PREFIX/include/stb_truetype.h" ]; then
    curl -sSL -o "$PREFIX/include/stb_truetype.h" \
        https://raw.githubusercontent.com/nothings/stb/master/stb_truetype.h
    curl -sSL -o "$PREFIX/include/stb_image.h" \
        https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
    echo "fetched stb"
fi
# stb_image_write (public domain / MIT): screenshots as PNG.
if [ ! -f "$PREFIX/include/stb_image_write.h" ]; then
    curl -sSL -o "$PREFIX/include/stb_image_write.h" \
        https://raw.githubusercontent.com/nothings/stb/master/stb_image_write.h
    echo "fetched stb_image_write"
fi

# Fonts (SIL Open Font License 1.1, from the Google Fonts repository): the
# typefaces the AI chooses between when it designs a user's desktop.
FONTS="$PREFIX/fonts"
mkdir -p "$FONTS"
base=https://github.com/google/fonts/raw/main/ofl
for spec in "inter/Inter[opsz,wght].ttf:Inter.ttf" "lora/Lora[wght].ttf:Lora.ttf" \
            "spacegrotesk/SpaceGrotesk[wght].ttf:SpaceGrotesk.ttf" \
            "jetbrainsmono/JetBrainsMono[wght].ttf:JetBrainsMono.ttf" \
            "caveat/Caveat[wght].ttf:Caveat.ttf" "outfit/Outfit[wght].ttf:Outfit.ttf" \
            "playfairdisplay/PlayfairDisplay[wght].ttf:PlayfairDisplay.ttf" \
            "fraunces/Fraunces[SOFT,WONK,opsz,wght].ttf:Fraunces.ttf" \
            "sora/Sora[wght].ttf:Sora.ttf" "unbounded/Unbounded[wght].ttf:Unbounded.ttf" \
            "manrope/Manrope[wght].ttf:Manrope.ttf" "syne/Syne[wght].ttf:Syne.ttf"; do
    src=${spec%%:*}; dst=${spec##*:}
    [ -f "$FONTS/$dst" ] && continue
    url="$base/$(echo "$src" | sed 's/\[/%5B/g; s/\]/%5D/g; s/,/%2C/g')"
    curl -sSLf -o "$FONTS/$dst" "$url" || { echo "font download failed: $src"; exit 1; }
    echo "fetched $dst"
done
[ -f "$FONTS/OFL.txt" ] || curl -sSLf -o "$FONTS/OFL.txt" "$base/inter/OFL.txt"

# Their bold and italic faces. The files above are variable fonts, but the
# shell's rasteriser draws only their default (regular) instance, so the
# other weights come as static instances from the Google Fonts API, which
# serves plain TrueType to clients that don't ask for WOFF. Each file is
# "Family-Style.ttf"; the shell reads its weight and slant from the font.
# Faces missing here (or that failed to download) are synthesised by the
# shell from the nearest one, so a failure only warns.
gfonts=https://fonts.googleapis.com/css2
for spec in "Inter:Inter:Light=0,300 Medium=0,500 SemiBold=0,600 Bold=0,700 ExtraBold=0,800 Italic=1,400 SemiBoldItalic=1,600 BoldItalic=1,700" \
            "Manrope:Manrope:SemiBold=0,600 Bold=0,700 ExtraBold=0,800" \
            "Lora:Lora:SemiBold=0,600 Bold=0,700 Italic=1,400 BoldItalic=1,700" \
            "Space Grotesk:SpaceGrotesk:SemiBold=0,600 Bold=0,700" \
            "JetBrains Mono:JetBrainsMono:Bold=0,700 Italic=1,400" \
            "Caveat:Caveat:Bold=0,700" "Outfit:Outfit:SemiBold=0,600 Bold=0,700" \
            "Playfair Display:PlayfairDisplay:Bold=0,700 Italic=1,400 BoldItalic=1,700" \
            "Fraunces:Fraunces:SemiBold=0,600 Bold=0,700 Italic=1,400 BoldItalic=1,700" \
            "Sora:Sora:SemiBold=0,600 Bold=0,700" "Unbounded:Unbounded:Bold=0,700" "Syne:Syne:Bold=0,700 ExtraBold=0,800"; do
    family=${spec%%:*}; rest=${spec#*:}; file=${rest%%:*}; styles=${rest#*:}
    for st in $styles; do
        name=${st%%=*} axes=${st#*=}
        dst="$FONTS/$file-$name.ttf"
        [ -f "$dst" ] && continue
        url=$(curl -sSLf "$gfonts?family=${family// /+}:ital,wght@$axes" | grep -o 'https://[^)]*\.ttf' | head -1) || url=
        if [ -n "$url" ] && curl -sSLf -o "$dst.part" "$url"; then
            mv "$dst.part" "$dst"
            echo "fetched $file-$name.ttf"
        else
            rm -f "$dst.part"
            echo "warning: no $family $name face; the shell will synthesise it" >&2
        fi
    done
done

echo "ports ready in $PREFIX"
