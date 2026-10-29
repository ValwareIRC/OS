#!/usr/bin/env bash
# The checks before publishing an image: the person's own disk, the title
# bar, Prism, and boot.
cd "$(dirname "$0")/.."
cp dist/lumen-disk.img /tmp/own-copy.img 2>/dev/null || cp dist/os-disk.img /tmp/own-copy.img
echo "== own disk"; OS_DEMO_DISK=/tmp/own-copy.img OS_MEM=6G OS_QMP=/tmp/qmp-rc.sock timeout 900 python3 tests/own_disk_check.py build/rc-own 2>&1 | grep -E 'PASS|FAIL'
grep -a 'store: restored\|mind ' build/rc-own/serial.log | head -3
echo "== title bar"; OS_MEM=3G OS_QMP=/tmp/qmp-rc.sock timeout 500 python3 tests/titlebar_test.py build/rc-titlebar 2>&1 | grep -E 'PASS|FAIL'
echo "== prism"; OS_MEM=6G OS_QMP=/tmp/qmp-rc.sock timeout 700 python3 tests/open_app.py prism build/rc-prism 360 2>&1 | grep -E 'WINDOW'
echo "== boot"; bash tests/boot_test.sh 2>&1 | grep -E 'PASS|FAIL'
