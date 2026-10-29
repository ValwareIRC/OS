#!/usr/bin/env python3
"""Opens Prism from the launcher, photographs it, then (optionally) clicks
at X,Y and photographs again, reporting what the shell logged.
  python3 tests/prism_click.py OUT_DIR [X,Y]"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo
out = os.path.abspath(sys.argv[1])
clicks = [tuple(int(v) for v in c.split(",")) for c in sys.argv[2].split(";")] if len(sys.argv) > 2 else []
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
os.environ.setdefault("OS_DEMO_DISK", "")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}",
               "-fw_cfg", f"name=opt/os/facet.xml,file={os.path.join(demo.ROOT, 'tests', 'designs', 'xp.xml')}"])
try:
    demo.wait_for(r"shell: screen desktop", 120)
    time.sleep(5)
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    m = len(demo.log_text())
    q.type("prism")
    demo.wait_for(r"shell: app window \d+: [^\n]*", 300, since=m)
    time.sleep(15)
    q.screenshot_to(os.path.join(out, "1-prism.png"))
    if clicks:
        m = len(demo.log_text())
        for c in clicks:
            q.click(*c)
            time.sleep(3)
        q.screenshot_to(os.path.join(out, "2-after-click.png"))
        time.sleep(4)
        q.screenshot_to(os.path.join(out, "3-later.png"))
        print("\n".join(l for l in demo.log_text()[m:].splitlines() if re.search(r"portal|frame|popup|window", l))[:3000])
    print("\n".join(l for l in demo.log_text().splitlines() if "draws" in l or "app window" in l))
finally:
    q.quit()
