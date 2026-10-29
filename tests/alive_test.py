#!/usr/bin/env python3
"""The companion's senses and hands, end to end, without real hardware.

Boots straight to a desktop with a fake PC (tests/fake_senses.py: a Ryzen,
an RTX 4080, a Corsair keyboard, two fans, a Logitech mouse) and a scripted
model, then checks that:
  - the companion's prompt carries what it senses, and nothing it doesn't;
  - it speaks up on its own when the CPU runs hot (once), and notices a new
    device;
  - [[light:]] reaches the PC; [[look:]] answers mid-reply; [[run:]] waits
    for a yes on screen and reports back; [[open:senses]] shows the panel;
    "lights follow the desktop" sends the design's colours;
  - a missing temperature and a missing service are said plainly.

  scripts/vm-slot.sh python3 tests/alive_test.py [out_dir]
Uses OS_QMP / OS_MEM like demo.py. Exit 0 when everything passed.
"""
import json, os, sys, time, urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
os.environ.setdefault("OS_QMP", "/tmp/qmp-alive.sock")
os.environ.setdefault("OS_MEM", "3G")
import demo
import fake_senses as fs

MIND_PORT, SENSES_PORT = 4590, 4591
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(demo.ROOT, "build", "alive-test")
os.makedirs(OUT, exist_ok=True)
demo.OUT, demo.LOG = OUT, os.path.join(OUT, "serial.log")
demo.QMP = os.environ["OS_QMP"]

results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok)))
    print(("PASS " if ok else "FAIL ") + name + (f"  ({detail})" if detail and not ok else ""), flush=True)


def scenario(**kv):
    req = urllib.request.Request(f"http://127.0.0.1:{SENSES_PORT}/scenario", data=json.dumps(kv).encode(),
                                 headers={"Content-Type": "application/json"}, method="POST")
    urllib.request.urlopen(req, timeout=5).read()


def say(q, text, expect_reply=True, timeout=60):
    """Types into the companion's window and waits for its answer."""
    n = len(fs.WORLD.replies)
    pos = demo.open_companion(q)
    if not pos:
        # From the start menu, as a person would.
        start = demo.last_rect("ui-start")
        if start:
            q.click(*start)
            time.sleep(1.5)
            app = demo.last_rect("ui-app-companion")
            if app:
                q.click(*app)
                time.sleep(1.5)
                pos = demo.last_rect("companion-input")
    if not pos:
        raise RuntimeError("no companion window")
    q.click(*pos)
    q.type(text)
    if expect_reply:
        end = time.time() + timeout
        while time.time() < end and len(fs.WORLD.replies) <= n:
            time.sleep(0.5)
        time.sleep(2)


def last_system():
    return fs.WORLD.prompts[-1][0] if fs.WORLD.prompts else ""


