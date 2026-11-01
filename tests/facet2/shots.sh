#!/usr/bin/env bash
# Pictures of the council's five apps in a design, with sample files.
#   tests/facet2/shots.sh [DESIGN.xml]   -> build/fxshots/<app>.png
cd "$(dirname "$0")/../.."
DESIGN=${1:-userland/rootfs/usr/share/os/themes/starlit.xml}
H=$PWD/build/fxhome
rm -rf "$H"
mkdir -p "$H/Notes" "$H/Documents/Zeta" build/fxshots
printf 'eggs, milk' > "$H/Notes/shopping.txt"
printf 'a moon that talks' > "$H/Notes/ideas.txt"
printf '  buy bread\nx call mum\n  water plants' > "$H/Todo.txt"
printf 'ay' > "$H/Documents/a.txt"
printf 'bee' > "$H/Documents/b.txt"
for a in notes files todo launcher stopwatch; do
    bash tests/fx_shot.sh tests/facet2/$a.facet "$DESIGN" build/fxshots/$a.png "$H" 2>&1 | grep -v 'note:\|warning\|^ \|^In file\|^tests/' | grep -v '^fx_shot: [a-z]* *[a-z]*$'
done
