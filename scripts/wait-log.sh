#!/usr/bin/env bash
# wait-log.sh LOG PATTERN SECONDS [SHOW-PATTERN]: waits for PATTERN in LOG, then shows matching lines.
LOG=$1; PAT=$2; SECS=${3:-300}; SHOW=${4:-$2}
for i in $(seq 1 $((SECS / 5))); do grep -qE "$PAT" "$LOG" 2>/dev/null && break; sleep 5; done
grep -E "$SHOW" "$LOG" | cut -c1-300 | tail -15
