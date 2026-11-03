"""The freeze after closing Notes (docs/toy-audit.md 14b): the desktop stopped once, right as the built-in model
finished loading. This opens and closes the apps over and over (from the launcher, closed with Alt+F4) while the
built-in model loads and after, and after every close checks that the desktop still answers (the launcher opens).
The kernel's hang reports are on (HANGDUMP): if it stops, the report a minute later says what each thread was doing.
Windows QEMU, silent sound, through QMP only.

  python tests\\freeze_stress_vm_win.py [OUTDIR]     (ROUNDS=n, default 12)"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "freeze-vm"))
os.makedirs(out, exist_ok=True)
os.environ["HANGDUMP"] = "1"
ROUNDS = int(os.environ.get("ROUNDS", "12"))
APPS = ["notes", "files", "terminal", "settings", "calculator", "notes"]
vm = vmkit.Vm(out, ["Okay."] * 5, memory=os.environ.get("MEM", "20G"),
              mind={"provider": "builtin", "base_url": "", "api_key": "", "model": "os"})
frozen = None
closes = 0
t0 = time.time()
try:
    vm.start()
    vm.wait_for(r"shell: screen (desktop|choose|onboard)", 240)
    time.sleep(3)
    for r in range(ROUNDS):
        for app in APPS:
            m = len(vm.log_text())
            vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
            if not vm.wait_for(r"launcher: open", 15, m):
                frozen = f"round {r + 1}: the launcher didn't open before {app}"
                break
            time.sleep(0.6)
            vm.type(app)
            time.sleep(0.6)
            vm.key("ret")
            if not vm.wait_for(r"opened \"", 20, m):
                vm.key("esc")
                print(f"round {r + 1}: {app} didn't open (the launcher may not know it)")
                continue
            time.sleep(1.0 + (r % 3) * 0.7)  # different timings each round
            m = len(vm.log_text())
            vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "f4"}])
            closes += 1
            time.sleep(0.8)
            vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
            if not vm.wait_for(r"launcher: open", 15, m):
                frozen = f"round {r + 1}: the desktop stopped after closing {app}"
                break
            vm.key("esc")
            time.sleep(0.5)
        log = vm.log_text()
        awake = re.search(r"mind: (awake|never woke) after (\d+)s", log)
        print(f"round {r + 1}: {closes} closes, {time.time() - t0:.0f}s, mind {awake.group(1) if awake else 'loading'}",
              flush=True)
        if frozen:
            break
    if frozen:
        print("FAIL", frozen)
        vm.shot("frozen.png")
        time.sleep(75)  # the hang report
    else:
        vm.shot("end.png")
finally:
    vm.stop()
if not frozen:
    print(f"PASS the desktop answered after all {closes} closes")
sys.exit(1 if frozen else 0)
