"""The companion looks at a picture file itself ([[see: PATH]]), a picture opens in the viewer (not in Notes), and
what the screen says while its tags are carried out ("isn't there any more") is told to it, so it doesn't try the
same thing again and again unaware. tests/fake_ai.py plays the companion. Windows QEMU, silent sound, a fresh disk.

  python tests\\see_picture_vm_win.py [OUTDIR]"""
import json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "see-picture-vm"))
os.makedirs(out, exist_ok=True)
PNG = "iVBORw0KGgoAAAANSUhEUgAAABAAAAAQCAIAAACQkWg2AAAAFklEQVR4nGO4o6FBEmIY1TCqYfhqAAAyBCwQhvh37QAAAABJRU5ErkJggg=="
replies = [
    {"when": "look at my picture", "reply": f"[[sh: mkdir -p ~/Pictures && echo {PNG} | base64 -d > ~/Pictures/red.png]]"},
    {"when": "What your body did, step 1", "reply": "[[see: ~/Pictures/red.png]]"},
    {"when": "What your body did, step 2", "reply": "It's a small red square."},
    {"when": "open it", "reply": "There. [[open:~/Pictures/red.png]]"},
    {"when": "open the other one", "reply": "Opening it. [[open:~/Pictures/missing.png]]"},
    {"when": "The screen showed them", "reply": "That one isn't there, so I couldn't open it."},
] + ["Okay."] * 8
results = {}
vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "8G"), qport=4479, aiport=8923, model="gpt-4o-stand-in")
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 240)
    time.sleep(4)
    vm.focus_companion()
    vm.say("look at my picture", r"shell: ai It's a small red square", 90)
    log = [json.loads(l) for l in open(vm.ailog) if l.strip()]
    saw = [r for r in log if r.get("pictures") and any("comes with this, as a picture" in t.get("content", "")
                                                       for t in r["tail"])]
    results["it looked at the file itself (the picture went with the next step)"] = bool(saw)
    m = len(vm.log_text())
    vm.say("open it", r"shell: ai There", 60)
    results["a picture opens in the viewer"] = bool(vm.wait_for(r"files: open \S*red\.png \(", 10, m))
    results["not in Notes"] = "There's no app here for red.png" not in vm.log_text()[m:]
    time.sleep(2)
    vm.shot("1-viewer.png")
    m = len(vm.log_text())
    vm.say("open the other one", r"shell: ai Opening it", 60)
    results["what the screen said is told to it"] = bool(vm.wait_for(r"companion: told what the screen said", 10, m))
    results["and it says so at once, once"] = bool(vm.wait_for(r"shell: ai That one isn't there", 60, m))
    time.sleep(8)
    log = [json.loads(l) for l in open(vm.ailog) if l.strip()]
    results["and it reads it in its next request"] = any(
        "the screen said" in t.get("content", "") and "missing.png" in t.get("content", "") for r in log for t in r["tail"])
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
