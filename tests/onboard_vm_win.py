"""A fresh onboarding, end to end, with Windows QEMU (Hyper-V) and silent
sound: tests/fake_ai.py plays the companion (a scripted conversation with a
learned fact, a room pick and a name), keys go in over QMP, and screenshots
are kept at each step: the comfort question, the conversation card, the
pick cards, the designing screen with its questions (how it should look,
which browser).

  python tests\\onboard_vm_win.py [OUTDIR]"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "onboard-vm"))
os.makedirs(out, exist_ok=True)
log = os.path.join(out, "serial.log")
if os.path.exists(log):
    os.remove(log)
QPORT = 4469

replies = [
    "Hello, and welcome. Before anything else, what should I call you?",
    "Lovely to meet you, Sam. [[user:Sam]] [[learn:rhythm|night owl, sharpest after 10pm]] "
    "[[learn:protect|evenings stay free of work]] When you're at your best, are you around people, or off alone "
    "with something?",
    "Alone with music, then. [[learn:people|recharges alone, with music]] Three places came to mind. Which feels "
    "like home? [[pick:room|Morning tiles:sun:#f3e9dc,#e0a458,#3d7ea6,#5b3a29:sunlight on blue tiles, coffee on "
    "the stove|Harbour dusk:waves:#1d2b3a,#e8a87c,#9fd3c7,#0e1621:the sea going violet after work|Paper and "
    "salt:paper:#f7f3ea,#c9a227,#4a6c6f,#2b2b2b:quiet, bright, nothing in the way]]",
    "Harbour dusk it is. [[learn:room|the harbour at dusk]] I'd like to be called Ember, if that's all right. "
    "[[name:Ember]] Now I'll make your space. [[done]]",
    "Now I'll make your space. [[done]]",
    "Now I'll make your space. [[done]]",
]
script = os.path.join(out, "replies.json")
json.dump(replies, open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8913
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                  "model": "fake"}) + "\n")


def fw(name, path):
    return ["-fw_cfg", f"name=opt/os/{name},file={path.replace(os.sep, '/')}"]


qargs = [QEMU, "-M", "q35,vmport=on", "-m", "12G", "-smp", "4", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg",
         "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none",
         "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
         "-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0",
         *fw("mind.json", mind),
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
        names = {" ": "spc", ".": "dot", ",": "comma", "\n": "ret", "'": "apostrophe"}
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


def wait_reply(n, secs=90):
    """Until the fake mind has answered n times and a moment has passed for it to show."""
    t0 = time.time()
    while time.time() - t0 < secs:
        if len(open(ailog, encoding="utf-8").read().splitlines()) >= n:
            time.sleep(3)
            return True
        time.sleep(0.5)
    return False


q = None
try:
    q = Qmp()
    print("onboarding:", bool(wait_for(r"screen onboard", 300)), flush=True)
    time.sleep(6)
    q.shot("1-comfort.png")
    q.key("1")  # the comfort question: as it is
    print("first reply:", wait_reply(1), flush=True)
    q.shot("2-hello.png")
    q.type("Sam\n")
    print("second reply:", wait_reply(2), flush=True)
    q.shot("3-card-and-chips.png")
    q.type("alone with music\n")
    print("third reply:", wait_reply(3), flush=True)
    q.shot("4-room-pick.png")
    q.key("2")  # Harbour dusk
    print("fourth reply:", wait_reply(4), flush=True)
    time.sleep(4)
    q.shot("5-name.png")
    print("designing:", bool(wait_for(r"screen designing", 60)), flush=True)
    time.sleep(8)
    q.shot("6-designing-look-question.png")
    m = len(log_text())
    q.key("1")  # nothing selected by key in the look question; click the gem instead below
    time.sleep(2)
    print("look asked:", bool(re.search(r"body: asking", log_text())), flush=True)
    print("browser asked:", bool(re.search(r"browsers: asking", log_text())), flush=True)
finally:
    text = log_text()
    for line in re.findall(r"shell: (?:screen [^\n]*|picked [^\n]*|pick shown [^\n]*|body: [^\n]*|browsers: [^\n]*|emblem: [^\n]*)", text):
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
