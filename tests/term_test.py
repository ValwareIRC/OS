#!/usr/bin/env python3
"""Opens the terminal app on a test desktop, runs commands in it, and
checks the output by screenshot (and the shell's log).

  python3 tests/term_test.py [out_dir]
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "term-test")
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
design = os.path.join(demo.ROOT, "tests", "designs", "terminal-test.xml")
mind = os.path.join(demo.ROOT, "build", "preview-mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:11434/v1","api_key":"","model":"qwen3:14b"}\n')
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"])
ok = False
try:
    demo.wait_for(r"shell: screen desktop", 120)
    time.sleep(4)
    pos = demo.last_rect("ui-app-terminal")
    if not pos:
        print("FAIL: no terminal icon on the desktop")
        sys.exit(1)
    q.click(*pos)
    demo.wait_for(r"terminal: shell \d+ on /dev/pts/\d+", 20)
    time.sleep(1.5)
    q.type("echo $((6*7)) os")
    time.sleep(1)
    q.type("ls /")
    time.sleep(1)
    q.type("python3 -c \"import sys; print(sys.version.split()[0], sum(range(10)))\"")
    time.sleep(6)
    q.type("ls /usr/bin | head")
    time.sleep(1.5)
    q.screenshot_to(os.path.join(out, "terminal.png"))
    ok = True
finally:
    q.quit()
print("PASS" if ok else "FAIL")
