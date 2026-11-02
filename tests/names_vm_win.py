"""Their name is theirs: no design sets it. Boots starlit (a starter design that once carried the sample name
"Sam") before they've given a name, then they give one ([[user:]]), then designs that carried "Sam" are applied
([[theme:]]): the desktop greets them by their own name every time, and "Sam" never appears. Windows QEMU, silent
sound, through QMP only.

  python tests\\names_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "names-vm"))
os.makedirs(out, exist_ok=True)
replies = ["Hello.", "Valerie it is. [[user:Valerie]]", "Here. [[theme:workbench]]", "And this one. [[theme:swiss]]",
           "Back to the night. [[theme:starlit]]"] + ["Okay."] * 6
results = {}


def say(vm, text):
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    vm.type(text + "\n")


vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "20G"))
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 240)
    time.sleep(5)
    vm.shot("1-no-name.png")
    log = vm.log_text()
    results["before a name: no sample name"] = "for Sam" not in log
    for i, (text, want) in enumerate([("hello", None), ("call me Valerie", r"applied|user"),
                                      ("try workbench", r"applied design workbench"),
                                      ("try swiss", r"applied design swiss"),
                                      ("back to starlit", r"applied design starlit")]):
        m = len(vm.log_text())
        say(vm, text)
        if want:
            vm.wait_for(want, 30, m)
        time.sleep(5)
        vm.shot(f"{i + 2}-{text.split()[-1]}.png")
    log = vm.log_text()
    applied = re.findall(r"companion: applied design (\S+) \(for ([^)]*)\)", log)
    print("applied:", applied)
    results["designs that carried \"Sam\" applied"] = {"workbench", "starlit"} <= {n for n, _ in applied}
    results["each one for Valerie"] = bool(applied) and all(who == "Valerie" for _, who in applied)
    results["\"Sam\" never appears"] = not re.search(r"\bSam\b", log)
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
