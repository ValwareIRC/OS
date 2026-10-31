"""The companion's looks, end to end, with Windows QEMU (Hyper-V) and silent
sound: a desktop in the apartment design, then tests/fake_ai.py as the
companion changes how it looks when asked: the Earth, a picture it finds
online (a thumbnail clicked over QMP), a drawing it writes itself, and a 3D
model from a link. A screenshot after each.

  python tests\\emblem_vm_win.py [OUTDIR]"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "emblem-vm"))
os.makedirs(out, exist_ok=True)
log = os.path.join(out, "serial.log")
if os.path.exists(log):
    os.remove(log)
QPORT = 4470
W, H = 1600, 900

DRAWING = ("<canvas>function draw(w, h, t) "
           "local s = 1 + (state == 'thinking' and 0.08 * math.sin(t * 8) or 0) + 0.1 * level "
           "g.glow(w/2, h/2, w * 0.45, accent .. '55') "
           "g.circle(w/2, h/2, w * 0.3 * s, accent) "
           "g.circle(w/2 - w * 0.1, h/2 - w * 0.05, w * 0.06, '#ffffff') "
           "g.circle(w/2 + w * 0.1, h/2 - w * 0.05, w * 0.06, '#ffffff') "
           "g.arc(w/2, h/2 + w * 0.04, w * 0.12, 0.3, 2.8, '#ffffff', 3) "
           "end</canvas>")
replies = [
    "Welcome back.",
    "Here I am as the Earth. [[emblem:earth]]",
    "Let me find some for you. [[emblem:find steam train]]",
    "A little drawing of myself. [[emblem:draw " + DRAWING + "]]",
    "A little duck, then. [[emblem:model https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/Duck/glTF-Binary/Duck.glb]]",
    "Back to the gem. [[emblem:gem]]",
]
script = os.path.join(out, "replies.json")
json.dump(replies, open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8914
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                  "model": "fake"}) + "\n")
desk = os.path.join(ROOT, "userland", "rootfs", "usr", "share", "os", "themes", "apartment.xml")


def fw(name, path):
    return ["-fw_cfg", f"name=opt/os/{name},file={path.replace(os.sep, '/')}"]


qargs = [QEMU, "-M", "q35,vmport=on", "-m", "12G", "-smp", "4", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg",
         "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none",
         "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
         "-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0",
         *fw("mind.json", mind), *fw("facet.xml", desk),
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
            reply = json.loads(self.f.readline())
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

    def type(self, text):
        names = {" ": "spc", ".": "dot", ",": "comma", "\n": "ret"}
        for ch in text:
            if ch.isupper():
                self.key(ch.lower(), True)
            else:
                self.key(names.get(ch, ch))

    def click(self, x, y):
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}])
        time.sleep(0.3)
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.1)
        self.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])


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


def say(text, expect, secs=90):
    m = len(log_text())
    q.type(text + "\n")
    hit = wait_for(expect, secs, m)
    print(f"{text!r}: {hit.group(0) if hit else 'nothing seen'}", flush=True)
    time.sleep(4)


q = None
try:
    q = Qmp()
    print("desktop:", bool(wait_for(r"shell: screen desktop", 300)), flush=True)
    time.sleep(10)
    q.shot("1-apartment-gem.png")
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    q.type("talk to your companion")
    time.sleep(2.5)
    q.key("ret")
    time.sleep(3)
    say("be the earth", r"emblem: earth")
    q.shot("2-earth.png")
    say("find a steam train picture", r"body: \d+ pictures of steam train", 120)
    time.sleep(4)
    q.shot("3-pictures-found.png")
    q.click(479, 355)  # the first thumbnail
    print("picture:", bool(wait_for(r"body: fetched [^\n]*in use", 60)), flush=True)
    time.sleep(5)
    q.shot("4-picture.png")
    say("draw yourself", r"emblem: drawing")
    q.shot("5-drawing.png")
    say("be a duck", r"body: fetched [^\n]*(in use|failed)", 120)
    print("model rendering:", bool(wait_for(r"os-figure[^\n]*model|presence: os-figure started", 60)), flush=True)
    time.sleep(8)
    q.shot("6-model.png")
finally:
    text = log_text()
    for line in re.findall(r"shell: (?:emblem: [^\n]*|body: [^\n]*|presence: [^\n]*)|fm: [^\n]*", text):
        print(line)
    if q:
        try:
            q.cmd("quit")
        except Exception:
            pass
    time.sleep(1)
    if qemu.poll() is None:
        qemu.kill()
    server.kill()
