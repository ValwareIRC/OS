#!/usr/bin/env bash
# Stops a running tests/demo.py and its QEMU.
for p in $(pgrep -f "python3 tests/demo.py"); do kill "$p" 2>/dev/null; done
sleep 1
for p in $(pgrep -f "os-demo-qmp"); do kill "$p" 2>/dev/null; done
sleep 1
pgrep -fa qemu-system | cut -c1-80 || true
