"""A desktop that dies while its model server runs doesn't leave the next one a server it can't use: Windows QEMU,
silent sound. The server (llama-server, in a session of its own) went on after a crash, holding its port and its
memory; the next desktop's couldn't start, and was started again at once, thousands of times, each a fork of the
whole desktop: lag, a key that typed itself over and over, and another crash (Valerie's log, 2026-11-05).

An app asks for the model on this computer ("local"), which starts it (port 8080); once it answers, the desktop is
killed. Then: the server ends with it (PR_SET_PDEATHSIG), or the new desktop stops it ("left behind"); the app
asks again and the new desktop's own server answers; one server, never a storm of starts.

  set ISO=build\\dev.iso & python tests\\leftover_server_vm_win.py"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = vmkit.ROOT
out = os.path.join(ROOT, "build", "leftover-server")
os.makedirs(out, exist_ok=True)
script = os.path.join(out, "autorun.sh")
open(script, "w", newline="\n").write(r'''#!/bin/sh
# (run at every start of the desktop: the first asks, then kills it; the next asks again. Told apart by the
# desktop's pid: the first is one of the boot's first processes; a mark left in /tmp or the home wasn't there
# for the next desktop)
desk() {
  for c in /proc/[0-9]*/comm; do
    [ "$(cat $c 2>/dev/null)" = os-shell ] || continue
    p=${c#/proc/}; p=${p%/comm}
    pp=$(awk '{print $4}' /proc/$p/stat 2>/dev/null)
    [ "$(cat /proc/$pp/comm 2>/dev/null)" = os-shell ] || { echo $p; return; }
  done
}
D=$(desk)
mkdir -p /run/user/1000/app/org.test.Local
S=/run/user/1000/app/org.test.Local/os-companion.sock
servers() { n=0; for c in /proc/[0-9]*/cmdline; do tr '\0' ' ' < $c 2>/dev/null | grep -q '^llama-server ' && n=$((n+1)); done; echo $n; }
ask() {
  B='{"model":"local","max_tokens":20,"chat_template_kwargs":{"enable_thinking":false},"messages":[{"role":"user","content":"Say hello."}]}'
  for i in $(seq 1 60); do
    for j in $(seq 1 30); do [ -S "$S" ] && break; sleep 1; done
    R=$(curl -s --max-time 600 -w 'HTTP%{http_code} ' --unix-socket $S -H 'Content-Type: application/json' -d "$B" http://os/v1/chat/completions)
    C=$(echo "$R" | grep -o 'HTTP[0-9]*' | tail -1)
    echo "LEFTOVER $1 chat $C $(echo "$R" | head -c 200)"
    case "$C" in HTTP200) return 0;; esac
    sleep 5
  done
  return 1
}
echo "LEFTOVER autorun for the desktop $D"
if [ "$D" -lt 150 ]; then
  ask first
  echo "LEFTOVER servers before $(servers)"
  echo "LEFTOVER killing the desktop $D"
  kill -KILL $D
  exit 0
fi
[ -e /run/user/1000/leftover-2 ] && exit 0
touch /run/user/1000/leftover-2
sleep 5
echo "LEFTOVER servers after $(servers)"
ask second
sleep 20
echo "LEFTOVER servers at the end $(servers)"
echo "LEFTOVER done"
''')
os.environ["AUTORUN"] = script
results = {}
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 24 << 30)
vm = vmkit.Vm(out, ["Okay."] * 20, memory=os.environ.get("MEM", "12G"), disk=disk, qport=4494, aiport=8938)
try:
    vm.start()
    results["the desktop starts"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    # the app's ask is granted by the person: yes, as localmodel_vm_win.py does
    if vm.wait_for(r"appapi: asking whether org\.test\.Local", 120):
        time.sleep(2)
        vm.key("ret")
    first = vm.wait_for(r"LEFTOVER first chat HTTP200", 600)
    results["the model on this computer answered"] = bool(first)
    results["the desktop was killed"] = bool(vm.wait_for(r"init: the desktop exited", 60))
    if vm.wait_for(r"appapi: asking whether org\.test\.Local", 180, since=first.end() if first else 0):
        time.sleep(2)
        vm.key("ret")
    after = vm.wait_for(r"LEFTOVER servers after (\d+)", 300)
    results["the old server didn't outlive its desktop"] = bool(after) and (
        after.group(1) == "0" or "left behind" in vm.log_text())
    results["the new desktop's own server answered"] = bool(vm.wait_for(r"LEFTOVER second chat HTTP200", 600))
    end = vm.wait_for(r"LEFTOVER servers at the end (\d+)", 120)
    results["one server at the end"] = bool(end) and end.group(1) == "1"
finally:
    vm.stop()
    log = vm.log_text()
    for line in re.findall(r"[^\n]*(?:LEFTOVER|left behind|mind: [^\n]*(?:starting|awake|never)|desktop exited)[^\n]*", log):
        print("   ", line[:220])
    starts = len(re.findall(r"mind: [^\n]*starting on this computer", log))
    print("    mind server starts:", starts)
    results["no storm of starts"] = starts <= 3
    for k, v in results.items():
        print(f"{k}: {v}")
    sys.exit(0 if all(results.values()) else 1)
