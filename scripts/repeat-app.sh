#!/usr/bin/env bash
# Opens an app N times on fresh boots and reports which runs got a window.
#   scripts/repeat-app.sh APP N WAIT
cd "$(dirname "$0")/.."
for i in $(seq 1 "$2"); do
  OS_MEM=6G OS_QMP=/tmp/qmp-rep.sock timeout $(( $3 + 200 )) python3 tests/open_app.py "$1" "build/rep-$1-$i" "$3" > /dev/null 2>&1
  echo "run $i: $(grep -ac 'app window' build/rep-$1-$i/serial.log) window line(s); last: $(grep -av 'rect \|store: saved' build/rep-$1-$i/serial.log | tail -1 | cut -c1-120)"
done
