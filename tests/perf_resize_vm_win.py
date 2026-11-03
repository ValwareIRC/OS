"""How smoothly a window moves and resizes, as the desktop measures it (its "wm frames" line after each drag: the
work per frame and the time between frames). Files is opened from the dock, dragged by its title bar, then
resized from its corner, each for 1.5 s at about 60 pointer moves a second (QMP, never the real mouse). Prints
PERF lines (the Performance fixes panel reads them).  python tests\\perf_resize_vm_win.py   (ISO= the image; dev.iso)"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.join(vmkit.ROOT, "build", "perf-resize")
os.makedirs(out, exist_ok=True)
os.environ.setdefault("ISO", os.path.join(vmkit.ROOT, "build", "dev.iso"))
vm = vmkit.Vm(out, ["Okay."] * 10, memory=os.environ.get("MEM", "8G"), qport=4514, aiport=8958)
FRAMES = r"shell: wm frames (\d+): work avg ([\d.]+) ms, worst ([\d.]+) ms; frame interval avg ([\d.]+) ms"


def move(x, y):
    vm.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (vmkit.W - 1))}},
                                       {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (vmkit.H - 1))}}])


def button(down):
    vm.cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])


def drag(x0, y0, x1, y1, secs=1.5, steps=90):
    m = len(vm.log_text())
    move(x0, y0)
    time.sleep(0.15)
    button(True)
    time.sleep(0.1)
    for i in range(1, steps + 1):
        move(x0 + (x1 - x0) * i / steps, y0 + (y1 - y0) * i / steps)
        time.sleep(secs / steps)
    time.sleep(0.3)
    button(False)
    return vm.wait_for(FRAMES, 8, m)


def last_rect(name):
    r = re.findall(r"shell: rect %s (-?\d+) (-?\d+) (\d+) (\d+)" % re.escape(name), vm.log_text())
    return tuple(map(int, r[-1])) if r else None


perf = {}
try:
    vm.start()
    if not vm.wait_for(r"shell: screen desktop", 300):
        print("the desktop didn't come up")
    time.sleep(20)
    dock = last_rect("ui-app-files")
    if dock:
        vm.click(dock[0] + dock[2] // 2, dock[1] + dock[3] // 2)
    win = None
    for _ in range(30):
        time.sleep(0.5)
        wins = re.findall(r"shell: rect win-(\d+) (-?\d+) (-?\d+) (\d+) (\d+)", vm.log_text())
        if wins:
            win = tuple(map(int, wins[-1][1:]))
            break
    if not win:
        print("Files didn't open")
    else:
        x, y, w, h = win
        m = drag(x + w // 2, y + 14, x + w // 2 - 200, y + 14 + 60)
        if m:
            print("move:  ", m.group(0)[7:], flush=True)
            perf["move_frame_ms"], perf["move_interval_ms"] = m.group(2), m.group(4)
        time.sleep(1.5)
        x, y, w, h = last_rect("win-%d" % int(re.findall(r"shell: rect win-(\d+)", vm.log_text())[-1])) or (x - 200, y + 60, w, h)
        cx, cy = x + w - 3, y + h - 3
        m = drag(cx, cy, cx + 250, cy + 150)
        if m:
            print("resize:", m.group(0)[7:], flush=True)
            perf["resize_frame_ms"], perf["resize_interval_ms"], perf["resize_worst_ms"] = m.group(2), m.group(4), m.group(3)
        vm.shot("resized.png")
finally:
    vm.stop()
for k, v in perf.items():
    print(f"PERF {k}={v}")
if not perf:
    print("no frame times were logged (see build/perf-resize/serial.log)")
