#!/usr/bin/env bash
# Builds the kernel and prints only errors (and the real exit status).
cd "$(dirname "$0")/.."
make -C kernel BUILD=../build/kernel OUT=../build/kernel.elf -k 2>&1 | grep -E 'error:|Error [0-9]' -A3 | grep -v '^--$' | head -${1:-60}
exit "${PIPESTATUS[0]}"
