#!/usr/bin/env python3
"""Flathub's list arrives on a fresh disk (the launcher's catalog, kept in the
disk's Flatpak installation).  python3 tests/catalog_check.py OUT_DIR"""
import os, sys
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
extra = []
if os.environ.get("CC_DEBUG"):  # every minute, what each thread waits on; flatpak's failing calls
    extra = ["-fw_cfg", "name=opt/os/hangdump,string=1", "-fw_cfg", "name=opt/os/strace,string=flatpak"]
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={design}"] + extra)
import re, time, subprocess
want = re.compile(r"launcher: Flathub offers \d+ apps|launcher: Flathub's list didn't come[^\n]*")
try:
    last, still = -1, time.time()
    while True:
        text = demo.log_text()
        hit = want.search(text)
        if hit:
            print(hit.group(0))
            break
        if len(text) != last:
            last, still = len(text), time.time()
        elif time.time() - still > 90:  # the log stopped: where is every CPU?
            regs = q.cmd("human-monitor-command", **{"command-line": "info registers -a"})
            rips = re.findall(r"RIP=([0-9a-f]+)", str(regs))
            print("STALLED; RIPs:", rips)
            elf = os.path.join(demo.ROOT, "build", "kernel.elf")
            for r in rips:
                print(r, subprocess.run(["addr2line", "-f", "-i", "-e", elf, "0x" + r], capture_output=True,
                                        text=True).stdout.replace("\n", " | "))
            break
        time.sleep(3)
finally:
    q.quit()
