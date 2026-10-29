#!/usr/bin/env python3
"""The VM launcher's full-screen size reaches the desktop (fw_cfg opt/os/display).
  python3 tests/display_check.py OUT_DIR WxH"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out, size = os.path.abspath(sys.argv[1]), sys.argv[2]
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
design = os.path.join(demo.ROOT, "tests", "designs", "xp.xml")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}",
               "-fw_cfg", f"name=opt/os/display,string={size}"])
try:
    demo.wait_for(r"shell: screen desktop", 150)
    hit = demo.wait_for(r"settings: display [^\n]*", 30)
    print(hit.group(0))
    time.sleep(4)
    q.screenshot_to(os.path.join(out, "desktop.png"))
finally:
    q.quit()
