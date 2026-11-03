"""The companion saying it is doing something with no tag behind it ("Fixing it now.", and nothing happened) is told
at once that nothing happened, and does it then; once per thing they said (never a loop). tests/fake_ai.py plays
the companion. Windows QEMU, silent sound, a fresh disk.

  python tests\\said_not_done_vm_win.py [OUTDIR]"""
import json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "said-not-done-vm"))
os.makedirs(out, exist_ok=True)
replies = [
    {"when": "open my notes", "reply": "Opening it now."},
    {"when": "but it had no tag", "reply": "Done this time. [[open:notes]]"},
    {"when": "how are you", "reply": "Fine, thanks. Let me know if you need anything."},
] + ["Okay."] * 10
results = {}
vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "8G"), qport=4481, aiport=8925)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 240)
    time.sleep(4)
    vm.focus_companion()
    m = len(vm.log_text())
    vm.say("open my notes", r"shell: ai Opening it now", 60)
    results["told it said so with no tag"] = bool(vm.wait_for(r"said it was doing something with no tag: told", 10, m))
    results["and then it did it"] = bool(vm.wait_for(r"opened \"Notes\"", 30, m))
    time.sleep(3)
    m = len(vm.log_text())
    vm.focus_companion()
    vm.say("how are you", r"shell: ai Fine", 60)
    time.sleep(6)
    results["plain talk with no promise isn't flagged"] = "with no tag: told" not in vm.log_text()[m:]
    n = sum(1 for _ in re.finditer(r"with no tag: told", vm.log_text()))
    results["once, never a loop"] = n == 1
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
