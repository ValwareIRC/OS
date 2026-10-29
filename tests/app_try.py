#!/usr/bin/env python3
"""Boots once, installs a Flathub app as the person and starts it, and says
whether its window appeared within WAIT seconds.
  python3 tests/app_try.py APP_ID OUT_DIR [WAIT]"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo
app, out = sys.argv[1], os.path.abspath(sys.argv[2])
wait = int(sys.argv[3]) if len(sys.argv) > 3 else 150
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
script = os.path.join(out, "try.sh")
open(script, "w").write(f"""#!/bin/sh
sleep 10
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo >/tmp/install.log 2>&1
flatpak install --user -y --noninteractive flathub {app} >>/tmp/install.log 2>&1
echo "APPTRY installed $?"
tail -5 /tmp/install.log
flatpak run {app} >/tmp/app.log 2>&1 &
echo "APPTRY started"
sleep {wait}
echo "APPTRY end"
tail -15 /tmp/app.log
""")
os.environ.setdefault("OS_DEMO_DISK", "")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}",
               "-fw_cfg", f"name=opt/os/facet.xml,file={os.path.join(demo.ROOT, 'tests', 'designs', 'xp.xml')}",
               "-fw_cfg", f"name=opt/os/autorun.sh,file={script}"])
try:
    demo.wait_for(r"shell: screen desktop", 120)
    demo.wait_for(r"APPTRY end", 600 + wait)
finally:
    q.quit()
log = demo.log_text()
inst = re.search(r"APPTRY installed (\d+)", log)
started = log.find("APPTRY started")
win = re.search(r"app window \d+: [^\n]*", log[started:]) if started >= 0 else None
print(app, "install", inst.group(1) if inst else "?", "->", ("WINDOW " + win.group(0)) if win else "NO WINDOW")
