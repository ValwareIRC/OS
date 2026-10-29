#!/bin/sh
# Wrapper so QEMU's guestfwd (which splits its command on spaces) can start
# the Windows relay: guestfwd=tcp:10.0.2.100:11434-cmd:tests/host-relay.sh 11434
exec "/mnt/c/Program Files/nodejs/node.exe" "$(wslpath -w "$(dirname "$0")/host-relay.js")" "${1:-11434}"
