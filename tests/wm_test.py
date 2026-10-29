#!/usr/bin/env python3
"""Window management, end to end: drags, resizes, snaps, maximises,
minimises, closes and Alt+Tabs windows through QMP on a design, screenshots
each step, and checks the results from the window rects and wm lines in the
serial log. Reports the frame times the shell measured while dragging and
resizing. With a terminal on the desktop it also runs a Wayland app
(weston-flower) and does the same to its window.

  python3 tests/wm_test.py DESIGN.xml OUT_DIR

Exit status 0 when every check passed.
"""
import os, re, sys, time
os.environ.setdefault("OS_QMP", "/tmp/qmp-wm.sock")
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

design, out = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:11434/v1","api_key":"","model":"qwen3:14b"}\n')
os.environ.setdefault("OS_DEMO_DISK", "none")
os.environ.setdefault("OS_DEMO_NONET", "1")
W, H = demo.W, demo.H

failures = []
frame_reports = []
step_no = [0]


def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what, flush=True)
    if not ok:
        failures.append(what)


def win_rect(w):
    """The window's last logged rect (x, y, w, h); None when closed or minimised."""
    rects = re.findall(r"shell: rect win-%d (-?\d+) (-?\d+) (\d+) (\d+)" % w, demo.log_text())
    if not rects:
        return None
    r = tuple(map(int, rects[-1]))
    return None if r[2] == 0 else r


def open_windows():
    out = {}
    for w in range(22):
        r = win_rect(w)
        if r:
            out[w] = r
    return out


def settle(t=0.9):
    time.sleep(t)  # the shell logs window rects every 250 ms once things settle


def shot(q, name):
    step_no[0] += 1
    q.screenshot_to(os.path.join(out, "%02d-%s.png" % (step_no[0], name)))


def mark():
    return len(demo.log_text())


def logged_since(m, pattern):
    return re.search(pattern, demo.log_text()[m:])


def press(q, x, y):
    q.move(x, y)
    time.sleep(0.08)
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(0.06)


def release(q):
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
    time.sleep(0.06)


def glide(q, x0, y0, x1, y1, steps=90, secs=1.5, hold=0.0):
    """Moves the pointer in small steps, like a hand would (about 60 a second)."""
    for i in range(1, steps + 1):
        t = i / steps
        q.move(x0 + (x1 - x0) * t, y0 + (y1 - y0) * t)
        time.sleep(secs / steps)
    time.sleep(hold)


def drag(q, x0, y0, x1, y1, name, steps=90, secs=1.5):
    m = mark()
    press(q, x0, y0)
    glide(q, x0, y0, x1, y1, steps, secs, hold=0.35)
    shot(q, name)  # before letting go: a snap preview shows here
    release(q)
    try:
        r = demo.wait_for(r"shell: wm frames (\d+): work avg ([\d.]+) ms, worst ([\d.]+) ms; frame interval avg ([\d.]+) ms, worst ([\d.]+) ms", 5, m)
        frame_reports.append((name, r.group(0)[len("shell: "):]))
        print("   ", r.group(0), flush=True)
    except TimeoutError:
        pass
    settle()


