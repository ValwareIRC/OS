#!/usr/bin/env bash
# The app-permission and install-safety layer's acceptance tests, each exercised, each reported PASS or FAIL.
#   tests/facet_security.sh            (the second opinion uses a deliberately gullible stand-in reviewer)
#   REVIEW_LIVE=1 tests/facet_security.sh   (also asks the real small model on 127.0.0.1:8092)
# h) and i) are about the system's own interface: tests/security_ui_test.c checks them on the host, and
# tests/security_vm_win.py in the OS itself.
cd "$(dirname "$0")/.." || exit 2
S=tests/facet2/security
bash tests/facet.sh check tests/facet2/notes.facet >/dev/null 2>&1 || { bash tests/facet.sh check tests/facet2/notes.facet; exit 2; }
F=build/host/facet
pass=0 fail=0
report() { # report LETTER NAME OK DETAIL
    if [ "$3" = 1 ]; then echo "PASS $1) $2"; pass=$((pass + 1)); else echo "FAIL $1) $2"; fail=$((fail + 1)); fi
    [ -n "$4" ] && echo "$4" | sed 's/^/       /'
}

# a) a URL built while it runs needs the general internet; a server fixed in the code is named
caps=$($F caps $S/a_hosts.facet)
run=$($F run $S/a_hosts.facet $S/a_hosts.script 2>&1)
ok=0
echo "$caps" | grep -q 'host:api.weather.test ' && echo "$caps" | grep -q '^  internet ' &&
    ! echo "$caps" | grep -q 'news.example.test' && echo "$run" | tail -1 | grep -q passed && ok=1
report a "a dynamically built URL needs the general internet; only fixed hosts are named" $ok "$caps"$'\n'"$(echo "$run" | tail -1)"

# b) an update that adds a capability is blocked until approved, old and new shown
gate=$($F gate $S/b_v1.facet $S/b_v2.facet)
ok=0
echo "$gate" | grep -q 'BLOCKED' && echo "$gate" | grep -q 'already allowed: read:~/Jot.txt' &&
    echo "$gate" | grep -q 'NEW: host:sync.jot.test' && echo "$gate" | grep -q 'after approving them: runs' && ok=1
report b "an update adding a capability is blocked until re-approved, old vs new shown" $ok "$gate"

# c) a notepad reaching for any server: a strong warning, from rules alone (no model involved)
caps=$($F caps $S/c_notepad.facet)
ok=0
echo "$caps" | grep -q 'WARNING: A notes app has no need to reach any server' && ok=1
report c "a notepad asking for the internet is warned about with no AI (Facet has no camera API to ask for)" $ok "$(echo "$caps" | grep -i warning)"

# d) the optional permission refused: the app runs, the call fails at run time
caps=$($F caps $S/d_optional.facet)
run=$($F run $S/d_optional.facet $S/d_optional.script 2>&1)
ok=0
echo "$caps" | grep -q 'host:backup.diary.test *optional' && echo "$run" | tail -1 | grep -q passed && ok=1
report d "an optional permission refused leaves the app running; that call fails at run time" $ok "$(echo "$caps" | grep backup)"$'\n'"$(echo "$run" | grep -E 'refused|passed|FAILED')"

# the sandbox's hard rules (part of d's enforcement): climbing out, system files, links, secret stores, settings
run=$($F run $S/escape.facet $S/escape.script 2>&1)
run2=$($F run $S/secret.facet $S/secret.script 2>&1)
ok=0
echo "$run" | tail -1 | grep -q passed && echo "$run2" | tail -1 | grep -q passed && ok=1
report d+ "no way out: '..', system paths, links out of home, ~/.ssh and private settings all refused" $ok "$(echo "$run" | grep -E 'refused:|passed|FAILED')"$'\n'"$(echo "$run2" | tail -1)"

# e) a comment telling the reviewer to approve: scores with and without differ, and it is flagged
r=$($F review $S/e_inject.facet --gullible)
ok=0
echo "$r" | grep -q 'injection YES' && echo "$r" | grep -q 'level 2' && ok=1
report e "a comment steering the reviewer changes the raw scores and is flagged (stand-in reviewer)" $ok "$r"
if [ -n "$REVIEW_LIVE" ]; then
    echo "       the real model: $($F review $S/e_inject.facet)"
fi

# f) injection text in strings, names or the app's own words doesn't change the scorecard
n1=$($F normalize $S/f_plain.facet)
n2=$($F normalize $S/f_steered.facet)
r1=$($F review $S/f_plain.facet --gullible | sed 's/^[^:]*://; s/ q[0-9] [0-9.]*\/[0-9.]*//g')
s1=$($F review $S/f_plain.facet --gullible | grep -o 'q[0-9] [0-9.]*' | tr '\n' ' ')
s2=$($F review $S/f_steered.facet --gullible | grep -o 'q[0-9] [0-9.]*' | tr '\n' ' ')
ok=0
[ "$n1" = "$n2" ] && [ "$s1" = "$s2" ] && ok=1
report f "injection in a string, a name or the description leaves the normalised code and its scores identical" $ok "normalised identical: $([ "$n1" = "$n2" ] && echo yes || echo no); scores: [$s1] vs [$s2]"

# g) what the companion is told holds nothing written in the app
note=$($F note $S/g_marked.facet --gullible)
ok=1
for m in ZQXNAME ZQXSTRING ZQXCOMMENT ZQXHOST ZQXPATH ZQXFIELD ZQXAPP; do
    echo "$note" | grep -q "$m" && ok=0
done
report g "the companion's note contains no text from the app file (names, strings, comments, hosts, paths)" $ok "$(echo "$note" | head -4)"

# every ordinary app still works under the sandbox
all=$(bash tests/facet2/run-all.sh 2>&1 | grep -E 'passed|FAILED')
ok=0
echo "$all" | grep -q FAILED || ok=1
report - "every sample app runs under the sandbox with what its code asks for" $ok "$all"

# h) and i): the security settings, on the host, against the OS's own code
hi=$(bash tests/security_ui_test.sh 2>/dev/null)
ok=0; echo "$hi" | grep -q "^i) passed" && ok=1
report i "turning warnings off needs the typed phrase and the wait; the permission list stays" $ok "$(echo "$hi" | grep -E "^i)|FAIL")"
ok=0; echo "$hi" | grep -q "^h) passed" && ok=1
report h "no Facet code, action or tag can reach the turn-off; Settings never opens onto it" $ok "$(echo "$hi" | grep -E "^h)|FAIL")"

echo "$pass passed, $fail failed"
[ $fail = 0 ]
