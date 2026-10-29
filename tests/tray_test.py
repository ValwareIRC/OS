#!/usr/bin/env python3
"""Boots a design and exercises its system tray: a notification sent from
the terminal (toast, then the unread badge), and every tray popover the
design has (volume, network, notifications, quick settings, the clock's
calendar). Saves a screenshot of each and checks the shell's log.

  python3 tests/tray_test.py DESIGN.xml OUT_DIR
Run it through scripts/vm-slot.sh (with OS_QMP set to a socket of your own).
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

design, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:11434/v1","api_key":"","model":"qwen3:14b"}\n')
os.environ.setdefault("OS_DEMO_DISK", "none")
failures = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"])
try:
    demo.wait_for(r"shell: screen desktop", 120)
    time.sleep(7)
    q.screenshot_to(os.path.join(out, "1-desktop.png"))

    # A notification from another program, through the spool.
    term = demo.last_rect("ui-app-terminal")
    if term:
        q.click(*term)
        time.sleep(0.4)
        q.click(*term)
        time.sleep(3)
        q.type("printf 'Backup finished\\nYour Documents folder is safe on the disk.\\nfiles\\n' > /run/os/notify/a")
        try:
            demo.wait_for(r'shell: notify "Backup finished"', 15)
            check(True, "notification from the spool")
            time.sleep(1.2)
            q.screenshot_to(os.path.join(out, "2-toast.png"))
            check(demo.last_rect("toast") is not None, "toast shown")
        except TimeoutError:
            check(False, "notification from the spool")
        q.key("esc")
    else:
        print("no terminal to send a notification from; skipping the toast", flush=True)

    shots = [("volume", "tray-el-volume", "tray-volume"), ("network", "tray-el-network", "tray-network"),
             ("notifications", "tray-el-notifications", "tray-notifications"),
             ("quicksettings", "tray-el-quicksettings", "tray-quicksettings"), ("calendar", "ui-clock", "tray-calendar")]
    n = 3
    for name, el, pop in shots:
        pos = demo.last_rect(el)
        if not pos:
            print(f"(this design has no {name})", flush=True)
            continue
        mark = len(demo.log_text())
        q.click(*pos)
        try:
            demo.wait_for(r"shell: rect " + pop + " ", 6, mark)
            check(True, f"{name} popover opens")
        except TimeoutError:
            check(False, f"{name} popover opens")
        time.sleep(0.8)
        q.screenshot_to(os.path.join(out, f"{n}-{name}.png"))
        n += 1
        if name == "volume":
            # drag the slider down to about a third, which plays the test chime
            r = demo.last_rect(pop)
            import re
            m = re.findall(r"shell: rect tray-volume (-?\d+) (-?\d+) (\d+) (\d+)", demo.log_text())
            if m:
                x, y, w, h = map(int, m[-1])
                q.click(x + 74 + int((w - 94) * 0.3), y + 64 + 20)
                time.sleep(0.8)
                q.screenshot_to(os.path.join(out, f"{n}-volume-lowered.png"))
                n += 1
        if name == "calendar":
            m = __import__("re").findall(r"shell: rect tray-calendar (-?\d+) (-?\d+) (\d+) (\d+)", demo.log_text())
            if m:
                x, y, w, h = map(int, m[-1])
                q.click(x + w - 18 - 16, y + 16 + 16)  # next month
                time.sleep(0.6)
                q.screenshot_to(os.path.join(out, f"{n}-calendar-next.png"))
                n += 1
        q.click(*pos)  # the same element closes it
        time.sleep(0.6)
finally:
    q.quit()
print("FAILED: " + ", ".join(failures) if failures else "ALL PASS", flush=True)
sys.exit(1 if failures else 0)
