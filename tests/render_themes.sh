#!/usr/bin/env bash
# Lints and renders starter themes natively (no VM), with a sample home
# folder for the ones that show files: build/tlint/<name>/1-desktop.jpg.
#   tests/render_themes.sh [name...]   (default: all of them)
cd "$(dirname "$0")/.."
T=userland/rootfs/usr/share/os/themes
H=build/tmp/fakehome
mkdir -p "$H"/{Documents,Pictures,Projects,Music,Downloads,Videos}
for f in notes.txt:3000 todo.md:800 budget.csv:12000 holiday.mp4:4000000 photo.jpg:900000 report.pdf:30000 script.py:2000; do
    [ -f "$H/${f%%:*}" ] || head -c "${f##*:}" /dev/zero > "$H/${f%%:*}"
done
names=("$@")
[ ${#names[@]} -gt 0 ] || names=($(ls $T/*.xml | xargs -n1 basename | sed 's/\.xml$//'))
for n in "${names[@]}"; do
    out=$(HOME=$PWD/$H bash tests/lint_design.sh "$T/$n.xml" "build/tlint/$n" 2>&1)
    printf '%-12s %s\n' "$n" "$(echo "$out" | head -1)"
    echo "$out" | grep '^- ' | head -6 | sed 's/^/             /'
done
