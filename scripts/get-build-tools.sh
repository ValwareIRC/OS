#!/usr/bin/env bash
# Build machine tools the OS build needs but this Ubuntu (WSL) doesn't have,
# fetched without root: the packages are downloaded and unpacked into
# ~/ostools/root, and scripts/ostools-env.sh puts them on the PATH. They
# only build things and fetch the Freedesktop runtime; nothing from here
# goes into the OS.
#
#   scripts/get-build-tools.sh
set -euo pipefail
TOOLS=${OSTOOLS:-$HOME/ostools}
WANT="flatpak flatpak-builder meson ninja-build libostree-dev ostree"
mkdir -p "$TOOLS/debs" "$TOOLS/root"
cd "$TOOLS/debs"

# Every package needed, minus the ones this system already has installed.
need=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks --no-replaces \
          --no-enhances $WANT 2>/dev/null | grep -E '^\w' | sort -u)
missing=()
for p in $need; do
    dpkg -s "$p" >/dev/null 2>&1 || missing+=("$p")
done
echo "get-build-tools: ${#missing[@]} packages to fetch"
for p in "${missing[@]}"; do
    ls "${p}"_*.deb >/dev/null 2>&1 || apt-get download "$p" >/dev/null 2>&1 || echo "  (no package $p)"
done
for d in *.deb; do
    dpkg -x "$d" "$TOOLS/root"
done
echo "get-build-tools: unpacked into $TOOLS/root"
