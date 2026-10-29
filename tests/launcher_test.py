#!/usr/bin/env python3
"""Drives the launcher through QMP on a design and photographs it.

  python3 tests/launcher_test.py DESIGN.xml OUT_DIR [panel|attached]

panel: Super opens the launcher panel (the design gives Super to nothing);
it is searched for an app, a sum, a volume, a file, a setting and a
question, and its recent items are shown. attached: the design's own
<search> (or the sheet Super opens) grows results under itself.
Every check reads the shell's own log on the serial port. Exit code 0 when
all pass.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

design, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
mode = sys.argv[3] if len(sys.argv) > 3 else "panel"
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:11434/v1","api_key":"","model":"qwen3:14b"}\n')
os.environ.setdefault("OS_DEMO_DISK", "")

SPECIAL = {" ": ("spc", False), ".": ("dot", False), ",": ("comma", False), "'": ("apostrophe", False),
           "?": ("slash", True), "-": ("minus", False), "/": ("slash", False), "*": ("8", True), "(": ("9", True),
           ")": ("0", True), "^": ("6", True), "+": ("equal", True), "%": ("5", True), ">": ("dot", True),
           ";": ("semicolon", False), "_": ("minus", True), "&": ("7", True), "=": ("equal", False)}

failures = []


def typ(q, text):
    """Types without pressing Enter."""
    for ch in text:
        if ch in SPECIAL:
            q.key(*SPECIAL[ch])
        elif ch.isalpha():
            q.key(ch.lower(), ch.isupper())
        elif ch.isdigit():
            q.key(ch)


def mark():
    return len(demo.log_text())


def check(name, pattern, since, timeout=8):
    try:
        demo.wait_for(pattern, timeout, since)
        print("PASS", name, flush=True)
        return True
    except TimeoutError:
        print("FAIL", name, "(no", repr(pattern) + ")", flush=True)
        failures.append(name)
        return False


def shot(q, name):
    time.sleep(0.8)
    q.screenshot_to(os.path.join(out, name + ".png"))


def query(q, text, top, name, png=None):
    """Opens the panel, types, and checks the top result."""
    m = mark()
    q.key("meta_l")
    check(name + ": opens", r"launcher: open", m)
    m = mark()
    typ(q, text)
    ok = check(name + ": top result", r'launcher: query "' + re.escape(text) + r'": \d+ results, top ' + top, m)
    if png:
        shot(q, png)
    return ok


q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"])
try:
    demo.wait_for(r"shell: screen desktop", 150)
    time.sleep(7)
    q.screenshot_to(os.path.join(out, "0-desktop.png"))
    if mode == "panel":
        m = mark()
        q.key("meta_l")
        check("Super opens the panel", r"launcher: open", m)
        shot(q, "1-empty")
        q.key("esc")
        check("Esc closes it", r"launcher: close", m)

        if query(q, "term", r"a terminal", "app", "2-term"):
            m = mark()
            q.key("ret")
            check("Enter opens Terminal", r"launcher: run a terminal", m)
        time.sleep(2)
        # a few things to find, made from the terminal it just opened
        q.type("mkdir -p /root/Documents/Letters && echo hello > /root/Documents/report-draft.txt && "
               "cp /usr/share/weston/background.png /root/Pictures/sunset.png; echo made")
        time.sleep(12)  # the index looks again when opened, at most every 10 s

        if query(q, "2*(3+4)^2", r"q 98", "sum", "3-sum"):
            q.key("esc")
        else:
            q.key("esc")
        query(q, "volume 30", r"c cmd:volume:30", "volume")
        m = mark()
        q.key("ret")
        check("volume is set", r"launcher: volume 30", m)

        if query(q, "report", r"f /root/Documents/report-draft.txt", "file", "4-file"):
            m = mark()
            q.key("ret")
            check("Enter opens the file", r"files: open /root/Documents/report-draft.txt", m)
        else:
            q.key("esc")
        time.sleep(1)
        query(q, "contrast", r"s set:contrast", "setting", "5-setting")
        q.key("esc")
        query(q, "dark mode", r"c cmd:dark", "command")
        q.key("esc")
        query(q, "ask what is on today?", r"\? what is on today\?", "ask", "6-ask")
        q.key("esc")
        query(q, "sunset", r"f /root/Pictures/sunset.png", "picture")
        m = mark()
        q.key("ret")
        check("a picture opens in the viewer", r"files: viewing /root/Pictures/sunset.png \(ok\)", m)
        shot(q, "7-viewer")
        q.key("esc")
        m = mark()
        q.key("meta_l")
        check("recent items with nothing typed", r'launcher: query "": \d+ results, top f /root/Pictures/sunset.png', m)
        shot(q, "8-recent")
        q.key("esc")
    else:
        # the design's own search: Super opens its sheet (with the search
        # focused), or the search is clicked where it sits
        m = mark()
        q.key("meta_l")
        time.sleep(1.2)
        if not re.search(r"launcher: open", demo.log_text()[m:]):
            pos = demo.last_rect("ui-ask") or demo.last_rect("ui-search")
            if pos:
                q.click(*pos)
                time.sleep(0.6)
        m = mark()
        typ(q, "not")
        check("results grow from the design's search", r'launcher: query "not": \d+ results, top a notes', m)
        check("drawn as a dropdown", r"shell: rect launcher-dropdown", m)
        shot(q, "1-attached")
        m = mark()
        typ(q, "es")
        check("typing narrows them", r'launcher: query "notes": \d+ results, top a notes', m)
        q.key("down")
        q.key("up")
        m = mark()
        q.key("ret")
        check("Enter opens Notes", r"launcher: run a notes", m)
        shot(q, "2-opened")
finally:
    q.quit()
print("launcher test:", "PASS" if not failures else "FAIL " + ", ".join(failures), flush=True)
sys.exit(1 if failures else 0)
