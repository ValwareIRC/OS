#!/usr/bin/env bash
# Every kind of right-click menu in every starter design, filled as the desktop fills it (menufill.c) and drawn by
# the real engine: is every action placed, and only once? With OUT_DIR, the pictures (2-open.jpg: it open).
#   tests/menu_shots.sh [OUT_DIR] [design ...]
cd "$(dirname "$0")/.."
OUT="${1:-}"
shift || true
THEMES=("$@")
[ ${#THEMES[@]} -gt 0 ] || THEMES=(userland/rootfs/usr/share/os/themes/*.xml)
bad=0
for t in "${THEMES[@]}"; do
  name=$(basename "$t" .xml)
  for k in app window desktop text image file link; do
    dir=""
    [ -n "$OUT" ] && dir="$OUT/$name-$k"
    line=$(MENU=$k bash tests/lint_design.sh "$t" $dir 2>&1 | grep -E "^MENU |DOES NOT HOLD")
    echo "$name: $line"
    case "$line" in *"SOME "*|*"DOES NOT"*|"") bad=$((bad+1));; esac
  done
done
echo "menus with a problem: $bad"
[ $bad -eq 0 ]
