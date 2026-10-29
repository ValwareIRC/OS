#!/usr/bin/env bash
# Waits for a running demo's build to finish, then previews a design.
cd "$(dirname "$0")/.."
OUTF=$1; shift
for i in $(seq 1 60); do grep -q built "$OUTF" 2>/dev/null && break; sleep 5; done
timeout 200 python3 tests/preview.py "$@" 2>&1 | tail -2
grep -E 'ui problem' build/gallery/preview-serial.log | head
