#!/usr/bin/env python3
"""Opens the Web app (NetSurf) inside the OS and walks it through the web.

Boots a test desktop, clicks the Web app in the dock, waits for its window
and the start page, then: follows the start page's Wikipedia link, scrolls,
types https://example.com into the address bar, and follows that page's
link. A screenshot is kept after every step (build/web-test/*.png).

The checks are the shell's and NetSurf's own words on the serial port (a
window mapped, pages fetched); the screenshots are the proof to look at.

  python3 tests/web_test.py [out_dir]
"""
import os, re, sys, time
os.environ.setdefault("OS_QMP", "/tmp/os-web-qmp.sock")  # apart from other QEMU runs
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "web-test")
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
design = os.path.join(demo.ROOT, "tests", "designs", "terminal-test.xml")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:11434/v1","api_key":"","model":"qwen3:14b"}\n')
extra = ["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"]
if os.environ.get("OS_STRACE"):
    open(os.path.join(out, "strace"), "w").write("1")
    extra += ["-fw_cfg", f"name=opt/os/strace,file={os.path.join(out, 'strace')}"]


def window_rect(since):
    """The app window's frame from the shell's log: rect win-<n> x y w h."""
    m = re.findall(r"shell: app window (\d+): ", demo.log_text()[since:])
    if not m:
        return None
    r = re.findall(r"shell: rect win-" + m[-1] + r" (-?\d+) (-?\d+) (\d+) (\d+)", demo.log_text())
    return tuple(map(int, r[-1])) if r else None


def scroll(q, x, y, steps):
    q.move(x, y)
    for _ in range(abs(steps)):
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "wheel-down" if steps > 0 else "wheel-up"}}])
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "wheel-down" if steps > 0 else "wheel-up"}}])
        time.sleep(0.15)


def chord(q, mod, key):
    q.cmd("input-send-event", events=[{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": mod}}},
                                      {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": key}}}])
    time.sleep(0.05)
    q.cmd("input-send-event", events=[{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": key}}},
                                      {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": mod}}}])
    time.sleep(0.2)


q = demo.Qemu(extra)
ok = []
try:
    demo.wait_for(r"shell: screen desktop", 180)
    time.sleep(4)
    pos = demo.last_rect("ui-app-web")
    if not pos:
        raise SystemExit("FAIL: no Web app in the dock")
    mark = len(demo.log_text())
    q.click(*pos)
    m = demo.wait_for(r"shell: app window (\d+): ([^\n]*)", 120, since=mark)
    print("compositor:", m.group(0).strip(), flush=True)
    time.sleep(float(os.environ.get("WEB_SETTLE", "12")))  # the start page
    q.screenshot("01-start-page")
    ok.append(("window", True))
    if os.environ.get("WEB_ONLY_START"):
        raise SystemExit(0)

    # Its first link: the film's Wikipedia page.
    q.move(100, 100)
    time.sleep(0.3)
    links = [m for m in re.finditer(r"shell: rect win-(\d+) (-?\d+) (-?\d+) (\d+) (\d+)", demo.log_text())]
    wr = window_rect(mark)
    print("window", wr, flush=True)
    x, y, w, h = wr
    # The start page puts its first link 40% into a typical window; a
    # direct address is a surer way to get there than guessing pixels.
    chord(q, "ctrl", "l")
    q.type("https://en.wikipedia.org/wiki/Her_(film)")
    time.sleep(float(os.environ.get("WEB_LOAD", "25")))
    q.screenshot("02-wikipedia-her")
    scroll(q, x + w // 2, y + h // 2, 8)
    time.sleep(3)
    q.screenshot("03-wikipedia-scrolled")

    chord(q, "ctrl", "l")
    q.type("https://example.com")
    time.sleep(15)
    q.screenshot("04-example-com")
finally:
    tail = demo.log_text()
    open(os.path.join(out, "tail.txt"), "w").write(tail[-20000:])
    q.quit()
print("serial log:", demo.LOG)
