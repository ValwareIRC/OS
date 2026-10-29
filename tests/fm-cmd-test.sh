#!/usr/bin/env bash
# Exercises companion [[ui:...]] commands against the sample documents.
cd "$(dirname "$0")/.."
bash tests/fm-dump.sh tests/designs/glass.xml cmd 'press #all' 2>&1 | grep -E 'CMD|ACTION|sheet#launch|PROBLEM' | head -5
bash tests/fm-dump.sh tests/designs/glass.xml cmd 'style dock { background: rgba(255,120,80,0.3); }' cmd 'add menubar <button what="open:notes" icon="notes">Notes</button>' cmd 'set #me text="Sol here"' cmd 'remove #all' cmd 'press "Files"' 2>&1 | grep -E 'CMD|ACTION|PROBLEM|Sol here|Notes"' | head -12
