#!/usr/bin/env python3
"""Apps from Flathub survive a restart: boot 1 formats the disk's app area and
installs DOSBox from the launcher, then the VM is switched off hard (no clean
shutdown); boot 2 must check the disk, list DOSBox, and open it.
  python3 tests/persist_app.py OUT_DIR"""
import os, re, sys, time, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
if os.path.exists(disk):
    os.unlink(disk)
os.environ["OS_DEMO_DISK"] = disk
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
design = os.path.join(demo.ROOT, "tests", "designs", "xp.xml")
result = {}

def boot(n):
    demo.OUT, demo.LOG = out, os.path.join(out, f"serial-{n}.log")
    return demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"])

def launcher(q, text):
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    q.type(text)

q = boot(1)
try:
    demo.wait_for(r"shell: screen desktop", 150)
    log = demo.log_text()
    result["format"] = bool(re.search(r"formatted /dev/sda2 for apps \(ok\)", log))
    demo.wait_for(r"launcher: Flathub offers [1-9]\d* apps", 400)
    time.sleep(2)
    m = len(demo.log_text())
    launcher(q, "dosbox")
    demo.wait_for(r"launcher: installed com\.dosbox\.DOSBox|installing [^\n]* failed[^\n]*", 600, since=m)
    result["installed"] = "launcher: installed com.dosbox.DOSBox" in demo.log_text()[m:]
    demo.wait_for(r"app window \d+: [^\n]*", 120, since=m)
    time.sleep(3)
finally:
    q.quit()  # hard power-off: no sync, no clean unmount

q = boot(2)
try:
    demo.wait_for(r"shell: screen desktop", 150)
    log = demo.log_text()
    chk = re.search(r"init: checked /dev/sda2: [^\n]*", log)
    result["check"] = chk.group(0) if chk else "no check"
    time.sleep(8)
    m = len(demo.log_text())
    launcher(q, "dosbox")
    try:
        hit = demo.wait_for(r"app window \d+: [^\n]*", 120, since=m)
        result["opens"] = hit.group(0)
        time.sleep(4)
        q.screenshot_to(os.path.join(out, "after-restart.png"))
    except TimeoutError:
        result["opens"] = "NO"
    result["listed"] = "flatpak app com.dosbox.DOSBox" in demo.log_text()
finally:
    q.quit()

fsck = subprocess.run(["e2fsck", "-fn", f"{disk}?offset={320 << 20}"], capture_output=True, text=True)
result["host e2fsck"] = "clean" if fsck.returncode == 0 else "PROBLEMS:\n" + fsck.stdout[-1500:]
for k, v in result.items():
    print(f"{k}: {v}")
