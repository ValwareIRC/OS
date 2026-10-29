#!/usr/bin/env python3
"""The start menu that went missing (2026-10-14): boots a desktop whose Menu
button opens a #startmenu that isn't there, and checks
  1. the menu is put back by itself, and Menu opens it;
  2. the companion's long "remove the menu, add a new one" goes through
     (it was cut off at 1200 characters) and Menu opens the new one;
  3. what its tags did is told back to it;
  4. it can be renamed ([[name:...]]).
The companion is tests/fake_ai.py, answering from a script.
  python3 tests/menu_repair_check.py OUT_DIR DESKTOP.xml"""
import json, os, re, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

out, desk = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
os.makedirs(out, exist_ok=True)
demo.OUT, demo.LOG = out, os.path.join(out, "serial.log")
os.environ.setdefault("OS_DEMO_DISK", "")

buttons = "".join(f'<button what="open:{a}" icon="{a}">{a.title()} app, opened from the brand new start menu</button>'
                  for a in ["terminal", "companion", "notes", "files", "settings", "about", "terminal", "notes",
                            "files", "settings", "about", "companion", "terminal", "notes"])
long_add = ('[[ui:add #desktop <popover id="startmenu" placement="above" align="start"><column id="menucol">'
            + buttons + '<divider/><button id="poweroff" what="power:off" icon="power">Power Off</button>'
            '</column></popover>]]')
assert len(long_add) > 1300, len(long_add)
replies = [
    "Welcome back.",  # the greeting at start-up takes the first answer
    "Rebuilding the menu.\n[[ui:remove #startmenu]]\n" + long_add + "\n[[ui:set #launcher what=toggle:#startmenu]]\nHave a look in the preview.",
    "[[ui:style #startmenu { overflow: scroll; max-height: 760; }]]",  # its fix for what the screen measured
    "VIKI it is, then. [[name:VIKI]]",
]
script = os.path.join(out, "replies.json")
json.dump(replies, open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8907
server = subprocess.Popen([sys.executable, os.path.join(demo.ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                  "model": "fake"}) + "\n")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}", "-fw_cfg", f"name=opt/os/facet.xml,file={desk}"])
result = {}

def rect(name):
    m = re.findall(rf"rect {re.escape(name)} (\d+) (\d+) (\d+) (\d+)", demo.log_text())
    return tuple(map(int, m[-1])) if m else None

def press_menu(shot):
    r = rect("ui-launcher")
    q.click(r[0] + r[2] // 2, r[1] + r[3] // 2)
    time.sleep(1.5)
    q.screenshot_to(os.path.join(out, shot))
    q.key("esc")
    time.sleep(0.6)

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
    time.sleep(5)
    log = demo.log_text()
    result["repaired"] = "startmenu" in log and bool(rect("ui-launcher"))
    m = len(demo.log_text())
    press_menu("1-repaired-menu.png")
    result["menu opens (repaired)"] = bool(re.search(r"rect ui-startmenu \d+ \d+ [1-9]", demo.log_text()[m:]))

    m = say("please rebuild the start menu")
    hit = demo.wait_for(r'ui command "add #desktop[^\n]*', 60, since=m)
    result["add"] = hit.group(0)[-90:]
    time.sleep(3)
    q.screenshot_to(os.path.join(out, "2-preview.png"))
    # apply the preview (its Apply button)
    r = rect("preview-apply")
    if r:
        q.click(r[0] + r[2] // 2, r[1] + r[3] // 2)
        time.sleep(3)
    result["applied"] = "design applied from the preview" in demo.log_text()[m:]
    m2 = len(demo.log_text())
    press_menu("3-new-menu.png")
    result["menu opens (new)"] = bool(re.search(r"rect ui-menucol \d+ \d+ [1-9]", demo.log_text()[m2:]))

    m = say("can I call you VIKI")
    hit = demo.wait_for(r"companion: renamed [^\n]*", 60, since=m)
    result["rename"] = hit.group(0)
    time.sleep(3)
    q.screenshot_to(os.path.join(out, "4-renamed.png"))
finally:
    q.quit()
    server.kill()

reqs = [json.loads(l) for l in open(ailog)]
told = any("What the tags in your last reply did" in json.dumps(r["tail"]) for r in reqs)
result["tags reported back"] = told
for k, v in result.items():
    print(f"{k}: {v}")
