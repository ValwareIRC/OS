"""The companion runs as the person, as the desktop does, so permissions alone can't keep it from what decides
what apps may do and from the keys to their accounts. In the OS, with Windows QEMU (Hyper-V) and silent sound,
tests/fake_ai.py as its mind:
  its shell ([[sh]]) runs in a view of the system without them: it can't read the mind's key, can't write the
  security settings (what it writes there is gone), yet its own files (its memory) change for real;
  its file tools refuse them by name and through "..": [[cat: /etc/os/mind.json]], [[cat: ~/../etc/os/...]].
  python tests\\fence_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = vmkit.ROOT
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "fence-vm"))
os.makedirs(out, exist_ok=True)
KEY = "sk-fence-test-7c1f0e"

replies = [
    {"when": r"check the locks", "reply": "[[sh]]echo VIEW: $(ls -a /etc/os | tr '\\n' ' '); cat /etc/os/mind.json; "
                                          "echo FENCE > /etc/os/security.conf && echo WROTE-SECURITY; "
                                          "mkdir -p /etc/os/app-grants/x 2>/dev/null && echo MADE-GRANT; "
                                          "echo fence-memory-line >> /etc/os/memory.txt && echo WROTE-MEMORY; "
                                          "echo SH-DONE[[/sh]]"},
    {"when": r"SH-DONE", "reply": "[[cat: /etc/os/mind.json]]"},
    {"when": r"private to the system[\s\S]*$", "reply": "[[cat: ~/../etc/os/security.conf]]"},
    {"when": r"cat: ~/\.\./etc/os/security\.conf", "reply": "[[ls: /etc/os]]"},
    {"when": r"ls: /etc/os", "reply": "[[cat: /etc/os/memory.txt]]"},
    {"when": r"cat: /etc/os/memory\.txt", "reply": "All checked."},
]
for _ in range(8):
    replies.append("Mm.")

vm = vmkit.Vm(out, replies, mind={"provider": "llamacpp", "base_url": "http://10.0.2.2:8918/v1", "api_key": KEY,
                                  "model": "fake"})
results = {}
try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    time.sleep(6)
    vm.focus_companion()
    vm.say("check the locks", r"ai All checked", secs=180)
    time.sleep(3)
    sent = open(vm.ailog, encoding="utf-8", errors="replace").read()
    sh = re.search(r"VIEW: \.[^\\\n]*", sent)  # the shell's output (to its end of line, "\n" in the JSON log)
    print("   ", sh.group(0)[:300] if sh else "(no shell output)")
    results["its shell ran in its view"] = "SH-DONE" in sent and "couldn't start in its own view" not in sent
    results["the view leaves out the key and the grants"] = bool(sh) and "mind.json" not in sh.group(0) and \
        "app-grants" not in sh.group(0) and "security.conf" not in sh.group(0)
    results["the mind's key never reached it"] = KEY not in sent
    results["its own memory changed for real"] = "fence-memory-line" in sent.split("cat: /etc/os/memory.txt")[-1]
    results["its file tools refuse the key"] = sent.count("private to the system") >= 2
    ls = sent.split("ls: /etc/os")[-1][:3000]
    m = re.search(r"security\.conf[^\n]*", ls)
    print("   ", m.group(0) if m else "(no security.conf in the listing)")
    results["the security settings weren't written"] = not m or not re.search(r"\b6 B\b|\b6 bytes\b|\(6\)", m.group(0))
    vm.shot("fence.png")
    for line in re.findall(r"shell: agent: [^\n]*", vm.log_text())[-20:]:
        print("   ", line[:200])
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
