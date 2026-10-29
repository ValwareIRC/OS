#!/usr/bin/env python3
"""The title bar's buttons, clicked the way a person clicks: the button
held for a moment between press and release (a press and release in the
same frame used to be the only click that worked). Maximise, minimise and
close on the XP theme's windows.

  python3 tests/titlebar_test.py [OUT_DIR]
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "titlebar-test"))
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
design = os.path.join(demo.ROOT, "tests", "designs", "xp.xml")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
os.environ.setdefault("OS_DEMO_DISK", "")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"])
fails = []


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what, flush=True)
    if not ok:
        fails.append(what)


def slow_click(x, y, hold=0.15):
    q.move(x, y)
    time.sleep(0.2)
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(hold)
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(1.0)


def win_rect(w):
    r = re.findall(r"shell: rect win-%d (-?\d+) (-?\d+) (\d+) (\d+)" % w, demo.log_text())
    return tuple(map(int, r[-1])) if r else None


ICON = None


def open_about():
    global ICON
    ICON = ICON or demo.last_rect("ui-app-about")  # the desktop icon, before a taskbar item shares its name
    p = ICON
    q.click(*p)
    time.sleep(0.4)
    q.click(*p)
    time.sleep(2)
    m = re.findall(r"shell: rect win-(\d+) ", demo.log_text())
    return int(m[-1]) if m else None


try:
    demo.wait_for(r"shell: screen desktop", 120)
    time.sleep(5)
    w = open_about()
    r = win_rect(w)
    print("window", w, r, flush=True)
    # XP's buttons: 22 px, 3 px apart, 5 px in from the right, centred in the 30 px bar
    cx, cy = r[0] + r[2] - 2 - 5 - 11, r[1] + 2 + 15
    m = len(demo.log_text())
    slow_click(cx - 25, cy)  # maximise
    check(re.search(r"wm snap win-%d max" % w, demo.log_text()[m:]) is not None, "maximise on one ordinary click")
    q.screenshot_to(os.path.join(out, "1-maximised.png"))
    r = win_rect(w)
    cx, cy = r[0] + r[2] - 2 - 5 - 11, r[1] + 2 + 15
    m = len(demo.log_text())
    slow_click(cx - 25, cy)  # restore
    check(re.search(r"wm snap win-%d none" % w, demo.log_text()[m:]) is not None, "restore on one ordinary click")
    r = win_rect(w)
    cx, cy = r[0] + r[2] - 2 - 5 - 11, r[1] + 2 + 15
    m = len(demo.log_text())
    slow_click(cx - 50, cy)  # minimise
    check(re.search(r"wm minimized win-%d" % w, demo.log_text()[m:]) is not None, "minimise on one ordinary click")
    w = open_about()
    r = win_rect(w)
    cx, cy = r[0] + r[2] - 2 - 5 - 11, r[1] + 2 + 15
    m = len(demo.log_text())
    slow_click(cx, cy)  # close
    check(re.search(r"wm closed win-%d" % w, demo.log_text()[m:]) is not None, "close on one ordinary click")
    q.screenshot_to(os.path.join(out, "2-closed.png"))
finally:
    q.quit()
print("FAIL: " + ", ".join(fails) if fails else "PASS")
sys.exit(1 if fails else 0)
