#!/usr/bin/env bash
# Builds Prism as a Flatpak (packaging/com.valware.prism) from its Linux
# release, into a local repo and a single-file bundle the image installs.
#   scripts/build-prism-flatpak.sh [path/to/Prism-<v>.tar.gz]
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
source "$ROOT/scripts/ostools-env.sh"
# Prism's own checkout, next to this one unless PRISM_DIR says otherwise
PRISM_DIR=${PRISM_DIR:-$ROOT/../prism}
TAR=${1:-$(ls "$PRISM_DIR"/dist/Prism-*.tar.gz | sort -V | tail -1)}
W=$HOME/osbuild/prism-flatpak
REPO=$HOME/osbuild/flatpak-repo
mkdir -p "$W/src"
cp "$ROOT"/packaging/com.valware.prism/* "$W/src/"
cp "$TAR" "$W/src/Prism.tar.gz"
cp "$PRISM_DIR"/build/icon.png "$W/src/icon.png"
cd "$W"
flatpak-builder --user --force-clean --disable-rofiles-fuse --disable-cache --repo="$REPO" --state-dir="$W/state" build src/com.valware.prism.yml
flatpak build-bundle "$REPO" "$HOME/osbuild/com.valware.prism.flatpak" com.valware.prism
ls -la "$HOME/osbuild/com.valware.prism.flatpak"
