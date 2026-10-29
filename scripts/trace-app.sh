#!/usr/bin/env bash
# Opens an app with its failing system calls traced and prints the last ones
# of the first of its processes that crashed: trace-app.sh APP [WAIT]
cd "$(dirname "$0")/.."
out=build/trace-$1
TRACE_APP=$1 OS_MEM=6G OS_QMP=/tmp/qmp-trace.sock timeout 900 python3 tests/open_app.py "$1" "$out" "${2:-240}" > /dev/null 2>&1
grep -a 'exception\|signal\|app window' "$out/serial.log" | grep -av rect | head -5
p=$(grep -ao "pid [0-9]* ($1): exception" "$out/serial.log" | head -1 | awk '{print $2}')
[ -n "$p" ] && { echo "crashed: $p"; grep -a "^\[$p/" "$out/serial.log" | tail -30 | cut -c1-200; }
