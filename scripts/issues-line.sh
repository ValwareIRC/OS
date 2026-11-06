#!/usr/bin/env bash
# One line about the project's issues (the Issues gadget's, in .prism/issues: local only), for Claude Code to add to
# each message Valerie sends (the UserPromptSubmit hook in .claude/settings.json): how many are open and resolved,
# and any filed by someone other than Claude since the last time it was shown. What was last seen is kept in
# .claude/issues-seen (not in git).
cd "${CLAUDE_PROJECT_DIR:-$(dirname "$0")/..}" 2>/dev/null || exit 0
dir=.prism/issues
[ -d "$dir" ] || exit 0
seen=$(cat .claude/issues-seen 2>/dev/null)
case "$seen" in '' | *[!0-9]*) seen=0 ;; esac
open=0 resolved=0 newest=$seen new=""
for f in "$dir"/[0-9][0-9][0-9][0-9]-*.md; do
    [ -e "$f" ] || continue
    id=$(sed -n 's/^id: //p' "$f" | head -1)
    status=$(sed -n 's/^status: //p' "$f" | head -1)
    by=$(sed -n 's/^by: //p' "$f" | head -1)
    title=$(sed -n 's/^title: //p' "$f" | head -1)
    case "$id" in '' | *[!0-9]*) continue ;; esac
    if [ "$status" = resolved ]; then resolved=$((resolved + 1)); else open=$((open + 1)); fi
    if [ "$id" -gt "$seen" ]; then
        [ "$by" != Claude ] && new="$new #$id \"$title\" ($status, by ${by:-someone});"
        [ "$id" -gt "$newest" ] && newest=$id
    fi
done
mkdir -p .claude && echo "$newest" > .claude/issues-seen
line="Issues (the Issues gadget, local): $open open, $resolved resolved."
[ -n "$new" ] && line="$line New since you last saw them, filed by others:$new Read them with the gadget's issue_get."
echo "$line"
