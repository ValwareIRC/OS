#!/usr/bin/env bash
# Every Facet 2 test app: checked, then run against its script.
#   tests/facet2/run-all.sh
cd "$(dirname "$0")/../.."
bash tests/facet.sh check tests/facet2/*.facet >/dev/null || { bash tests/facet.sh check tests/facet2/*.facet; exit 1; }
fail=0
for f in tests/facet2/*.facet; do
    s=${f%.facet}.script
    [ -f "$s" ] || continue
    out=$(build/host/facet run "$f" "$s" 2>&1)
    echo "$out" | grep -v '^> \|^  *<\|^  *"' | grep -v '^  -> '
    echo "$out" | tail -1 | grep -q passed || fail=1
done
exit $fail
