#!/usr/bin/env python3
"""Settings, end to end: boots a design on a fresh disk, opens Settings and
photographs every page; switches the keyboard to German and types in the
test field; changes the resolution to 1280x720 and keeps it; picks "Claude
on this PC" as the mind; then restarts on the same disk and checks the
resolution and layout came back.

  python3 tests/settings_test.py DESIGN.xml OUT_DIR
Run it through scripts/vm-slot.sh (with OS_QMP set to a socket of your own).
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

design, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.makedirs(out, exist_ok=True)
demo.OUT = out
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:11434/v1","api_key":"","model":"qwen3:14b"}\n')
disk = os.path.join(out, "disk.img")
if os.path.exists(disk):
    os.unlink(disk)
os.environ["OS_DEMO_DISK"] = disk
args = ["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"]
failures = []


def check(cond, what):
    print(("PASS " if cond else "FAIL ") + what, flush=True)
    if not cond:
        failures.append(what)


def rect_after(name, mark, timeout=6):
    end = time.time() + timeout
    while time.time() < end:
        m = re.findall(r"shell: rect " + re.escape(name) + r" (-?\d+) (-?\d+) (\d+) (\d+)", demo.log_text()[mark:])
        if m:
            x, y, w, h = map(int, m[-1])
            return x + w // 2, y + h // 2
        time.sleep(0.3)
    return None


def page(q, name, shot):
    mark = len(demo.log_text())
    p = demo.last_rect("settings-page-" + name)
    if not p:
        check(False, f"page {name} listed")
        return mark
    q.click(*p)
    time.sleep(1.6)
    q.screenshot_to(os.path.join(out, shot))
    return mark


# ---- first boot ----
demo.LOG = os.path.join(out, "serial-1.log")
demo.W, demo.H = 1600, 900
q = demo.Qemu(args)
try:
    demo.wait_for(r"shell: screen desktop", 120)
    time.sleep(6)
    p = demo.last_rect("ui-app-settings")
    check(p is not None, "settings app on the desktop")
    q.click(*p)
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "1-appearance.png"))
    check(demo.last_rect("settings-page-Appearance") is not None, "settings opens with its sidebar")

    page(q, "Sound", "2-sound.png")
    t = demo.last_rect("settings-test-sound")
    if t:
        q.click(*t)

    mark = page(q, "Keyboard", "3-keyboard.png")
    de = rect_after("settings-kb-de", mark)
    check(de is not None, "keyboard layouts listed")
    if de:
        q.click(*de)
        time.sleep(0.5)
        for k in ("z", "y", "bracket_left", "semicolon"):  # German: y z ü ö
            q.key(k)
        time.sleep(1)
        q.screenshot_to(os.path.join(out, "4-keyboard-de.png"))

    mark = page(q, "Display", "5-display.png")
    m720 = rect_after("settings-mode-1280x720", mark)
    check(m720 is not None, "resolutions listed")
    if m720:
        mark = len(demo.log_text())
        q.click(*m720)
        try:
            demo.wait_for(r"shell: display 1280x720", 10, mark)
            check(True, "switched to 1280x720")
            demo.W, demo.H = 1280, 720
            time.sleep(2)
            q.screenshot_to(os.path.join(out, "6-display-1280.png"))
            keep = rect_after("settings-keep", mark)
            check(keep is not None, "asked to keep it")
            if keep:
                q.click(*keep)
                time.sleep(1)
        except TimeoutError:
            check(False, "switched to 1280x720")

    mark = page(q, "Mind", "7-mind.png")
    ch = rect_after("settings-mind-change", mark)
    if ch:
        q.click(*ch)
        time.sleep(1.5)
        q.screenshot_to(os.path.join(out, "8-mind-choose.png"))
        mark = len(demo.log_text())
        cpc = rect_after("settings-mind-claude-pc", mark)
        check(cpc is not None, "Claude on this PC offered")
        if cpc:
            q.click(*cpc)
            time.sleep(1.2)
            mark = len(demo.log_text())
            go = rect_after("settings-mind-connect", mark)
            if go:
                q.click(*go)
                try:
                    m = demo.wait_for(r"shell: (settings: mind check failed|mind claude-pc)", 40, mark)
                    if "failed" in m.group(0):
                        time.sleep(1.5)
                        q.screenshot_to(os.path.join(out, "9-mind-unreachable.png"))
                        any_ = rect_after("settings-mind-anyway", mark)
                        if any_:
                            q.click(*any_)
                    else:
                        use = rect_after("settings-mind-use", mark, 8)
                        if use:
                            q.click(*use)
                    demo.wait_for(r"shell: mind claude-pc .*chosen in Settings", 15, mark)
                    check(True, "Claude on this PC chosen")
                except TimeoutError:
                    check(False, "Claude on this PC chosen")
            time.sleep(1.5)
            q.screenshot_to(os.path.join(out, "10-mind-now.png"))

    page(q, "Privacy", "11-privacy.png")
    page(q, "About", "12-about.png")
    page(q, "Appearance", "13-appearance-720.png")
    time.sleep(9)  # kept on the disk a few seconds after a change
finally:
    q.quit()

# ---- the same disk again ----
demo.LOG = os.path.join(out, "serial-2.log")
q = demo.Qemu(args)
try:
    demo.wait_for(r"shell: screen desktop", 120)
    text = demo.log_text()
    check("settings: display 1280x720" in text, "resolution kept across a restart")
    check("settings: keyboard de" in text, "keyboard layout kept across a restart")
    time.sleep(6)
    q.screenshot_to(os.path.join(out, "14-after-restart.png"))
finally:
    q.quit()
print("FAILED: " + ", ".join(failures) if failures else "ALL PASS", flush=True)
sys.exit(1 if failures else 0)
