#!/usr/bin/env python3
"""Boots once, runs tests/prism_once_<MODE>.sh, says whether Prism's window appeared.
  python3 tests/prism_ab.py MODE OUT_DIR"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo
mode, out = sys.argv[1], os.path.abspath(sys.argv[2])
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
os.environ.setdefault("OS_DEMO_DISK", "")
script = os.path.join(demo.ROOT, "tests", f"prism_once_{mode}.sh")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}",
               "-fw_cfg", f"name=opt/os/facet.xml,file={os.path.join(demo.ROOT, 'tests', 'designs', 'xp.xml')}",
               "-fw_cfg", f"name=opt/os/autorun.sh,file={script}"])
try:
    demo.wait_for(r"shell: screen desktop", 120)
    demo.wait_for(r"PRISMONCE end", 240)
finally:
    q.quit()
print(mode, "window" if re.search(r"app window \d+: Prism", demo.log_text()) else "NO WINDOW")
