#!/usr/bin/env bash
# Compiles the shell and lays out every sample design plus the latest real ones.
cd "$(dirname "$0")/.."
make userland 2>&1 | grep -E 'error|warning:' -A3 | head
bash tests/fm-dump.sh build/tmp/demo-05.xml 2>&1 | grep -E '^    <(stack)|^      <(applist|orb)'
for f in tests/designs/*.xml build/tmp/*.xml; do bash tests/fm-dump.sh "$f" > /dev/null 2>&1 || echo "FAIL $f"; done
echo checked
