#!/usr/bin/env python3
"""Runs weston-simple-shm inside the OS and checks that its window animates.

weston-simple-shm draws every frame into a memfd it has mapped MAP_SHARED
and hands the compositor (os-shell) only the descriptor. The compositor
maps the same memfd and reads the pixels at each commit, so the window only
shows the animation if both mappings really share pages.

Boots a test desktop, opens the terminal, starts `weston-simple-shm &`, then
takes two screenshots a second apart and compares them.

  python3 tests/wl_os_test.py [out_dir]
"""
import os, re, sys, time
os.environ.setdefault("OS_QMP", "/tmp/os-wl-shm-qmp.sock")  # apart from other QEMU runs
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "wl-os-test")
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")
design = os.path.join(demo.ROOT, "tests", "designs", "terminal-test.xml")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:11434/v1","api_key":"","model":"qwen3:14b"}\n')


def read_ppm(path):
    data = open(path, "rb").read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split())
    return w, h, parts[3]


def shot(q, name):
    """Screendump to name.ppm (kept for comparison) and name.png."""
    ppm = os.path.join(out, name + ".ppm")
    if os.path.exists(ppm):
        os.unlink(ppm)
    q.cmd("screendump", filename=ppm)
    for _ in range(50):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
            break
        time.sleep(0.1)
    time.sleep(0.3)
    png = os.path.join(out, name + ".png")
    demo.ppm_to_png(ppm, png)
    print("screenshot", png, flush=True)
    return read_ppm(ppm)


def window_box(img):
    """simple-shm's 250x250 buffer has a white frame around its pattern: the
    rows and columns holding many pure white pixels bound it. (Nothing else
    on the test desktop is pure white in such numbers; if the compositor saw
    only the empty first contents of the shared memory, there is no frame.)"""
    w, h, px = img
    white = lambda x, y: px[(y * w + x) * 3:(y * w + x) * 3 + 3] == b"\xff\xff\xff"

    def longest_run(counts, need):  # the frame is one solid band; icons elsewhere are not
        best, start = (0, -1, -1), None
        for i, c in enumerate(counts + [0]):
            if c >= need and start is None:
                start = i
            elif c < need and start is not None:
                best = max(best, (i - start, start, i - 1))
                start = None
        return best[1:] if best[0] else None

    # Every row of the picture has at least the frame's two 20-pixel sides.
    rows = [sum(white(x, y) for x in range(w)) if b"\xff\xff\xff" in px[y * w * 3:(y + 1) * w * 3] else 0
            for y in range(h)]
    ys = longest_run(rows, 30)
    if not ys:
        return None
    cols = [sum(white(x, y) for y in range(ys[0], ys[1] + 1)) for x in range(w)]
    xs = longest_run(cols, 30)
    if not xs:
        return None
    return xs[0], ys[0], xs[1], ys[1]


def changed_in(a, b, box):
    w, _, pa = a
    _, _, pb = b
    x0, y0, x1, y1 = box
    n = 0
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            i = (y * w + x) * 3
            if pa[i:i + 3] != pb[i:i + 3]:
                n += 1
    return n


def colours_in(img, box):
    w, _, px = img
    x0, y0, x1, y1 = box
    seen = set()
    for y in range(y0, y1 + 1, 2):
        for x in range(x0, x1 + 1, 2):
            i = (y * w + x) * 3
            seen.add(px[i:i + 3])
    return len(seen)


q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"])
ok = False
try:
    demo.wait_for(r"shell: screen desktop", 180)
    time.sleep(4)
    pos = demo.last_rect("ui-app-terminal")
    if not pos:
        raise SystemExit("FAIL: no terminal icon on the desktop")
    q.click(*pos)
    demo.wait_for(r"terminal: shell \d+ on /dev/pts/\d+", 30)
    time.sleep(1.5)
    mark = len(demo.log_text())
    q.type("weston-simple-shm &")
    m = demo.wait_for(r"shell: app window (\d+): ([^\n]*)", 60, since=mark)
    print("compositor:", m.group(0).strip(), flush=True)
    time.sleep(4)  # let it draw for a while
    a = shot(q, "simple-shm-1")
    time.sleep(1.0)
    b = shot(q, "simple-shm-2")
    box = window_box(b)
    if not box:
        print("no simple-shm picture found on screen", flush=True)
    else:
        bw, bh = box[2] - box[0] + 1, box[3] - box[1] + 1
        n = changed_in(a, b, box)
        colours = colours_in(b, box)
        print(f"simple-shm picture at {box} ({bw}x{bh}): {n} of {bw * bh} pixels changed between the "
              f"screenshots, {colours} distinct colours", flush=True)
        # The pattern moves every frame: a large share of the picture differs a
        # second later, and it is a colourful pattern, not a flat fill.
        ok = 200 <= bw <= 300 and 200 <= bh <= 300 and n > bw * bh // 5 and colours > 50
    tail = demo.log_text()[mark:]
    if re.search(r"panic|PANIC|segfault|killed by signal", tail):
        print("log after launch shows a crash:\n" + tail[-2000:])
        ok = False
finally:
    q.quit()
print("PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
