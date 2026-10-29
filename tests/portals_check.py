#!/usr/bin/env python3
"""The portals besides the file chooser, end to end on the desktop: an app's
sandbox reads the colour scheme, sends a notification, and asks to open a
link (tests/portals_check.sh, run as the person through fw_cfg autorun).

  python3 tests/portals_check.py OUT_DIR
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
script = os.path.join(demo.ROOT, "tests", "portals_check.sh")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}",
               "-fw_cfg", f"name=opt/os/autorun.sh,file={script}"])
try:
    demo.wait_for(r"shell: screen desktop", 120)
    try:
        demo.wait_for(r"notify \"Hello from a sandbox\"", 240)
        time.sleep(1)
        q.screenshot_to(os.path.join(out, "notification.png"))
    except TimeoutError:
        pass
    try:
        demo.wait_for(r"PORTALCHECK done", 240)
        time.sleep(3)
        q.screenshot_to(os.path.join(out, "openuri.png"))
    except TimeoutError:
        print("NO END")
    # installing from the launcher: a small app built on the system's own runtime
    q.key("esc")
    time.sleep(1)
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    m = len(demo.log_text())
    for ch in "dosbox":
        q.key(ch)
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "get-app.png"))
    q.key("ret")
    try:
        demo.wait_for(r"launcher: installed com\.dosbox\.DOSBox", 400, since=m)
        print("INSTALLED: yes")
        demo.wait_for(r"shell: app window \d+: [^\n]*", 120, since=m)
        print("APP WINDOW: yes")
        time.sleep(6)
    except TimeoutError:
        print("INSTALL/WINDOW: timed out")
    q.screenshot_to(os.path.join(out, "dosbox.png"))
    try:
        demo.wait_for(r"DOSBOXRUN end", 200)
    except TimeoutError:
        pass
    log = demo.log_text()
    for l in log.splitlines():
        if "PORTALCHECK" in l or "PULSECHECK" in l or "DOSBOX" in l or re.search(r"notify \"|portal:|launcher: (install|run|Flathub)|app window|flatpak-install", l):
            print(l[:240])
finally:
    q.quit()
