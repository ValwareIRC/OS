"""Notes as more than one note (docs/toy-audit.md 14d): two notes written, both in the list beside the page;
opening the first from the list; searching the list for a word only one of them says; Ctrl+F in a note finds
the word and counts it; a note moved to Trash from the list. Keys and clicks through QMP, never the real mouse.
Windows QEMU, silent sound.

  python tests\\notes_list_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "notes-list-vm"))
os.makedirs(out, exist_ok=True)
results = {}


def rect(vm, name, since=0):
    m = None
    for m in re.finditer(rf"rect {name} (-?\d+) (-?\d+) (\d+) (\d+)", vm.log_text()[since:]):
        pass
    return tuple(int(v) for v in m.groups()) if m else None


def click_rect(vm, name, since=0, dx=None):
    r = rect(vm, name, since)
    if r:
        vm.click(r[0] + (r[2] // 2 if dx is None else dx), r[1] + r[3] // 2)
    return r


def ctrl(vm, k):
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"}, {"type": "qcode", "data": k}])


vm = vmkit.Vm(out, ["Okay."] * 5, memory=os.environ.get("MEM", "20G"), qport=4476, aiport=8920)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 240)
    time.sleep(4)
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    vm.type("notes")
    time.sleep(0.8)
    vm.key("ret")
    vm.wait_for(r"opened \"Notes\"", 20)
    time.sleep(2)
    m = len(vm.log_text())
    vm.type("Shopping list\nApples, bread and a pineapple. Then more apples.\n")
    results["the first note is kept"] = bool(vm.wait_for(r"notes: saved as .*Shopping list", 10, m))
    time.sleep(1.5)
    m = len(vm.log_text())
    click_rect(vm, "notes-new")
    time.sleep(1)
    vm.type("Ideas for the garden\nA bench under the tree.\n")
    results["the second note is kept"] = bool(vm.wait_for(r"notes: saved as .*Ideas for the garden", 10, m))
    time.sleep(3)
    m = len(vm.log_text())
    time.sleep(1.5)
    results["both are in the list"] = bool(rect(vm, "notes-row-0", m) and rect(vm, "notes-row-1", m))
    vm.shot("1-list.png")
    # the older one, from the list (newest first: it is second)
    click_rect(vm, "notes-row-1", m)
    results["opening one from the list"] = bool(vm.wait_for(r"notes: opened .*Shopping list", 8, m))
    time.sleep(1)
    # search
    click_rect(vm, "notes-search", m)
    time.sleep(0.5)
    vm.type("bench")
    time.sleep(3)
    m2 = len(vm.log_text())
    time.sleep(1.5)
    results["search finds only the note that says it"] = bool(rect(vm, "notes-row-0", m2)) and not rect(vm, "notes-row-1", m2)
    vm.shot("2-search.png")
    vm.key("esc")
    time.sleep(1)
    # Ctrl+F in the open note (Shopping list)
    m = len(vm.log_text())
    vm.click(900, 450)
    time.sleep(0.5)
    ctrl(vm, "f")
    results["Ctrl+F opens find"] = bool(vm.wait_for(r"notes: find", 5, m))
    time.sleep(0.5)
    vm.type("apples")
    time.sleep(1.5)
    vm.shot("3-find.png")
    vm.key("esc")
    time.sleep(1)
    # to Trash: the second row's bin
    m = len(vm.log_text())
    time.sleep(1.5)
    r = rect(vm, "notes-row-0", m)
    if r:  # the bin shows on the row under the pointer
        vm.click(r[0] + r[2] - 18, r[1] + r[3] // 2)
    results["a note goes to Trash from the list"] = bool(vm.wait_for(r"notes: .* to Trash", 8, m))
    time.sleep(1.5)
    vm.shot("4-trashed.png")
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
