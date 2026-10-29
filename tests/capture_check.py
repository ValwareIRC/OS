#!/usr/bin/env python3
"""Screenshots and screen recording, end to end: Print Screen opens the bar,
an area is dragged and taken (a PNG), then Record + Screen makes a video of a
few seconds, stopped with Print Screen again. Photographs each stage; the
files are checked on the host from the disk image afterwards.
  python3 tests/capture_check.py OUT_DIR"""
import os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
if os.path.exists(disk):
    os.unlink(disk)
os.environ["OS_DEMO_DISK"] = disk
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
design = os.path.join(demo.ROOT, "tests", "designs", "xp.xml")
script = os.path.join(out, "autorun.sh")
# inside: once a screenshot and a video exist, the screenshot goes onto the disk too
open(script, "w").write("""#!/bin/sh
for i in $(seq 1 400); do
  p=$(ls /root/Pictures/Screenshots/*.png 2>/dev/null | head -1)
  v=$(ls /var/lib/os-data/Videos/Screencasts/*.webm 2>/dev/null | head -1)
  if [ -n "$p" ] && [ -n "$v" ]; then sleep 4; cp "$p" /var/lib/os-data/Videos/shot.png && echo CAPCHECK copied; ls -l /var/lib/os-data/Videos/Screencasts; exit; fi
  sleep 1
done
echo CAPCHECK nothing to copy
""")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}",
               "-fw_cfg", f"name=opt/os/autorun.sh,file={script}"])

def bar():
    m = re.findall(r"rect capture-bar (\d+) (\d+) (\d+) (\d+)", demo.log_text())
    return tuple(map(int, m[-1])) if m else None

def drag(x0, y0, x1, y1):
    q.move(x0, y0)
    time.sleep(0.2)
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
    for i in range(1, 11):
        time.sleep(0.05)
        q.move(x0 + (x1 - x0) * i / 10, y0 + (y1 - y0) * i / 10)
    time.sleep(0.2)
    q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])

result = {}
try:
    demo.wait_for(r"shell: screen desktop", 200)
    time.sleep(6)
    # a window to be in the pictures (the launcher opens Notes)
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    q.type("new note")
    time.sleep(3)
    m = len(demo.log_text())
    q.key("print")
    result["bar"] = bool(demo.wait_for(r"capture: bar open", 20, since=m))
    time.sleep(1.5)
    q.screenshot_to(os.path.join(out, "1-bar.png"))
    drag(300, 180, 980, 560)
    time.sleep(1)
    q.screenshot_to(os.path.join(out, "2-area.png"))
    m = len(demo.log_text())
    q.key("ret")
    hit = demo.wait_for(r"capture: saved [^\n]*|capture: screenshot failed[^\n]*", 30, since=m)
    result["screenshot"] = hit.group(0)
    time.sleep(0.8)
    q.screenshot_to(os.path.join(out, "3-card.png"))
    time.sleep(1)

    # record the whole screen for a few seconds
    m = len(demo.log_text())
    q.key("print")
    demo.wait_for(r"capture: bar open", 20, since=m)
    time.sleep(1.5)
    b = bar()
    bx, by, bw, bh = b
    q.click(bx + bw - 18 - 286 + 172, by + 36)  # Record
    time.sleep(0.8)
    q.click(bx + 18 + 165, by + 36)              # Screen
    time.sleep(0.8)
    q.screenshot_to(os.path.join(out, "4-record-bar.png"))
    m = len(demo.log_text())
    q.key("ret")
    result["recording"] = demo.wait_for(r"capture: recording \d+x\d+[^\n]*", 20, since=m).group(0)
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "5-recording.png"))
    # something moving while it records: the launcher, typed into
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1)
    q.type("terminal")
    time.sleep(3)
    m = len(demo.log_text())
    q.key("print")
    result["stopped"] = demo.wait_for(r"capture: recording stopped[^\n]*", 20, since=m).group(0)
    result["saved"] = demo.wait_for(r"capture: recording saved[^\n]*", 30, since=m).group(0)
    time.sleep(1)
    q.screenshot_to(os.path.join(out, "6-video-card.png"))
    q.key("esc")
    time.sleep(0.5)
    q.key("meta_l")  # the start menu: its app lists hold apps only
    time.sleep(1.5)
    q.screenshot_to(os.path.join(out, "7-startmenu.png"))
    try:
        demo.wait_for(r"CAPCHECK copied", 60)
        time.sleep(6)  # the disk's own write-back
    except TimeoutError:
        result["copied"] = "NO"
finally:
    q.quit()

for k, v in result.items():
    print(f"{k}: {v}")
# the files, read off the disk's file system on the host
part = os.path.join(out, "part.img")
subprocess.run(["dd", f"if={disk}", f"of={part}", "bs=4M", "skip=80", "conv=sparse", "status=none"])
ls = subprocess.run(["debugfs", "-R", "ls -l /Videos/Screencasts", part], capture_output=True, text=True).stdout
print(ls)
names = re.findall(r"(Screencast [^\n]*\.webm)", ls)
if names:
    subprocess.run(["debugfs", "-R", f'dump "/Videos/Screencasts/{names[0]}" {out}/video.webm', part], capture_output=True)
subprocess.run(["debugfs", "-R", f"dump /Videos/shot.png {out}/shot.png", part], capture_output=True)
for f in ("video.webm", "shot.png"):
    p = os.path.join(out, f)
    print(f, os.path.getsize(p) if os.path.exists(p) else "missing")
os.unlink(part)
