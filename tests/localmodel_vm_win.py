"""The model on this computer, offered to apps beside the companion's own mind (appapi.c, "local"): Windows QEMU,
silent sound. The companion's mind is tests/fake_ai.py (elsewhere, as Claude on the PC is); an app (a folder
under /run/user/1000/app, as a Flatpak's is) asks the companion API for its models: "local" must be there, with
tools, though the model is still on the system's disc; then it asks "local" for an answer: "loading" until the
model is read and started, then the model on this computer answers.

  set ISO=build\\dev.iso & python tests\\localmodel_vm_win.py"""
import json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(ROOT, "build", "localmodel")
os.makedirs(out, exist_ok=True)
script = os.path.join(out, "autorun.sh")
open(script, "w", newline="\n").write(r'''#!/bin/sh
mkdir -p /run/user/1000/app/org.test.Local
S=/run/user/1000/app/org.test.Local/os-companion.sock
for i in $(seq 1 30); do [ -S "$S" ] && break; sleep 1; done
echo "LOCALTEST models $(curl -s --max-time 300 --unix-socket $S http://os/v1/models)"
B='{"model":"local","max_tokens":40,"chat_template_kwargs":{"enable_thinking":false},"messages":[{"role":"user","content":"Say hello in five words."}]}'
for i in $(seq 1 60); do
  R=$(curl -s --max-time 600 -w 'HTTP%{http_code} ' --unix-socket $S -H 'Content-Type: application/json' -d "$B" http://os/v1/chat/completions)
  C=$(echo "$R" | grep -o 'HTTP[0-9]*' | tail -1)
  echo "LOCALTEST chat $C $(echo "$R" | head -c 500)"
  case "$C" in HTTP200) break;; esac
  sleep 5
done
echo "LOCALTEST done"
''')
os.environ["AUTORUN"] = script
results = {}
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 24 << 30)
vm = vmkit.Vm(out, ["Okay."] * 10, memory=os.environ.get("MEM", "12G"), disk=disk, qport=4490, aiport=8934)
try:
    vm.start()
    results["the desktop starts"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    if vm.wait_for(r"appapi: asking whether org\.test\.Local", 120):
        time.sleep(4)
        vm.shot("1-consent.png")
        vm.key("ret")
    models = vm.wait_for(r"LOCALTEST models (.*)", 400)
    data = {}
    try:
        data = json.loads(models.group(1)) if models else {}
    except ValueError:
        pass
    local = [m for m in data.get("data", []) if m.get("id") == "local"]
    print(f"  models: {models.group(1)[:400] if models else None}", flush=True)
    results["the model on this computer is listed for apps"] = bool(local)
    results["with tools"] = bool(local) and local[0].get("tools") is True
    results["not loaded yet (still on the disc)"] = bool(local) and local[0].get("loaded") is False
    results["the companion's own mind is listed too"] = any(m.get("id") == "os" for m in data.get("data", []))
    loading = vm.wait_for(r"LOCALTEST chat HTTP503 .*\"loading\"", 120)
    results["asked: loading, while it is read and started"] = bool(loading)
    t0 = time.time()
    ok = vm.wait_for(r"LOCALTEST chat HTTP200 (.*)", 900)
    print(f"PERF local_ready_s={time.time() - t0:.0f}", flush=True)
    results["then the model on this computer answers"] = bool(ok) and '"choices"' in ok.group(1)
    if ok:
        print(f"  answer: {ok.group(1)[:300]}", flush=True)
    results["read from the disc for it"] = "model.gguf read" in vm.log_text()
finally:
    vm.stop()
for l in re.findall(r".*(?:appapi:|medium:|mind:).*", vm.log_text())[-16:]:
    print("  " + l[:200])
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