def key_down(q, k):
    q.cmd("input-send-event", events=[{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": k}}}])
    time.sleep(0.08)


def key_up(q, k):
    q.cmd("input-send-event", events=[{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": k}}}])
    time.sleep(0.08)


def combo(q, *keys):
    """Presses keys in order, then releases them in reverse (e.g. alt, f4)."""
    for k in keys:
        key_down(q, k)
    for k in reversed(keys):
        key_up(q, k)


def open_app(q, app):
    before = set(open_windows())
    p = demo.last_rect("ui-app-" + app)
    if not p:
        return None
    q.click(*p)
    for _ in range(12):
        settle(0.5)
        new = set(open_windows()) - before
        if new:
            return new.pop()
    q.click(*p)  # some designs open on a second click
    settle(1.5)
    new = set(open_windows()) - before
    return new.pop() if new else None


def near(a, b, tol=6):
    return a is not None and b is not None and all(abs(x - y) <= tol for x, y in zip(a, b))


def title_point(r):
    return r[0] + r[2] // 2, r[1] + 14


def window_tests(q, wa, wb, label):
    """Everything a person does to a window, on window wa (wb: another one)."""
    r = win_rect(wa)
    print(label, "window", wa, r, flush=True)

    # Move: drag the titlebar.
    x0, y0 = title_point(r)
    drag(q, x0, y0, x0 + 180, y0 + 90, label + "-moving")
    r2 = win_rect(wa)
    check(r2 and abs(r2[0] - (r[0] + 180)) <= 4 and abs(r2[1] - (r[1] + 90)) <= 4, "%s: drag moves the window (%s -> %s)" % (label, r, r2))
    r = r2

    # Resize from the bottom-right corner: the pointer shows it first.
    cx, cy = r[0] + r[2] - 2, r[1] + r[3] - 2
    m = mark()
    q.move(cx - 30, cy - 30)
    time.sleep(0.2)
    q.move(cx, cy)
    time.sleep(0.4)
    check(logged_since(m, r"wm cursor resize se") is not None, "%s: cursor shows a corner resize" % label)
    shot(q, label + "-resize-cursor")
    drag(q, cx, cy, cx + 140, cy + 70, label + "-resizing-corner", steps=60, secs=1.0)
    r2 = win_rect(wa)
    check(r2 and r2[0] == r[0] and r2[1] == r[1] and abs(r2[2] - (r[2] + 140)) <= 4 and abs(r2[3] - (r[3] + 70)) <= 4,
          "%s: corner resize grows it (%s -> %s)" % (label, r, r2))
    r = r2

    # Resize from the left edge (outside the frame, in the grab band).
    lx, ly = r[0] - 3, r[1] + r[3] // 2
    drag(q, lx, ly, lx - 100, ly, label + "-resizing-left", steps=50, secs=0.8)
    r2 = win_rect(wa)
    check(r2 and abs(r2[0] - (r[0] - 100)) <= 4 and abs(r2[2] - (r[2] + 100)) <= 4 and r2[1] == r[1] and r2[3] == r[3],
          "%s: left edge resize (%s -> %s)" % (label, r, r2))
    r = r2

    # Resize from the top edge, far down: the minimum size holds.
    tx, ty = r[0] + r[2] // 2, r[1] + 1
    drag(q, tx, ty, tx, ty + r[3] + 300, label + "-resizing-min", steps=50, secs=0.8)
    r2 = win_rect(wa)
    check(r2 and 140 <= r2[3] <= 240 and r2[1] + r2[3] == r[1] + r[3], "%s: minimum height kept from the top edge (%s -> %s)" % (label, r, r2))
    # back to a decent size from the bottom edge
    r = r2
    bx, by = r[0] + r[2] // 2, r[1] + r[3] - 1
    drag(q, bx, by, bx, by + 260, label + "-resizing-bottom", steps=40, secs=0.6)
    floating = win_rect(wa)

    # Snap left: drag to the left edge; the preview shows before letting go.
    x0, y0 = title_point(floating)
    m = mark()
    drag(q, x0, y0, 1, 450, label + "-snap-left-preview")
    check(logged_since(m, r"wm snap preview left") is not None, "%s: preview of the left half while dragging" % label)
    check(logged_since(m, r"wm snap win-%d left" % wa) is not None, "%s: dropped on the left edge snaps left" % label)
    settle(0.4)
    left = win_rect(wa)
    check(left and left[0] < 120 and left[2] < W // 2 and left[3] > H // 2, "%s: occupies the left half (%s)" % (label, left))
    shot(q, label + "-snapped-left")

    # Pull it out again: its own size returns, under the pointer.
    x0, y0 = left[0] + left[2] // 2, left[1] + 14
    m = mark()
    drag(q, x0, y0, x0 + 300, y0 + 120, label + "-unsnap")
    r2 = win_rect(wa)
    check(r2 and abs(r2[2] - floating[2]) <= 4 and abs(r2[3] - floating[3]) <= 4 and r2[0] <= x0 + 300 <= r2[0] + r2[2],
          "%s: dragging a snapped window restores its size under the pointer (%s)" % (label, r2))

    # Drag to the top: maximise.
    x0, y0 = title_point(r2)
    m = mark()
    drag(q, x0, y0, W // 2, 1, label + "-max-preview")
    check(logged_since(m, r"wm snap win-%d max" % wa) is not None, "%s: dropped at the top maximises" % label)
    settle(0.4)
    mx = win_rect(wa)
    check(mx and mx[2] > W - 200 and mx[3] > H - 250, "%s: maximised fills the free area (%s)" % (label, mx))
    shot(q, label + "-maximised")

    # Double-click the titlebar: restore.
    x0, y0 = title_point(mx)
    m = mark()
    q.move(x0, y0)
    time.sleep(0.1)
    for _ in range(2):
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.05)
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.07)
    settle()
    check(logged_since(m, r"wm snap win-%d none" % wa) is not None and near(win_rect(wa), r2, 8),
          "%s: double-click on the titlebar restores (%s)" % (label, win_rect(wa)))
    q.move(10, H // 2)  # off the window, so it isn't hovered

    # Keyboard: Super+Right, Super+Up (quarter), Super+Down twice, Super+Up.
    m = mark()
    combo(q, "meta_l", "right")
    settle(0.6)
    check(logged_since(m, r"wm snap win-%d right" % wa) is not None, "%s: Super+Right snaps right" % label)
    shot(q, label + "-super-right")
    combo(q, "meta_l", "up")
    settle(0.6)
    check(logged_since(m, r"wm snap win-%d top-right" % wa) is not None, "%s: then Super+Up makes a quarter" % label)
    # As in Windows 11: Down from the top quarter is the right half again,
    # and Left from the right half is the window's own size and place.
    combo(q, "meta_l", "down")
    settle(0.6)
    combo(q, "meta_l", "left")
    settle(0.6)
    check(logged_since(m, r"wm snap win-%d none" % wa) is not None, "%s: Super+Down then Super+Left steps back to its own size" % label)
    combo(q, "meta_l", "up")
    settle(0.6)
    check(logged_since(m, r"wm snap win-%d max" % wa) is not None, "%s: Super+Up maximises" % label)
    combo(q, "meta_l", "down")
    settle(0.6)
    # a Super press with arrows must not also open the launcher/companion
    check(logged_since(m, r"ui action ask") is None, "%s: Super+arrows don't count as a Super press" % label)


def main():
    q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"])
    try:
        demo.wait_for(r"shell: screen desktop", 120)
        time.sleep(6)
        shot(q, "desktop")
        apps = [a for a in ("about", "files", "notes", "settings", "terminal") if demo.last_rect("ui-app-" + a)]
        print("apps on the desktop:", apps, flush=True)
        m = mark()
        wb = open_app(q, apps[0])
        time.sleep(0.05)
        wa = open_app(q, apps[1]) if len(apps) > 1 else None
        check(wb is not None and wa is not None, "two windows open (%s, %s)" % (apps[0], apps[1] if len(apps) > 1 else "-"))
        if wa is None:
            return
        shot(q, "two-windows")

        window_tests(q, wa, wb, "builtin")

        # Alt+Tab: hold Alt, Tab once, the overlay shows; letting go switches.
        m = mark()
        key_down(q, "alt")
        key_down(q, "tab")
        key_up(q, "tab")
        time.sleep(0.5)
        shot(q, "alt-tab")
        check(logged_since(m, r"rect wm-switcher") is not None, "Alt+Tab shows the switcher")
        key_up(q, "alt")
        settle(0.6)
        check(logged_since(m, r"wm switched to win-%d" % wb) is not None, "releasing Alt switches to the previous window (win-%d)" % wb)

        # Minimise the front window (Super+Down), see it fly, bring it back with Alt+Tab.
        m = mark()
        combo(q, "meta_l", "down")
        time.sleep(0.12)
        shot(q, "minimise-flying")
        settle()
        check(logged_since(m, r"wm minimized win-%d" % wb) is not None and win_rect(wb) is None, "Super+Down minimises (win-%d)" % wb)
        shot(q, "minimised")
        m = mark()
        key_down(q, "alt")
        key_down(q, "tab")
        key_up(q, "tab")
        key_down(q, "tab")
        key_up(q, "tab")
        time.sleep(0.4)
        shot(q, "alt-tab-twice")
        key_up(q, "alt")
        time.sleep(0.15)
        shot(q, "restoring")
        settle()
        sw = logged_since(m, r"wm switched to win-(\d+)")
        check(sw is not None and int(sw.group(1)) == wb and win_rect(wb) is not None,
              "Alt+Tab+Tab reaches the minimised window and restores it (%s)" % (sw.group(0) if sw else None))

        # Alt+Shift+Tab goes the other way.
        m = mark()
        key_down(q, "alt")
        key_down(q, "shift")
        key_down(q, "tab")
        key_up(q, "tab")
        key_up(q, "shift")
        time.sleep(0.3)
        key_up(q, "alt")
        settle(0.5)
        check(logged_since(m, r"wm switcher 2 of 2") is not None, "Alt+Shift+Tab steps backwards")

        # Alt+F4 closes the front window; the other one gets the focus.
        front = max(open_windows(), key=lambda w: 0)  # placeholder, replaced below
        m = mark()
        combo(q, "alt", "f4")
        time.sleep(0.08)
        shot(q, "closing")
        settle()
        closed = logged_since(m, r"wm closed win-(\d+)")
        check(closed is not None and win_rect(int(closed.group(1))) is None, "Alt+F4 closes the front window (%s)" % (closed.group(0) if closed else None))
        check(len(open_windows()) == 1, "one window left (%s)" % open_windows())

        # A Wayland app, when there is a terminal to start it from.
        if "terminal" in apps:
            wayland_tests(q)
    finally:
        q.quit()
    print("\nframe times:")
    for name, rep in frame_reports:
        print("  %-28s %s" % (name, rep))
    print("\n%d failure(s)" % len(failures))
    for f in failures:
        print("  FAIL", f)
    sys.exit(1 if failures else 0)


def wayland_tests(q):
    term = open_app(q, "terminal")
    if term is None:
        term = [w for w in open_windows()][0]
    try:
        demo.wait_for(r"terminal: shell \d+ on /dev/pts/\d+", 30)
    except TimeoutError:
        pass
    time.sleep(1.5)
    m = mark()
    q.type("weston-flower &")
    try:
        r = demo.wait_for(r"shell: app window (\d+): ([^\n]*)", 60, since=m)
    except TimeoutError:
        check(False, "weston-flower opens a window")
        return
    w = int(r.group(1))
    settle(2.5)
    check(win_rect(w) is not None, "weston-flower window %d is shown" % w)
    shot(q, "wayland-open")
    rr = win_rect(w)
    # make it big enough to work with
    cx, cy = rr[0] + rr[2] - 2, rr[1] + rr[3] - 2
    drag(q, cx, cy, cx + 260, cy + 200, "wayland-resizing", steps=60, secs=1.0)
    r2 = win_rect(w)
    check(r2 and r2[2] >= rr[2] + 250 and r2[3] >= rr[3] + 190, "app window resizes (%s -> %s)" % (rr, r2))
    check(logged_since(m, r"wm resized win-%d" % w) is not None, "app window resize finished")
    settle(1.0)
    shot(q, "wayland-resized")
    x0, y0 = title_point(r2)
    drag(q, x0, y0, x0 - 200, y0 + 60, "wayland-moving")
    r3 = win_rect(w)
    check(r3 and abs(r3[0] - (r2[0] - 200)) <= 4, "app window moves (%s -> %s)" % (r2, r3))
    m = mark()
    x0, y0 = title_point(r3)
    drag(q, x0, y0, W - 1, 450, "wayland-snap-right")
    check(logged_since(m, r"wm snap win-%d right" % w) is not None, "app window snaps right")
    settle(1.0)
    shot(q, "wayland-snapped")
    m = mark()
    combo(q, "alt", "f4")
    settle(1.5)
    check(logged_since(m, r"wm closed win-%d" % w) is not None, "Alt+F4 asks the app to close")
    check(win_rect(w) is None, "the app closed its window")


if __name__ == "__main__":
    main()
