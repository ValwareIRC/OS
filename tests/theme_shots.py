#!/usr/bin/env python3
"""Boots a design straight to the desktop and photographs it: the desktop,
its start menu open, and a couple of windows open.

  python3 tests/theme_shots.py DESIGN.xml OUT_DIR [app,app...]   (default about,terminal)
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

design, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
mind = os.path.join(demo.ROOT, "build", "preview-mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:11434/v1","api_key":"","model":"qwen3:14b"}\n')
os.environ.setdefault("OS_DEMO_DISK", "")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"])
try:
    demo.wait_for(r"shell: screen desktop", 120)
    time.sleep(6)
    q.screenshot_to(os.path.join(out, "1-desktop.png"))
    pos = demo.last_rect("ui-start")
    if pos:
        q.click(*pos)
        time.sleep(1.5)
        q.screenshot_to(os.path.join(out, "2-start-menu.png"))
        q.key("esc")
        time.sleep(0.6)
        if demo.last_rect("ui-startmenu"):
            q.click(*pos)
            time.sleep(0.6)
    apps = sys.argv[3].split(",") if len(sys.argv) > 3 else ["about", "terminal"]
    for app in ["ui-app-" + a for a in apps]:
        p = demo.last_rect(app)
        if p:
            q.click(*p)
            time.sleep(0.4)
            q.click(*p)  # desktop icons open on a click; a second is harmless
            time.sleep(1.5)
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "3-windows.png"))
finally:
    q.quit()
