#!/usr/bin/env python3
"""Chromium from Flathub, choosing a file to upload: installs it, opens a
page that is one big file input, clicks it, and reports what Chromium and
the system said (the OS's chooser should appear over Chromium).
  python3 tests/chromium_upload.py OUT_DIR"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
mind = os.path.join(out, "mind.json")
open(mind, "w").write('{"provider":"ollama","base_url":"http://10.0.2.2:1/v1","api_key":"","model":"none"}\n')
# like s.h4ks.com: several files at once, read as soon as they are chosen
import base64
html = ("<title>upload</title><input type=file multiple "
        "onchange=\"for(const f of this.files){const r=new FileReader();"
        "r.onload=()=>document.title='read '+f.name+' '+r.result.length;"
        "r.onerror=()=>document.title='error '+r.error;r.readAsText(f)}\" "
        "style='position:fixed;left:0;top:0;width:100vw;height:100vh;font-size:40px'>")
page = "data:text/html;base64," + base64.b64encode(html.encode()).decode()
page = os.environ.get("CHR_URL", page)  # a real site, and where its upload box is
click = tuple(map(int, os.environ.get("CHR_CLICK", "800,450").split(",")))
script = os.path.join(out, "try.sh")
open(script, "w").write(f"""#!/bin/sh
sleep 10
[ -f /tmp/chr-ran ] && exit 0
touch /tmp/chr-ran
mkdir -p "$HOME/Documents"; echo hello > "$HOME/Documents/hello.txt"
W=--installation=os-data
[ -e /var/lib/os-data/flatpak ] || W=--system
echo "CHR where $W"
flatpak install $W -y --noninteractive flathub org.freedesktop.Platform.GL.default//26.08 >/tmp/install.log 2>&1
flatpak install $W -y --noninteractive flathub org.chromium.Chromium >>/tmp/install.log 2>&1
echo "CHR installed $?"
tail -3 /tmp/install.log
flatpak run org.chromium.Chromium --no-first-run --start-maximized "{page}" >/tmp/app.log 2>&1 &
CPID=$!
echo "CHR started"
for i in $(seq 1 60); do
  sleep 5
  echo "CHR mem $(grep -E 'MemAvailable' /proc/meminfo)"
  if ! kill -0 $CPID 2>/dev/null; then echo "CHR exited"; break; fi
done
echo "CHR log"
tail -60 /tmp/app.log | sed 's/^/CHRLOG /'
echo "CHR end"
""")
disk = os.path.join(out, "disk.img")
if os.path.exists(disk):
    os.unlink(disk)
os.environ["OS_DEMO_DISK"] = disk
os.environ.setdefault("OS_MEM", "8G")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}",
               "-fw_cfg", f"name=opt/os/facet.xml,file={os.path.join(demo.ROOT, 'tests', 'designs', 'xp.xml')}",
               "-fw_cfg", f"name=opt/os/autorun.sh,file={script}"])
try:
    demo.wait_for(r"shell: screen desktop", 150)
    print(demo.wait_for(r"CHR installed \d+", 900).group(0), flush=True)
    m = len(demo.log_text())
    print(demo.wait_for(r"app window \d+: [^\n]*", 240, since=m).group(0), flush=True)
    time.sleep(25)
    q.screenshot_to(os.path.join(out, "1-chromium.png"))
    q.click(*click)
    time.sleep(12)
    q.screenshot_to(os.path.join(out, "2-after-click.png"))
    # into Documents, choose hello.txt, Open
    q.click(440, 283)  # the sidebar's Documents
    time.sleep(3)
    q.screenshot_to(os.path.join(out, "3-documents.png"))
    q.click(617, 297)
    time.sleep(1)
    q.click(1160, 711)
    time.sleep(15)
    q.screenshot_to(os.path.join(out, "4-picked.png"))
    time.sleep(15)
    q.screenshot_to(os.path.join(out, "5-later.png"))
    demo.wait_for(r"CHR end", 400)
finally:
    q.quit()
log = demo.log_text()
for line in log[m:].splitlines():
    if re.search(r"CHR|portal|chooser|app window|crash|segfault|killed|signal|exited", line, re.I):
        print(line[:300])
