#!/usr/bin/env python3
"""Inside a Flatpak sandbox a terminal opens, and a running app's instance
details survive other flatpak commands (tests/sandbox_check.sh, via autorun).
  python3 tests/sandbox_check.py OUT_DIR"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
design = os.path.join(demo.ROOT, "tests", "designs", "xp.xml")
os.environ.setdefault("OS_DEMO_DISK", "")
script = os.path.join(demo.ROOT, "tests", "sandbox_check.sh")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}",
               "-fw_cfg", f"name=opt/os/autorun.sh,file={script}"])
try:
    demo.wait_for(r"shell: screen desktop", 150)
    demo.wait_for(r"SBCHECK done", 300)
finally:
    q.quit()
for line in demo.log_text().splitlines():
    if "SBCHECK" in line:
        print(line)
