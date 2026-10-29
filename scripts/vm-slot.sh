#!/usr/bin/env bash
# Runs a command while holding one of two VM slots, so parallel work never
# starts more test VMs at once than this computer has memory for.
#   scripts/vm-slot.sh python3 tests/preview.py ...
for i in 1 2; do
    exec {fd}>/tmp/os-vm-slot-$i
    if flock -n $fd; then "$@"; exit $?; fi
    exec {fd}>&-
done
exec {fd}>/tmp/os-vm-slot-$((RANDOM % 2 + 1))
flock $fd
"$@"
