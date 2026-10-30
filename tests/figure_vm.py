#!/usr/bin/env python3
"""Her face in the OS: boots to a desktop whose orb is big enough for a face,
with presence "face", and checks os-figure starts, its frames show up in the
orb, she looks towards the pointer, and her tags in a reply ([[mood:]],
[[act:]]) reach her. The companion is tests/fake_ai.py.

  python3 tests/figure_vm.py OUTDIR

Writes the screenshots and orb close-ups (a strip) to OUTDIR."""
import json, os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "figure-vm"))
os.makedirs(out, exist_ok=True)
demo.OUT = out
demo.LOG = os.path.join(out, "serial.log")

frost = open(os.path.join(demo.ROOT, "userland", "rootfs", "usr", "share", "os", "themes", "frost.xml")).read()
desk = os.path.join(out, "desk.xml")
open(desk, "w").write(frost.replace("#heart { size:150; }", "#heart { size:236; }"))
presence = os.path.join(out, "presence")
open(presence, "w").write("face\n")

replies = [
    "Welcome back. [[mood:happy]]",
    "Hello! [[act:wave]]",
    "Hmm, let me think about that. [[mood:curious]] [[act:tilt]]",
]
script = os.path.join(out, "replies.json")
json.dump(replies, open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8911
server = subprocess.Popen([sys.executable, os.path.join(demo.ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                  "model": "fake"}) + "\n")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={desk}",
               "-fw_cfg", f"name=opt/os/presence,file={presence}"])
result = {}
shots = []

def rect(name):
    m = re.findall(rf"rect {re.escape(name)} (\d+) (\d+) (\d+) (\d+)", demo.log_text())
    return tuple(map(int, m[-1])) if m else None

def shot(name):
    p = os.path.join(out, name)
    q.screenshot_to(p)
    shots.append(p)

def say(text):
    m = len(demo.log_text())
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    q.type("talk to your companion")
    time.sleep(2.5)
    q.type(text)
    return m

try:
    demo.wait_for(r"shell: screen desktop", 200)
    hit = demo.wait_for(r"presence: os-figure started[^\n]*", 30)
    result["started"] = hit.group(0) if hit else None
    time.sleep(8)
    orb = rect("ui-heart")
    result["orb"] = orb
    shot("1-face.png")
    if orb:
        cx, cy = orb[0] + orb[2] // 2, orb[1] + orb[3] // 2
        q.move(cx - 420, cy - 60)  # far to the picture's left
        time.sleep(1.5)
        shot("2-looks-left.png")
        q.move(cx + 380, cy + 260)  # down to the right
        time.sleep(1.5)
        shot("3-looks-right.png")
    m = say("say hello")
    demo.wait_for(r"companion: [^\n]*|reply[^\n]*", 40, since=m)
    time.sleep(1.0)
    for i in range(4):
        shot(f"4-wave-{i}.png")
        time.sleep(0.45)
    log = demo.log_text()
    result["errors from os-figure"] = re.findall(r"presence: os-figure: err[^\n]*", log)[:5]
    result["crashed"] = "os-figure stopped" in log
finally:
    q.quit()
    server.kill()

# the orb's neighbourhood from each screenshot, side by side
try:
    from PIL import Image
    if result.get("orb"):
        x, y, w, h = result["orb"]
        pad = 40
        crops = [Image.open(p).crop((x - pad, y - pad, x + w + pad, y + h + pad)) for p in shots]
        strip = Image.new("RGB", (sum(c.width for c in crops), crops[0].height))
        ox = 0
        for c in crops:
            strip.paste(c, (ox, 0))
            ox += c.width
        strip.save(os.path.join(out, "strip.png"))
        result["strip"] = os.path.join(out, "strip.png")
except Exception as e:
    result["strip"] = f"not made: {e}"
for k, v in result.items():
    print(f"{k}: {v}")
