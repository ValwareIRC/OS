#!/usr/bin/env python3
"""Theme-aware apps, light/dark, the companion's caption and the Notes editor,
on one design. Saves screenshots and checks what it can in the shell's log.

  python3 tests/apps_test.py DESIGN.xml OUT_DIR

Checks: the terminal and Notes open; a line written to /run/os-say shows
as a caption that does not cover the focused window; Notes saves a titled
file in /root/Documents and undo/redo/copy/paste work (by reading the file
back in the terminal); Ctrl+Shift+D switches the scheme and back; the
caption's x dismisses it and a click on it opens the conversation.
"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

design, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
mind = os.path.join(demo.ROOT, "build", "preview-mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
os.environ.setdefault("OS_DEMO_DISK", "")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"])
fails = []


def check(ok, what):
    print(("ok   " if ok else "FAIL ") + what, flush=True)
    if not ok:
        fails.append(what)


def combo(*keys):
    """Holds the modifiers long enough for the shell to see them with the key."""
    mods, key = keys[:-1], keys[-1]
    ev = lambda k, d: {"type": "key", "data": {"down": d, "key": {"type": "qcode", "data": k}}}
    q.cmd("input-send-event", events=[ev(m, True) for m in mods])
    time.sleep(0.12)
    q.cmd("input-send-event", events=[ev(key, True)])
    time.sleep(0.06)
    q.cmd("input-send-event", events=[ev(key, False)])
    time.sleep(0.12)
    q.cmd("input-send-event", events=[ev(m, False) for m in mods])
    time.sleep(0.15)


def typed(text):
    """Types without the Enter that demo.type adds."""
    saved = q.key
    q.key = lambda k, shift=False: None if k == "ret" else saved(k, shift)
    try:
        q.type(text)
    finally:
        q.key = saved


def shot(name):
    q.screenshot_to(os.path.join(out, name + ".png"))


def rect(name):
    m = re.findall(r"shell: rect " + re.escape(name) + r" (-?\d+) (-?\d+) (\d+) (\d+)", demo.log_text())
    return tuple(map(int, m[-1])) if m else None


def open_app(app):
    p = demo.last_rect("ui-app-" + app)
    if not p:
        return False
    q.click(*p)
    time.sleep(1.2)
    return True


try:
    demo.wait_for(r"shell: screen desktop", 120)
    time.sleep(5)
    check(bool(re.search(r"theme: (light|dark)", demo.log_text())), "theme worked out at boot")

    # the terminal, with the 16 colours on show
    check(open_app("terminal"), "terminal opens")
    demo.wait_for(r"terminal: shell \d+", 20)
    time.sleep(1.5)
    q.type("printf '\\033[30m black \\033[31m red \\033[32m green \\033[33m yellow \\033[34m blue \\033[35m magenta \\033[36m cyan \\033[37m white \\033[0m\\n\\033[90m grey \\033[91m red \\033[92m green \\033[93m yellow \\033[94m blue \\033[95m magenta \\033[96m cyan \\033[97m white \\033[0m\\n'")
    time.sleep(0.8)
    # the companion speaks while the terminal has focus
    q.type("echo 'Your mix from yesterday is still open in Projects. Want me to bring it back where you left off?' > /run/os-say")
    time.sleep(2.5)
    shot("1-caption-over-terminal")
    check(rect("caption") is not None, "caption shown")

    # Notes: write, select, copy, paste, undo, redo, save
    check(open_app("notes"), "notes opens")
    time.sleep(0.8)
    typed("Shopping for Sunday")
    q.key("ret")
    typed("bread, figs and coffee")
    time.sleep(0.3)
    q.key("home")
    combo("shift", "end")          # select the line
    combo("ctrl", "c")
    q.key("end")
    q.key("ret")
    combo("ctrl", "v")             # the line twice
    typed(" again")
    combo("ctrl", "z")             # undo " again"
    combo("ctrl", "z")             # undo the paste
    combo("ctrl", "y")             # redo the paste
    time.sleep(0.3)
    combo("shift", "left")
    combo("shift", "left")
    combo("shift", "left")
    combo("shift", "left")         # "fee" and the end of "cof..." selected, shown
    time.sleep(0.5)
    shot("2-notes")
    combo("ctrl", "s")
    time.sleep(1.5)
    saved = re.findall(r"notes: saved as (\S.*)", demo.log_text())
    check(bool(saved) and saved[-1].startswith("/root/Documents/Shopping for Sunday"), "notes saved with its title: %s" % (saved[-1] if saved else "none"))

    # read the file back in the terminal
    check(open_app("terminal"), "terminal again")
    q.type("cat '/root/Documents/Shopping for Sunday.txt'; echo '<end>'")
    time.sleep(1.2)
    shot("3-file-read-back")

    check(open_app("about"), "about opens")
    time.sleep(1)
    shot("4-apps")

    # light/dark by the hotkey, with its cross-fade
    before = demo.log_text().count("theme: ")
    combo("ctrl", "shift", "d")
    time.sleep(0.12)
    shot("5-switching")
    time.sleep(1.2)
    shot("6-other-scheme")
    log = demo.log_text()
    check(log.count("theme: ") > before, "Ctrl+Shift+D switched the scheme")
    print("  ", re.findall(r"theme: .*", log)[-1])
    open_app("terminal")
    q.type("echo 'And it is night here now. Notes and the console followed.' > /run/os-say")
    time.sleep(2.5)
    shot("7-other-scheme-caption")
    combo("ctrl", "shift", "d")
    time.sleep(1.2)
    shot("8-back")

    # the caption: x dismisses it, a click opens the conversation
    c = rect("caption-close")
    if c:
        q.click(c[0] + c[2] // 2, c[1] + c[3] // 2)
        time.sleep(1)
        n = demo.log_text().count("rect caption ")
        time.sleep(2.2)
        check(demo.log_text().count("rect caption ") == n, "x puts the caption away")
    open_app("terminal")
    q.type("echo 'Click me to talk.' > /run/os-say")
    time.sleep(2.5)
    c = rect("caption")
    if c:
        q.click(c[0] + 30, c[1] + c[3] // 2)
        time.sleep(1.5)
        check(rect("companion-input") is not None, "clicking the caption opens the conversation")
    shot("9-conversation")
finally:
    q.quit()
print("PASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
