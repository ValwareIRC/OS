#!/usr/bin/env python3
"""the OS's own offline mind: boots with the built-in model chosen (and the
network unplugged), and waits for the first onboarding words, which come
from llama.cpp running inside the OS.

  python3 tests/mind_test.py [out_dir]
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "mind-test")
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"builtin","base_url":"http://127.0.0.1:8080/v1","api_key":"","model":"os"}\n')
os.environ["OS_DEMO_DISK"] = "none"
os.environ["OS_DEMO_NONET"] = "1"  # no network at all: everything happens on this machine
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}"])
ok = False
try:
    demo.wait_for(r"shell: mind: built-in model starting", 180)
    demo.wait_for(r"shell: screen onboard", 120)
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "1-comfort.png"))
    pos = demo.last_rect("pick-comfort-1")
    if pos:
        q.click(*pos)
    t0 = time.time()
    m = demo.wait_for(r"shell: (ai|ai-error) ", 900)
    print(f"first words after {time.time() - t0:.0f} s: {m.group(0)}")
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "2-first-words.png"))
    ok = m.group(1) == "ai"
finally:
    q.quit()
for line in demo.log_text().splitlines():
    if line.startswith("shell: ai") or "mind" in line:
        print(line[:240])
print("PASS" if ok else "FAIL")
