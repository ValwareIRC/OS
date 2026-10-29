#!/usr/bin/env python3
"""Boots the OS straight to a desktop from a saved design and screenshots it.

  python3 tests/preview.py DESIGN.json OUT.png [--menu] [--files]

The design is handed to the guest through QEMU's fw_cfg (see
preload_state_from_fwcfg in kernel/main.c), together with a mind config that
points at the host's Ollama through the relay, so the companion can talk.
Several screenshots are taken a moment apart to show the living wallpaper.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

design_path, out = sys.argv[1], sys.argv[2]
mind = os.path.join(demo.ROOT, "build", "preview-mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:11434/v1","api_key":"","model":"qwen3:14b"}\n')

demo.OUT = os.path.dirname(os.path.abspath(out))
demo.LOG = os.path.join(demo.OUT, "preview-serial.log")
kind = "facet.xml" if design_path.endswith(".xml") else "design.json"
extra = ["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/{kind},file={design_path}"]
q = demo.Qemu(extra)
try:
    demo.wait_for(r"shell: screen desktop", 120)
    time.sleep(10)  # let the companion say hello
    base = os.path.splitext(out)[0]
    q.screenshot_to(out)
    if "--menu" in sys.argv:
        pos = demo.last_rect("ui-start") or demo.last_rect("ui-menu-button") or demo.last_rect("dock-0") or demo.last_rect("ui-orb")
        if pos:
            q.click(*pos)
            time.sleep(1.5)
            q.screenshot_to(base + "-menu.png")
            q.key("esc")
    if "--files" in sys.argv:
        pos = demo.last_rect("desk-0") or demo.last_rect("dock-2")
        if pos:
            q.click(*pos)
            time.sleep(1.5)
            q.screenshot_to(base + "-files.png")
finally:
    q.quit()
