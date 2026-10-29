#!/usr/bin/env bash
# Builds the OS and runs every automated check, one after another.
cd "$(dirname "$0")/.."
make 2>&1 | grep -E 'error:|warning:' | head -20
ls -la build/os.iso
echo "== boot"; make test 2>&1 | grep -E 'PASS|FAIL'
echo "== persist"; make test-persist 2>&1 | grep -E 'PASS|FAIL'
echo "== audio"; bash tests/audio_test.sh 2>&1 | tail -3
echo "== wayland in os"; OS_QMP=/tmp/os-wlc-qmp.sock timeout 900 python3 tests/wl_os_test.py 2>&1 | tail -3
echo "== voice"; OS_QMP=/tmp/os-voice-qmp.sock timeout 900 python3 tests/voice_test.py 2>&1 | tail -1
echo "== offline mind"; OS_QMP=/tmp/os-mind-qmp.sock timeout 1500 python3 tests/mind_test.py 2>&1 | tail -4
echo ALL-DONE
