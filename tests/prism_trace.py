#!/usr/bin/env python3
"""Runs tests/prism_trace.sh on the desktop (fw_cfg autorun) and reports, per
attempt, whether Prism's window appeared, with Chromium's launch log.

  python3 tests/prism_trace.py OUT_DIR
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
design = os.path.join(demo.ROOT, "tests", "designs", "xp.xml")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
os.environ.setdefault("OS_DEMO_DISK", "")
script = os.path.join(demo.ROOT, "tests", "prism_trace.sh")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}",
               "-fw_cfg", f"name=opt/os/autorun.sh,file={script}", "-fw_cfg", "name=opt/os/hangdump,string=1"]
              + (["-fw_cfg", "name=opt/os/strace,string=" + os.environ["TRACE_APP"]] if os.environ.get("TRACE_APP") else []))
try:
    demo.wait_for(r"shell: screen desktop", 120)
    demo.wait_for(r"PRISMTRACE done", 480)
finally:
    q.quit()
log = demo.log_text()
for n in (1, 2, 3):
    m = re.search(rf"PRISMTRACE attempt {n}(.*?)PRISMTRACE end {n}", log, re.S)
    seg = m.group(1) if m else ""
    print(f"attempt {n}: window {'yes' if re.search(r'app window', seg) else 'NO'}")
