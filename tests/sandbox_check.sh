#!/bin/sh
# Runs inside the OS as the person (fw_cfg autorun; tests/sandbox_check.py).
echo "SBCHECK start"
flatpak run com.valware.prism >/tmp/prism.log 2>&1 &
sleep 25
flatpak ps 2>&1 | sed 's/^/SBCHECK ps: /'
flatpak list --app >/dev/null 2>&1   # another flatpak command: it tidies instances it thinks are gone
for d in /run/user/1000/.flatpak/*/; do
    [ -f "$d/info" ] && grep -q com.valware.prism "$d/info" && echo "SBCHECK instance $(basename $d): bwrapinfo $( [ -s $d/bwrapinfo.json ] && echo present || echo MISSING)"
done
flatpak run --command=sh com.valware.prism -c 'ls -l /dev/ptmx /dev/pts/; exec 3<>/dev/ptmx && echo "SBCHECK pty ok"' 2>&1 | sed 's/^/SBCHECK sandbox: /'
echo "SBCHECK done"
