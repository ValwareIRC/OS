#!/usr/bin/env python3
"""A mind on this computer, from a brand-new first run:
  offline  no network: "On this computer" shows the built-in model ready;
           choosing it wakes llama.cpp's server and the companion answers.
  online   a small model is downloaded onto the disk, chosen, and answers.
  python3 tests/local_mind_check.py OUT_DIR offline|online"""
import os, re, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out, mode = os.path.abspath(sys.argv[1]), sys.argv[2]
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
if os.path.exists(disk):
    os.unlink(disk)
os.environ["OS_DEMO_DISK"] = disk
if mode == "offline":
    os.environ["OS_DEMO_NONET"] = "1"
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
extra = ["-fw_cfg", "name=opt/os/hangdump,string=1", "-fw_cfg", "name=opt/os/strace,string=llama-server"] if os.environ.get("LM_DEBUG") else []
q = demo.Qemu(extra)
result = {}

def rect(name):
    m = re.findall(rf"rect {re.escape(name)} (-?\d+) (-?\d+) (\d+) (\d+)", demo.log_text())
    return tuple(map(int, m[-1])) if m else None

def click_rect(name):
    r = rect(name)
    q.click(r[0] + r[2] // 2, r[1] + r[3] // 2)

try:
    demo.wait_for(r"shell: screen choose", 200)
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "1-choose.png"))
    # "On this computer": the last card of the last column (Other: custom, then builtin)
    x0 = 1600 // 2 - 640
    q.click(x0 + 4 * 256 + 120, 230 + 88 + 40)
    demo.wait_for(r"shell: screen models", 20)
    want = r"models: (online|offline)"
    result["network"] = demo.wait_for(want, 30).group(1)
    time.sleep(2)
    q.screenshot_to(os.path.join(out, "2-models.png"))
    pick = "qwen3-0.6b"
    if mode == "online":
        pick = os.environ.get("LM_PICK", "qwen3-1.7b")
        demo.wait_for(rf"rect models-{pick} ", 10)
        click_rect(f"models-{pick}")
        demo.wait_for(rf"models: downloading {pick}", 20)
        time.sleep(20)
        q.screenshot_to(os.path.join(out, "3-downloading.png"))
        result["download"] = demo.wait_for(rf"models: {pick} (downloaded|[^\n]*failed[^\n]*)|models: download of {pick} failed[^\n]*",
                                           1800).group(0)
        time.sleep(2)
    demo.wait_for(rf"rect models-{pick} ", 10)
    time.sleep(1.6)
    click_rect(f"models-{pick}")
    result["chosen"] = demo.wait_for(r"mind: [^\n]* starting on this computer[^\n]*", 30).group(0)
    result["onboarding"] = bool(demo.wait_for(r"shell: screen onboard", 300))
    # the first question is on the screen (comfort); answering it asks the model
    demo.wait_for(r"rect pick-comfort-1 ", 30)
    time.sleep(2)
    click_rect("pick-comfort-1")
    t0 = time.time()
    # its first words, from the model on this computer
    hit = demo.wait_for(r"shell: ai [^\n]+|shell: ai-error[^\n]*", 1000)
    result["first words"] = f"after {time.time() - t0:.0f} s: " + hit.group(0)[:160]
    time.sleep(3)
    q.screenshot_to(os.path.join(out, "4-onboarding.png"))
finally:
    q.quit()
for k, v in result.items():
    print(f"{k}: {v}")