def main():
    fs.serve(fs.SensesHandler, SENSES_PORT)
    fs.serve(fs.MindHandler, MIND_PORT)
    mind = os.path.join(OUT, "mind.json")
    json.dump({"provider": "ollama", "base_url": f"http://10.0.2.2:{MIND_PORT}/v1", "api_key": "", "model": "fake",
               "senses_url": f"http://10.0.2.2:{SENSES_PORT}"}, open(mind, "w"))
    facet = os.path.join(demo.ROOT, "tests", "designs", "classic.xml")
    q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}",
                   "-fw_cfg", f"name=opt/os/facet.xml,file={facet}"])
    try:
        demo.wait_for(r"shell: screen desktop", 180)
        demo.wait_for(r"shell: ai Welcome home", 90)
        demo.wait_for(r"alive: event \(0, host\) you can feel their real PC", 30)
        check("feels the PC when the service answers", True)

        # What it's told: real readings, plainly, and its hands.
        say(q, "TEMPTEST")
        s = last_system()
        open(os.path.join(OUT, "temptest-prompt.txt"), "w").write(s)
        check("prompt has the sensing block", "What you can sense right now" in s, s[-1500:])
        check("prompt has the CPU and its temperature", "AMD Ryzen 7 7800X3D" in s and "61°C" in s)
        check("prompt has the GPU", "RTX 4080" in s and "48°C" in s)
        check("prompt has the lights, grouped", "K70 RGB PRO #ff8800" in s and "2 fans #00aaff" in s, s[-1500:])
        check("prompt says when a colour isn't reported", "G502 HERO (colour not reported)" in s)
        check("prompt documents the hands", "[[light:all #ff8800]]" in s and "[[run:command]]" in s)
        check("prompt has this computer's load", "This computer (the one you live in): CPU" in s)
        open(os.path.join(OUT, "system-prompt.txt"), "w").write(s)
        q.screenshot_to(os.path.join(OUT, "1-companion.png"))

        # A missing temperature is said plainly, with the reason.
        scenario(cpu_temp=None, missing=["CPU temperature: LibreHardwareMonitor isn't running as administrator"])
        time.sleep(5)
        say(q, "TEMPTEST")
        s = last_system()
        open(os.path.join(OUT, "temptest-prompt.txt"), "w").write(s)
        check("unknown temperature is not invented", "temperature unknown" in s and "61°C" not in s, s[-1200:])
        check("the reason is passed on", "Can't sense on the PC: CPU temperature: LibreHardwareMonitor" in s)
        scenario(cpu_temp=61.0, missing=[])

        # Hands: the keyboard turns red on the PC.
        n = len(fs.WORLD.posts)
        say(q, "LIGHTTEST")
        time.sleep(3)
        posts = fs.WORLD.posts[n:]
        check("[[light:keyboard #ff0000]] reaches the PC",
              any(p == "/lights" and b.get("target") == "keyboard" and b.get("color") == "#ff0000" for p, b in posts),
              str(posts))
        check("the action is logged", "action: lights: keyboard #ff0000" in demo.log_text())

        # A look mid-reply: asks, gets the answer, continues.
        say(q, "LOOKTEST", timeout=90)
        time.sleep(4)
        last = fs.WORLD.replies[-1]
        check("[[look:processes]] answers mid-reply", last.startswith("LOOKED") and "This computer: CPU" in last, last)
        check("the look stayed in one turn", "look (1)" in demo.log_text())

        # A file, read when asked.
        say(q, "FILETEST", timeout=90)
        time.sleep(4)
        check("[[look:file]] reads a file", fs.WORLD.replies[-1].startswith("LOOKED"), fs.WORLD.replies[-1])

        # Run: nothing happens until they say yes on screen.
        say(q, "RUNTEST")
        demo.wait_for(r"shell: run asked: echo hello-from-run", 20)
        time.sleep(2)
        q.screenshot_to(os.path.join(OUT, "2-run-confirm.png"))
        check("nothing ran before the yes", "hello-from-run" not in " ".join(r for r in fs.WORLD.replies if r.startswith("RAN")))
        yes = demo.last_rect("run-yes")
        check("the confirm card is on screen", yes is not None)
        if yes:
            n = len(fs.WORLD.replies)
            q.click(*yes)
            end = time.time() + 60
            while time.time() < end and not any(r.startswith("RAN") for r in fs.WORLD.replies[n:]):
                time.sleep(0.5)
            ran = [r for r in fs.WORLD.replies[n:] if r.startswith("RAN")]
            check("after the yes it runs and reports back", ran and "hello-from-run" in ran[0] and "exit code 0" in ran[0],
                  str(ran))
            time.sleep(2)
            q.screenshot_to(os.path.join(OUT, "3-run-reported.png"))

        # The senses panel.
        say(q, "SENSESTEST")
        time.sleep(2)
        demo.wait_for(r"shell: rect senses-panel", 10)
        check("[[open:senses]] shows the panel", True)
        q.screenshot_to(os.path.join(OUT, "4-senses-panel.png"))
        q.key("esc")
        time.sleep(1)

        # Lights follow the desktop.
        n = len(fs.WORLD.posts)
        say(q, "FOLLOWTEST")
        time.sleep(5)
        posts = fs.WORLD.posts[n:]
        check("lights follow the desktop's colours",
              any(p == "/lights" and b.get("target") == "all" and str(b.get("color", "")).startswith("#") for p, b in posts),
              str(posts))
        n = len(fs.WORLD.posts)
        say(q, "UNFOLLOWTEST")
        time.sleep(5)
        check("and are handed back when switched off", any(p == "/lights/restore" for p, b in fs.WORLD.posts[n:]),
              str(fs.WORLD.posts[n:]))

        # Speaking up: the CPU runs hot. It waits for a lull in the
        # conversation (about a minute and a half after their last word).
        scenario(add_light={"id": "vengeance", "name": "Vengeance RGB", "kind": "ram", "vendor": "Corsair", "leds": 10,
                            "color": "#ffffff", "colors": None, "controllable": True})
        demo.wait_for(r"alive: event \(1, device\) a new Corsair ram appeared", 30)
        check("notices a new device", True)
        scenario(cpu_temp=88.0)
        demo.wait_for(r"alive: event \(2, temp\) their PC's CPU is running hot: 88", 30)
        check("notices the CPU running hot", True)
        try:
            demo.wait_for(r"alive: speaking up", 150)
            demo.wait_for(r"shell: ai Your PC is running warm", 60)
            ok = True
        except TimeoutError:
            ok = False
        check("speaks up about it on its own", ok)
        time.sleep(2)
        q.screenshot_to(os.path.join(OUT, "5-spoke-up.png"))
        # ...and only once, though it stays hot.
        time.sleep(15)
        check("doesn't repeat itself", demo.log_text().count("alive: speaking up") == 1)

        # The service goes away: it says so instead of guessing.
        scenario(down=True)
        time.sleep(28)
        say(q, "TEMPTEST")
        s = last_system()
        open(os.path.join(OUT, "temptest-prompt.txt"), "w").write(s)
        # The live line says so; the 88°C may still be in the timestamped
        # history of what happened, which is true, but not as a reading now.
        pc = [l for l in s.splitlines() if l.startswith("- Their real PC")]
        check("knows when it can't feel the PC", pc and "you can't feel it right now" in pc[0] and "88°C" not in pc[0], pc)
    finally:
        q.quit()
    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
