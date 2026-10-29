#!/usr/bin/env bash
# Builds build/sysroot: real Linux programs for the OS, taken from this Ubuntu
# with exactly the shared libraries they load (glibc's dynamic loader
# included), plus the data they read (fonts, fontconfig, cursors, Python's
# standard library). The Makefile packs it into the apps module.
#
#   scripts/build-sysroot.sh [OUT]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${1:-$ROOT/build/sysroot}

# Wayland demo clients, a Wayland terminal, Python, and bash for the
# Terminal app (history, completion, line editing).
PROGRAMS="weston-simple-shm weston-flower weston-smoke weston-clickdot weston-editor weston-terminal
          weston-image python3 bash"

rm -rf "$OUT"
mkdir -p "$OUT/usr/bin" "$OUT/lib64" "$OUT/etc"

copy_file() { # copy a file (following symlinks) to the same path under OUT
    local src=$1 dst="$OUT$1"
    [ -e "$dst" ] && return
    mkdir -p "$(dirname "$dst")"
    cp -L "$src" "$dst"
}

copy_libs() { # every library a binary loads, by the paths ld.so would use
    ldd "$1" 2>/dev/null | awk '/=> \// {print $3} /^\t\/lib64/ {print $1}' | while read -r lib; do
        case "$lib" in /mnt/*|$ROOT/*) continue ;; esac  # bundled next to the program already
        copy_file "$lib"
        # also under the merged /usr path, which the loader may search
        case "$lib" in /lib/*) copy_file "$lib" ;; esac
    done
}

for p in $PROGRAMS; do
    bin=$(command -v "$p" || true)
    if [ -z "$bin" ]; then
        echo "sysroot: $p not installed, skipped" >&2
        continue
    fi
    cp -L "$bin" "$OUT/usr/bin/$p"
    copy_libs "$bin"
done

# libraries opened at run time (dlopen), not listed by ldd
for lib in /usr/lib/x86_64-linux-gnu/libwayland-cursor.so.0 /usr/lib/x86_64-linux-gnu/libnss_files.so.2; do
    [ -e "$lib" ] && copy_file "$lib" && copy_libs "$lib"
done

# the terminal descriptions bash's line editing reads for the Terminal app
for ti in /usr/share/terminfo/x/xterm-256color /lib/terminfo/x/xterm-256color /usr/share/terminfo/x/xterm \
          /lib/terminfo/x/xterm /lib/terminfo/d/dumb /usr/share/terminfo/d/dumb; do
    [ -e "$ti" ] && copy_file "$ti"
done

# the dynamic loader, where binaries ask for it
cp -L /lib64/ld-linux-x86-64.so.2 "$OUT/lib64/ld-linux-x86-64.so.2"

# Python's standard library, without tests and GUI toolkits it can't use
PY=$(python3 -c 'import sys; print(f"python{sys.version_info[0]}.{sys.version_info[1]}")')
mkdir -p "$OUT/usr/lib/$PY"
(cd /usr/lib/$PY && tar cf - --exclude=test --exclude=tests --exclude=idlelib --exclude=tkinter \
    --exclude=turtledemo --exclude=__pycache__ --exclude=ensurepip --exclude=lib2to3 --exclude=config-* . ) |
    (cd "$OUT/usr/lib/$PY" && tar xf -)
for so in "$OUT/usr/lib/$PY"/lib-dynload/*.so; do copy_libs "$so"; done

# fonts and fontconfig, cursors, weston's pictures
mkdir -p "$OUT/usr/share/fonts/truetype"
cp -rL /usr/share/fonts/truetype/dejavu "$OUT/usr/share/fonts/truetype/"
cp -rL /etc/fonts "$OUT/etc/"
[ -d /usr/share/weston ] && cp -rL /usr/share/weston "$OUT/usr/share/"
# Each program's .desktop file and PNG icons, so the shell can show the
# app's own icon. Weston's demos have none, but ship pictures of themselves.
mkdir -p "$OUT/usr/share/applications"
for p in $PROGRAMS; do
    d=/usr/share/applications/$p.desktop
    [ -f "$d" ] || continue
    cp -L "$d" "$OUT/usr/share/applications/"
    icon=$(sed -n 's/^Icon=//p' "$d" | head -1)
    [ -n "$icon" ] || continue
    for f in /usr/share/icons/hicolor/*/apps/"$icon".png /usr/share/pixmaps/"$icon".png; do
        if [ -f "$f" ]; then copy_file "$f"; fi
    done
done
for demo in flower:Flower smoke:Smoke; do
    id=${demo%%:*} name=${demo#*:}
    [ -f "/usr/share/weston/icon_ivi_$id.png" ] || continue
    printf '[Desktop Entry]\nType=Application\nName=%s\nExec=weston-%s\nIcon=/usr/share/weston/icon_ivi_%s.png\n' \
        "$name" "$id" "$id" > "$OUT/usr/share/applications/weston-$id.desktop"
done
if [ -d /usr/share/icons/Adwaita/cursors ]; then
    mkdir -p "$OUT/usr/share/icons/default"
    cp -rL /usr/share/icons/Adwaita/cursors "$OUT/usr/share/icons/default/"
fi

# the companion's ears and voice (ports/build-voice.sh), if built
VOICE=$ROOT/build/voice
if [ -x "$VOICE/whisper-cli" ]; then
    cp "$VOICE/whisper-cli" "$OUT/usr/bin/"
    copy_libs "$VOICE/whisper-cli"
    mkdir -p "$OUT/usr/share/os/voice"
    cp "$VOICE"/ggml-*.bin "$VOICE"/*.onnx "$VOICE"/*.onnx.json "$OUT/usr/share/os/voice/" 2>/dev/null || true
    if [ -d "$VOICE/piper" ]; then
        mkdir -p "$OUT/opt"
        cp -a "$VOICE/piper" "$OUT/opt/piper"
        rm -rf "$OUT/opt/piper/pkgconfig"
        copy_libs "$VOICE/piper/piper"
        for so in "$VOICE"/piper/lib*.so.*; do copy_libs "$so"; done
        # Piper finds its libraries and voice data next to itself through
        # /proc/self/exe; until the OS has /proc, say where they are.
        cat > "$OUT/usr/bin/piper" <<'WRAP'
#!/bin/sh
LD_LIBRARY_PATH=/opt/piper exec /opt/piper/piper --espeak_data /opt/piper/espeak-ng-data "$@"
WRAP
        chmod +x "$OUT/usr/bin/piper"
    fi
fi

# The OS's own offline mind (ports/build-mind.sh): the server; the model
# itself is a boot module, loaded in place (see the Makefile's iso target)
if [ -x "$ROOT/build/mind/llama-server" ]; then
    cp "$ROOT/build/mind/llama-server" "$OUT/usr/bin/"
    copy_libs "$ROOT/build/mind/llama-server"
fi

# the Web app (NetSurf): scripts/sysroot-browser.sh
. "$ROOT/scripts/sysroot-browser.sh"

# so the loader finds everything without searching
ldconfig -r "$OUT" 2>/dev/null || true
cat > "$OUT/etc/ld.so.conf" <<'EOF'
/usr/lib/x86_64-linux-gnu
/lib/x86_64-linux-gnu
EOF
ldconfig -r "$OUT" -f /etc/ld.so.conf 2>/dev/null || true

echo "sysroot: $(du -sh "$OUT" | cut -f1) in $OUT"
