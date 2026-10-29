#!/usr/bin/env bash
# Copies every shared library a program loads (as this machine resolves
# them) into ROOT under the same paths, so it runs inside the OS too.
# Libraries that live next to the program (in APPDIR) are left where they are.
#   scripts/bundle-libs.sh ROOT APPDIR PROGRAM...
set -euo pipefail
ROOT=$1 APPDIR=$2; shift 2
for prog in "$@"; do
    LD_LIBRARY_PATH="$APPDIR/usr/lib:$APPDIR" ldd "$prog" 2>/dev/null |
        awk '/=> \// {print $3} /^\t\/lib64/ {print $1}' | while read -r lib; do
            case "$lib" in "$APPDIR"/*) continue ;; esac
            [ -e "$ROOT$lib" ] && continue
            mkdir -p "$ROOT$(dirname "$lib")"
            cp -L "$lib" "$ROOT$lib"
        done
done
