#!/usr/bin/env python3
"""What the person sees with their own saved desktop: boots a copy of their
disk, photographs the desktop and its start menu, opens the launcher with
Alt+Space, finds Prism (a Flatpak) by typing and opens it.

  OS_DEMO_DISK=/tmp/copy.img python3 tests/own_disk_check.py OUT_DIR
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
q = demo.Qemu([])
fails = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what, flush=True)
    if not ok:
        fails.append(what)


try:
    demo.wait_for(r"shell: screen desktop", 180)
    time.sleep(8)
    q.screenshot_to(os.path.join(out, "1-desktop.png"))
    start = demo.last_rect("ui-start")
    if start:
        q.click(*start)
        time.sleep(1.5)
        q.screenshot_to(os.path.join(out, "2-menu.png"))
        q.key("esc")
        time.sleep(0.8)
    m = len(demo.log_text())
    q.key("alt", "spc") if False else q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    check(re.search(r"launcher: open", demo.log_text()[m:]) is not None, "Alt+Space opens search")
    m = len(demo.log_text())
    q.type("prism")  # types, then Enter
    try:
        demo.wait_for(r"shell: app window \d+: ", 300, since=m)
        check(True, "Prism opens from search")
    except TimeoutError:
        check(False, "Prism opens from search")
    time.sleep(10)
    q.screenshot_to(os.path.join(out, "4-prism.png"))
finally:
    q.quit()
print("FAIL: " + ", ".join(fails) if fails else "PASS")
sys.exit(1 if fails else 0)
