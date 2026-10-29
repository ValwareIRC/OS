#!/bin/sh
# One Prism start (sandbox), for tests/prism_ab.sh
sleep 10
echo "PRISMONCE start"
flatpak run com.valware.prism  >/tmp/prism-once.log 2>&1 &
sleep 150
echo "PRISMONCE end"
