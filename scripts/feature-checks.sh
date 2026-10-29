#!/usr/bin/env bash
# The new desktop features, end to end on the main tree (scripts/feature_checks.py).
#   scripts/feature-checks.sh [--only NAME,NAME...]
exec python3 "$(dirname "$0")/feature_checks.py" "$@"
