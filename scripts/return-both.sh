#!/usr/bin/env bash
# Rebuilds, then boots Sam's and Noor's saved disks ("the next morning").
cd "$(dirname "$0")/.."
make 2>&1 | grep -E 'error:' | head
for p in sam noor; do
  OS_DEMO_DISK=build/$p-disk.img OS_QMP=/tmp/os-ret-$p.sock timeout 900 python3 tests/return_test.py build/return-$p 2>&1 | tail -4
done
