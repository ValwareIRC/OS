#!/usr/bin/env bash
# Copies an AppImage's .desktop file and its PNG icons into a root
# directory (usr/share/applications, usr/share/icons/hicolor), where the
# shell looks for each app's own icon. The image is read with unsquashfs,
# never run. Missing tools or icons only warn: the app keeps its monogram.
#
#   scripts/appimage-icon.sh APP.AppImage ROOT [ID]
set -uo pipefail
img=$1 root=$2 id=${3:-}
command -v unsquashfs >/dev/null || { echo "appimage-icon: no unsquashfs; $img keeps its monogram" >&2; exit 0; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# The filesystem starts after the runtime: try each squashfs magic in turn.
for off in $(grep -abo hsqs "$img" 2>/dev/null | head -8 | cut -d: -f1); do
    rm -rf "$tmp/x"
    unsquashfs -q -n -o "$off" -d "$tmp/x" "$img" '*.desktop' usr/share/icons >/dev/null 2>&1 || true
    ls "$tmp"/x/*.desktop >/dev/null 2>&1 && break
done
desk=$(ls "$tmp"/x/*.desktop 2>/dev/null | head -1)
if [ -z "$desk" ]; then
    echo "appimage-icon: no .desktop in $img" >&2
    exit 0
fi
mkdir -p "$root/usr/share/applications"
cp "$desk" "$root/usr/share/applications/${id:-$(basename "$desk")}${id:+.desktop}"
if [ -d "$tmp/x/usr/share/icons/hicolor" ]; then
    find "$tmp/x/usr/share/icons/hicolor" -type f ! -name '*.png' -delete # the shell reads PNGs only
    mkdir -p "$root/usr/share/icons/hicolor"
    cp -r "$tmp/x/usr/share/icons/hicolor/." "$root/usr/share/icons/hicolor/"
fi
echo "appimage-icon: $(basename "$desk") and its icons from $(basename "$img")"
