#!/usr/bin/env python3
"""Boots with a kept disk and opens an app by typing its name in the launcher.
  python3 tests/open_app.py OUT_DIR DISK NAME [PROGRAM_TO_TRACE]
With a program name, the kernel logs that program's failing system calls and
every minute what each thread waits on."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out, disk, name = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3]
os.makedirs(out, exist_ok=True)
os.environ["OS_DEMO_DISK"] = disk
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
design = os.path.join(demo.ROOT, "tests", "designs", "xp.xml")
extra = []
if len(sys.argv) > 4:
    extra = ["-fw_cfg", "name=opt/os/hangdump,string=1", "-fw_cfg", f"name=opt/os/strace,string={sys.argv[4]}"]
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"] + extra)
try:
    demo.wait_for(r"shell: screen desktop", 150)
    time.sleep(8)
    m = len(demo.log_text())
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    q.type(name)  # and Enter
    try:
        print(demo.wait_for(r"app window \d+: [^\n]*", 150, since=m).group(0))
        time.sleep(4)
    except TimeoutError:
        print("NO WINDOW")
        time.sleep(70)  # one more thread dump
    q.screenshot_to(os.path.join(out, "after.png"))
finally:
    q.quit()
