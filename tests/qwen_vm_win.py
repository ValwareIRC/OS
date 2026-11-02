r"""The model on this computer, the big one (Qwen3 8B), answers the companion: it doesn't think silently
for minutes first (thinking is switched off in every request), its context holds the companion's instructions
(never under 12288 tokens), and a reply arrives. Qwen3 8B comes in the image; DISK (optional, used in snapshot
mode: never written) for one downloaded onto a disk. Windows QEMU with silent sound; MEM sets the VM's memory.

  python tests\qwen_vm_win.py [OUTDIR] [DISK]"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vmkit import Vm, ROOT

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "qwen-vm"))
disk = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else None
os.makedirs(out, exist_ok=True)
vm = Vm(out, [], disk=disk, memory=os.environ.get("MEM", "20G"),
        mind={"provider": "builtin", "base_url": "", "api_key": "", "model": "os"})
ok = True


def check(name, cond, detail=""):
    global ok
    ok &= bool(cond)
    print(("PASS " if cond else "FAIL ") + name + (f": {detail}" if detail else ""), flush=True)


try:
    vm.start()
    m = vm.wait_for(r"mind: (.+?) starting on this computer \(pid \d+, (\d+) tokens\)", 240)
    check("the model starts", m and "Qwen" in m.group(1), m.group(0) if m else "not started")
    check("its context holds the companion's instructions", m and int(m.group(2)) >= 12288,
          m.group(2) + " tokens" if m else "")
    awake = vm.wait_for(r"mind: (awake|never woke) after (\d+)s", 600)
    check("it loads", awake and awake.group(1) == "awake", awake.group(0) if awake else "still loading")
    time.sleep(20)
    vm.shot("before")
    vm.focus_companion()
    since = len(vm.log_text())
    t0 = time.time()
    vm.type("hi, what can you do for me?\n")
    got = vm.wait_for(r"\n(?:shell: )?(ai|ai-error) (.{0,300})", 900, since)
    took = time.time() - t0
    vm.shot("after")
    text = got.group(2) if got else ""
    check("a reply comes", got and got.group(1) == "ai", f"{took:.0f}s: {text[:200]}" if got else "none in 15 min")
    check("no thinking in the reply", "<think>" not in text and "</think>" not in text)
    log = vm.log_text()
    check("no refusal for size", "exceeds the available context size" not in log)
    check("the server didn't end", "ended by itself" not in log)
finally:
    vm.stop()
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
