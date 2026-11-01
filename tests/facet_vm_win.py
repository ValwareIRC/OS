"""Facet 2 apps in the OS, end to end, with Windows QEMU (Hyper-V) and silent
sound. tests/fake_ai.py plays the companion:
  she opens Daybook (a Facet app that comes with the system);
  she writes a Water app with a mistake: the problems go back to her, she
  writes it again, and the good one is saved to ~/Apps and opens.
Screenshots after each.

  python tests\\facet_vm_win.py [OUTDIR]"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "facet-vm"))
os.makedirs(out, exist_ok=True)
QPORT = 4473
W, H = 1600, 900

water = open(os.path.join(ROOT, "tests", "facet2", "water.facet"), encoding="utf-8").read()
broken = water.replace("    fs.write(", "    glasses = 3\n    fs.write(", 1)  # a missing set
replies = [
    "Hello again.",
    "Here it is. [[open:Daybook]]",
    "A water tracker, coming up. [[app:Water]]\n" + broken + "[[/app]]",
    "Fixing it now. [[app:Water]]\n" + water + "[[/app]]",
    "Still here.",
    "Still here.",
]
script = os.path.join(out, "replies.json")
json.dump(replies, open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8917
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                  "model": "fake"}) + "\n")
alive = os.path.join(out, "alive.conf")
open(alive, "w").write("proactive=0\n")  # she doesn't speak up on her own: the replies stay in step
desk = os.path.join(ROOT, "userland", "rootfs", "usr", "share", "os", "themes", os.environ.get("DESIGN", "starlit") + ".xml")
log = os.path.join(out, "serial.log")
if os.path.exists(log):
    os.remove(log)


def fw(name, path):
    return ["-fw_cfg", f"name=opt/os/{name},file={path.replace(os.sep, '/')}"]


qargs = [QEMU, "-M", "q35,vmport=on", "-m", "12G", "-smp", "8", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg",
         "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none",
         "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
         "-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0",
         *fw("mind.json", mind), *fw("facet.xml", desk), *fw("alive.conf", alive),
         "-qmp", f"tcp:127.0.0.1:{QPORT},server,nowait", "-serial", "file:" + log.replace(os.sep, "/"),
         "-cdrom", os.environ.get("ISO", os.path.join(ROOT, "build", "os.iso")).replace(os.sep, "/")]
qemu = subprocess.Popen(qargs, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w"))


class Qmp:
    def __init__(self):
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", QPORT))
                break
            except OSError:
                time.sleep(0.2)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def cmd(self, name, **args):
        self.f.write(json.dumps({"execute": name, "arguments": args}) + "\n")
        self.f.flush()
        while True:
            line = self.f.readline()
            if not line:
                return {}
            reply = json.loads(line)
            if "return" in reply or "error" in reply:
                return reply

    def shot(self, name):
        p = os.path.join(out, name)
        self.cmd("screendump", filename=p.replace(os.sep, "/"), format="png")
        time.sleep(0.4)
        print("screenshot", name, flush=True)

    def key(self, qcode, shift=False):
        keys = ([{"type": "qcode", "data": "shift"}] if shift else []) + [{"type": "qcode", "data": qcode}]
        self.cmd("send-key", keys=keys)
        time.sleep(0.06)

    def click(self, x, y):
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}])
        time.sleep(0.3)
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.1)
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])

    def type(self, text):
        names = {" ": "spc", ".": "dot", ",": "comma", "\n": "ret"}
        for ch in text:
            if ch.isupper():
                self.key(ch.lower(), True)
            else:
                self.key(names.get(ch, ch))


def log_text():
    try:
        return open(log, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def wait_for(pat, secs, since=0):
    t0 = time.time()
    while time.time() - t0 < secs:
        m = re.search(pat, log_text()[since:])
        if m:
            return m
        time.sleep(0.5)
    return None


def say(q, text, expect, secs=90):
    m = len(log_text())
    q.type(text + "\n")
    hit = wait_for(expect, secs, m)
    print(f"  {text!r}: {hit.group(0) if hit else 'nothing seen'}", flush=True)
    time.sleep(4)
    return hit


results = {}
q = None
try:
    q = Qmp()
    results["desktop"] = bool(wait_for(r"shell: screen desktop", 300))
    results["apps listed"] = bool(wait_for(r"facet|bundled app|flatpak app", 30))
    time.sleep(8)
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    q.type("talk to your companion")
    time.sleep(2.5)
    q.key("ret")
    time.sleep(3)
    results["daybook opened"] = bool(say(q, "open daybook", r"facet: opened [^\n]*Daybook"))
    time.sleep(6)
    q.shot("1-daybook.png")
    # Daybook has the keyboard now: back to the conversation's input
    r = re.findall(r"rect companion-input (\d+) (\d+) (\d+) (\d+)", log_text())
    if r:
        x, y, w, h = map(int, r[-1])
        q.click(x + w // 2, y + h // 2)
        time.sleep(1)
    results["problems sent back"] = bool(say(q, "make me a water app", r"facet: Water has \d+ problems", 60))
    results["fixed and saved"] = bool(wait_for(r"facet: saved [^\n]*Water.facet", 90))
    results["water opened"] = bool(wait_for(r"facet: opened [^\n]*Water", 30))
    time.sleep(6)
    q.shot("2-water.png")
    faults = re.findall(r"shell: facet: [^\n]*", log_text())
    for f in faults:
        print("   ", f)
finally:
    if q:
        try:
            q.cmd("quit")
        except Exception:
            pass
    time.sleep(1)
    if qemu.poll() is None:
        qemu.kill()
    server.kill()
    for k, v in results.items():
        print(f"{k}: {v}")
