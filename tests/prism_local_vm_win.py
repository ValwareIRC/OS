"""Prism on the OS when the companion's mind can't use tools (Claude through the PC's bridge): Prism's agent must
start on the model on this computer ("local", companion API), which the system reads from its disc and starts
for it, and the agent's turn must work on it. Windows QEMU, silent sound. The companion's mind is the
"claude-pc" provider pointed at tests/fake_ai.py (so "os" says tools:false, as the real bridge does). Prism is
driven over its test-only debug port (prism-work's scripts/room-test/cdp.mjs), from an autorun script.

  set ISO=build\\dev.iso & python tests\\prism_local_vm_win.py"""
import base64, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(ROOT, "build", "prism-local")
os.makedirs(out, exist_ok=True)
AIPORT = 8935
cdp = base64.b64encode(open(os.environ.get("CDP_MJS", os.path.join(ROOT, "..", "prism-work", "scripts", "room-test",
                                                                   "cdp.mjs")), "rb").read()).decode()
# a conversation with no model named: Prism chooses ("os" if it takes tools, else "local")
start = ("(async () => { await prism.workspace.set('/home/user/Projects/ostest'); await new Promise(r => setTimeout(r, 2500));"
         " const s = await prism.sessions.create('OS test');"
         " await prism.agent.send(s.id, 'Read README.md and tell me its first line.', { mode: 'auto-edit' });"
         " return s.id })()")
script = os.path.join(out, "autorun.sh")
open(script, "w", newline="\n").write(
    "#!/bin/sh\nmkdir -p $HOME/Projects/ostest\n"
    "printf 'Lighthouse keeper notes\\nsecond line\\n' > $HOME/Projects/ostest/README.md\n"
    "base64 -d > $HOME/cdp.mjs <<'B64'\n" + cdp + "\nB64\n"
    "cat > $HOME/turn-start.js <<'JS'\n" + start + "\nJS\n"
    "flatpak run --env=PRISM_DEBUG_PORT=9223 com.valware.prism >/tmp/prism.log 2>&1 &\n"
    "sleep 90\n"
    "CDP=\"ELECTRON_RUN_AS_NODE=1 /app/prism/prism $HOME/cdp.mjs eval\"\n"
    "ID=$(flatpak run --command=sh com.valware.prism -c \"$CDP \\\"\\$(cat $HOME/turn-start.js)\\\"\" 2>/dev/null | tr -d '\"\\n ')\n"
    "echo \"AGENTCHECK session $ID\"\n"
    "for i in $(seq 1 40); do\n"
    "  sleep 15\n"
    "  echo \"prism.sessions.items('$ID').then(i => JSON.stringify(i).slice(-3000))\" > $HOME/turn-items.js\n"
    "  R=$(flatpak run --command=sh com.valware.prism -c \"$CDP \\\"\\$(cat $HOME/turn-items.js)\\\"\" 2>&1 | tr '\\n' ' ')\n"
    "  case \"$R\" in *Lighthouse*) echo \"AGENTCHECK items $R\" | head -c 3500; echo; break;; esac\n"
    "done\n"
    "echo \"PRISMLOG $(tail -c 1500 /tmp/prism.log | tr '\\n' '|')\"\n"
    "echo 'AGENTCHECK done'\n")
os.environ["AUTORUN"] = script
results = {}
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 24 << 30)
mind = {"provider": "claude-pc", "base_url": f"http://10.0.2.2:{AIPORT}/v1", "api_key": "", "model": "sonnet"}
vm = vmkit.Vm(out, ["Okay."] * 20, memory=os.environ.get("MEM", "14G"), disk=disk, mind=mind, qport=4491, aiport=AIPORT)
try:
    vm.start()
    results["the desktop starts"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    if vm.wait_for(r"appapi: asking whether com\.valware\.prism", 300):
        time.sleep(4)
        vm.shot("1-consent.png")
        vm.key("ret")
    results["Prism asks for the model on this computer (the mind can't use tools)"] = bool(
        vm.wait_for(r"appapi: com\.valware\.prism asks for the model on this computer", 400))
    results["read from the disc and started"] = bool(vm.wait_for(r"medium: /usr/share/os/mind/model\.gguf read", 300))
    results["Prism's agent asks it"] = bool(vm.wait_for(r"appapi: com\.valware\.prism asks the model on this computer", 600))
    items = vm.wait_for(r"AGENTCHECK items (.*)", 900)
    results["the agent's turn on it reads the file"] = bool(items) and "Lighthouse keeper notes" in items.group(1)
    vm.shot("2-after.png")
    vm.wait_for(r"AGENTCHECK done", 120)
finally:
    vm.stop()
for l in re.findall(r".*(?:AGENTCHECK session|PRISMLOG|appapi: com\.valware|medium:|mind: ).*", vm.log_text())[-20:]:
    print("  " + l[:300])
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
