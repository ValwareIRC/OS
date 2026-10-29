#!/usr/bin/env python3
"""The next morning: boots the OS again from a disk a person already used
(nothing injected), and records what they come back to: their own desktop,
restored from the disk, and the companion remembering them.

  OS_DEMO_DISK=build/noor-disk.img python3 tests/return_test.py [out_dir]
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "return-test")
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
if not os.environ.get("OS_DEMO_DISK"):
    print("set OS_DEMO_DISK to a disk image from an earlier run")
    sys.exit(2)
q = demo.Qemu()
ok = False
try:
    demo.wait_for(r"shell: store: restored \d+ files", 120)
    demo.wait_for(r"shell: screen desktop", 120)
    time.sleep(5)
    q.screenshot_to(os.path.join(out, "1-back.png"))
    try:
        demo.wait_for(r"shell: ai ", 240)
        time.sleep(3)
    except TimeoutError:
        pass
    q.screenshot_to(os.path.join(out, "2-welcome-back.png"))
    ok = True
finally:
    q.quit()
text = demo.log_text()
for line in text.splitlines():
    if line.startswith("shell: store") or line.startswith("shell: ai "):
        print(line[:300])
print("PASS" if ok else "FAIL")
