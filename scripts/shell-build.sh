#!/usr/bin/env bash
# Builds os-shell and prints only warnings and errors.
cd "$(dirname "$0")/../userland"
make ../build/userland/os-shell OUT=../build/userland PORTS=../build/ports 2>&1 | grep -E 'error|warning|undefined' -A2 | head -80
echo "exit=${PIPESTATUS[0]}"
