#!/usr/bin/env bash
# Opens Prism on ROUNDS x 2 fresh boots (two VMs at a time, 4G each, so WSL
# keeps room) and reports which got a window: scripts/prism-reliability.sh [ROUNDS]
cd "$(dirname "$0")/.."
rounds=${1:-3}
k=0
for r in $(seq 1 "$rounds"); do
  for j in 1 2; do
    k=$((k + 1))
    OS_MEM=4G OS_QMP=/tmp/qmp-rel$j.sock timeout 600 python3 tests/open_app.py prism build/rel-prism-$k 240 \
      > build/rel-prism-$k.txt 2>&1 &
    sleep 15
  done
  wait
done
ok=0
for i in $(seq 1 "$k"); do
  line=$(head -1 build/rel-prism-$i.txt)
  case "$line" in WINDOW*) ok=$((ok + 1));; esac
  echo "run $i: $line | execvp failures: $(grep -ac 'failed to execvp' build/rel-prism-$i/serial.log)"
done
echo "$ok of $k opened a window"
