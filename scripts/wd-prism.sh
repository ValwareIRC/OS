#!/usr/bin/env bash
# Opens Prism on ROUNDS x 2 fresh boots with WAYLAND_DEBUG=1 (its side of the
# Wayland conversation goes to the serial log), two VMs at a time, until one
# fails to show a window: scripts/wd-prism.sh [ROUNDS]
cd "$(dirname "$0")/.."
rounds=${1:-2}
k=0
for r in $(seq 1 "$rounds"); do
  for j in 1 2; do
    k=$((k + 1))
    OS_APP_ENV="${APP_ENV:-WAYLAND_DEBUG=1}" OS_MEM=4G OS_QMP=/tmp/qmp-wd$j.sock timeout 600 \
      python3 tests/open_app.py prism build/wd-prism-$k 240 > build/wd-prism-$k.txt 2>&1 &
    sleep 15
  done
  wait
  for i in $(seq $((k - 1)) "$k"); do echo "run $i: $(head -1 build/wd-prism-$i.txt)"; done
  grep -q "NO WINDOW" build/wd-prism-$((k - 1)).txt build/wd-prism-$k.txt && break
done
