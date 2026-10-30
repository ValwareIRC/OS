"""Her face in the OS, tested with Windows QEMU (Hyper-V), for when WSL has no
KVM: boots to a desktop whose orb is big enough for a face, presence "face",
and checks os-figure starts, its frames show in the orb, she follows the
pointer, and her tags in a reply ([[mood:]], [[act:]]) reach her. The
companion is tests/fake_ai.py. Run with Windows' Python:

  python tests\\figure_vm_win.py [OUTDIR]"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "figure-vm"))
os.makedirs(out, exist_ok=True)
log = os.path.join(out, "serial.log")
if os.path.exists(log):
    os.remove(log)
W, H = 1600, 900
QPORT = 4467

frost = open(os.path.join(ROOT, "userland", "rootfs", "usr", "share", "os", "themes", "frost.xml"), encoding="utf-8").read()
desk = os.path.join(out, "desk.xml")
if os.environ.get("FIG_DESK"):  # a whole design of its own (one with <avatar> and a chat strip, say)
    open(desk, "w", encoding="utf-8", newline="\n").write(open(os.environ["FIG_DESK"], encoding="utf-8").read())
else:
    open(desk, "w", encoding="utf-8", newline="\n").write(frost.replace("#heart { size:150; }", "#heart { size:%s; }" % os.environ.get("FIG_ORB", "236")))
presence = os.path.join(out, "presence")
open(presence, "w", newline="\n").write("face\n")
replies = ["Welcome back. [[mood:happy]]", os.environ.get("FIG_REPLY", "Hello! [[act:wave]]"),
           "Hmm, let me think about that. [[mood:curious]] [[act:tilt]]"]
script = os.path.join(out, "replies.json")
json.dump(replies, open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8911
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                  "model": "fake"}) + "\n")

def fw(name, path):
    return ["-fw_cfg", f"name=opt/os/{name},file={path.replace(os.sep, '/')}"]

qargs = [QEMU, "-M", "q35,vmport=on", "-m", "6G", "-smp", "4", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg",
         "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none",
         "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
         *fw("mind.json", mind), *fw("facet.xml", desk), *fw("presence", presence),
         *(fw("figure.vrm", os.environ["FIG_VRM"]) if os.environ.get("FIG_VRM") else []),
         "-qmp", f"tcp:127.0.0.1:{QPORT},server,nowait", "-serial", "file:" + log.replace(os.sep, "/"),
         "-cdrom", os.path.join(ROOT, "build", "os.iso").replace(os.sep, "/")]
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
        r = self.cmd("screendump", filename=p.replace(os.sep, "/"), format="png")
        time.sleep(0.3)
        print("screenshot", name, "ok" if "return" in r else r, flush=True)
        return p

    def move(self, x, y):
        self.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (H - 1))}}])

    def key(self, qcode, shift=False):
        evs = []
        if shift:
            evs.append({"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "shift"}}})
        evs.append({"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": qcode}}})
        self.cmd("input-send-event", events=evs)
        time.sleep(0.05)
        evs = [{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": qcode}}}]
        if shift:
            evs.append({"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "shift"}}})
        self.cmd("input-send-event", events=evs)
        time.sleep(0.05)

    def type(self, text):
        special = {" ": ("spc", False), ".": ("dot", False), ",": ("comma", False), "!": ("1", True),
                   "?": ("slash", True), "\n": ("ret", False)}
        for ch in text:
            if ch in special:
                self.key(*special[ch])
            elif ch.isupper():
                self.key(ch.lower(), True)
            else:
                self.key(ch)


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


def rect(name):
    m = re.findall(rf"rect {re.escape(name)} (\d+) (\d+) (\d+) (\d+)", log_text())
    return tuple(map(int, m[-1])) if m else None


result = {}
shots = []
q = None
try:
    q = Qmp()
    result["desktop"] = bool(wait_for(r"shell: screen desktop", 240))
    hit = wait_for(r"presence: os-figure started[^\n]*", 30)
    result["started"] = hit.group(0) if hit else None
    time.sleep(8)
    orb = rect("ui-heart") or rect("ui-face")
    result["orb"] = orb
    shots.append(q.shot("1-face.png"))
    if os.environ.get("FIG_DRAG"):  # her own corner frame, dragged somewhere else
        sx, sy = W - 220 - 28 + 110, H - 220 - 72 + 110
        q.move(sx, sy)
        time.sleep(0.3)
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        for k in range(1, 13):
            q.move(sx + (420 - sx) * k / 12, sy + (330 - sy) * k / 12)
            time.sleep(0.06)
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(1.0)
        shots.append(q.shot("1b-dragged.png"))
    if orb:
        cx, cy = orb[0] + orb[2] // 2, orb[1] + orb[3] // 2
        q.move(cx - 420, cy - 60)
        time.sleep(1.5)
        shots.append(q.shot("2-looks-left.png"))
        q.move(cx + 380, cy + 260)
        time.sleep(1.5)
        shots.append(q.shot("3-looks-right.png"))
    m = len(log_text())
    chat = rect("chat-input")
    if chat:  # the conversation is part of the desktop: type into it
        q.move(chat[0] + chat[2] // 2, chat[1] + chat[3] // 2)
        time.sleep(0.2)
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        time.sleep(0.1)
        q.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.5)
        q.type("say hello\n")
    else:
        q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
        time.sleep(1.2)
        q.type("talk to your companion")
        time.sleep(2.5)
        q.type("say hello\n")
    result["act sent"] = bool(wait_for(r"presence: act wave", 40, since=m))
    for i in range(5):
        shots.append(q.shot(f"4-wave-{i}.png"))
        time.sleep(0.3)
    text = log_text()
    result["os-figure said"] = re.findall(r"presence: os-figure: [^\n]*", text)[:6]
    result["crashed"] = "os-figure stopped" in text
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
for k, v in result.items():
    print(f"{k}: {v}")
json.dump({"orb": result.get("orb"), "shots": shots}, open(os.path.join(out, "result.json"), "w"))
