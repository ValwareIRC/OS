"""Styling with the companion, as Valerie does with Mita, until the desktop has outlived every design it replaced:
the companion edits the preview ([[ui:style]]), sees pictures of it and answers, the person has it applied
([[preview:apply]]), and the companion's next edits open a new preview at once. A replaced design is freed 10 s
after it goes (desktop_set_ui); the desktop crashed 10 to 30 s after each apply (in memchr, from snprintf, reading
memory that was gone), three times in one evening. Each round waits 30 s after its apply: nothing may crash.

  set ISO=build\\dev.iso & python tests\\style_apply_vm_win.py [OUTDIR]"""
import json, os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = vmkit.ROOT
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "style-apply-vm"))
os.makedirs(out, exist_ok=True)
ROUNDS = int(os.environ.get("ROUNDS", "4"))

replies = []
for i in range(ROUNDS):
    replies += [
        {"when": rf"taller {i}\b", "reply": f"Taller, and the start button fills it. [[ui:style #bar {{ height: {40 + 4 * i}; "
                                            f"padding-top: 0; padding-bottom: 0; }}]] [[ui:style #start {{ height: 100%; "
                                            f"margin: 0; }}]]"},
        {"when": rf"apply {i}\b", "reply": "Done, it's yours. [[preview:apply]]"},
        # what it says after the apply, to the pictures of the edit: more edits (a new preview, as in her log)
        {"when": r"how it looks now", "reply": f"[[ui:unstyle #bar {{ padding-top; padding-bottom }}]] "
                                               f"[[ui:style #start {{ radius: {i}; }}]]"},
    ]
for _ in range(30):
    replies.append({"when": r"how it looks now", "reply": "[[ok]]"})
for _ in range(10):
    replies.append("Mm.")

# DISK=a disk with someone's saved state (build/tmp/her_store_disk.py): their own design, not w95's
disk = os.environ.get("DISK") or vmkit.Vm.new_disk(os.path.join(out, "disk.img"), 4 << 30)
vm = vmkit.Vm(out, replies, alive="proactive=0\n", model="fake-vision", disk=disk, qport=4492, aiport=8936,
              memory=os.environ.get("MEM", "8G"), design_path=os.path.join(ROOT, "tests", "designs", "w95.xml"),
              facet=not os.environ.get("DISK"))
results = {}


def crashed(log):
    return re.findall(r"pid \d+ \(os-shell\): segmentation fault[^\n]*|init: the desktop exited[^\n]*", log)


try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    time.sleep(6)
    vm.focus_companion()
    applied = 0
    for i in range(ROUNDS):
        vm.say(f"make the bar taller {i}", r'ui command "style #bar[^\n]*\(preview\)')
        time.sleep(4)
        before = vm.log_text().count("design applied from the preview")
        vm.say(f"apply {i} please", r"design applied from the preview", secs=60)
        if vm.log_text().count("design applied from the preview") > before:
            applied += 1
        t0 = time.time()
        while time.time() - t0 < 30 and not crashed(vm.log_text()):
            time.sleep(1)
        if crashed(vm.log_text()):
            break
        vm.focus_companion()  # (the preview took the keys)
    log = vm.log_text()
    results[f"applied {ROUNDS} times"] = applied == ROUNDS
    results["the desktop never crashed"] = not crashed(log)
    vm.shot("after.png")
finally:
    vm.stop()
    log = vm.log_text()
    for line in crashed(log):
        print("   ", line[:400])
    for line in re.findall(r"shell: (?:preview: [^\n]*|design applied[^\n]*|ui command [^\n]*)", log)[-16:]:
        print("   ", line[:200])
    for k, v in results.items():
        print(f"{k}: {v}")
    sys.exit(0 if results and all(results.values()) else 1)
