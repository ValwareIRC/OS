#!/usr/bin/env bash
cd "$(dirname "$0")/.."
export OS_MEM=3G OS_QMP=/tmp/qmp-feature.sock
timeout 1500 python3 tests/alive_test.py build/fc-alive 2>&1 | grep -E "FAIL|passed"
timeout 900 python3 tests/apps_test.py tests/designs/paper.xml build/fc-apps-paper 2>&1 | grep -E "FAIL|^PASS"
