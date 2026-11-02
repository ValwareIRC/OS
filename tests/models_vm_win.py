"""Its own models from share links, end to end, with Windows QEMU (Hyper-V)
and silent sound: tests/fake_ai.py as the companion takes a .glb and then a
.vrm from links that answer with a web page first (s.h4ks.com wants
?download=true), each must show up on screen; the state must then save to
the disk and come back on a second boot.

  python tests\\models_vm_win.py [OUTDIR]"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "models-vm"))
os.makedirs(out, exist_ok=True)
QPORT = 4471
W, H = 1600, 900
GLB = os.environ.get("GLB", "https://s.h4ks.com/K2F")
VRM = os.environ.get("VRM", "https://s.h4ks.com/K2E")

replies = [
    "Welcome back.",
    "Let me try that one on. [[emblem:model " + GLB + "]]",
    "And now this one. [[emblem:model " + VRM + "]]",
    "Still me.",
]
script = os.path.join(out, "replies.json")
json.dump(replies, open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8915
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                  "model": "fake"}) + "\n")
desk = os.path.join(ROOT, "userland", "rootfs", "usr", "share", "os", "themes", "starlit.xml")
disk = os.path.join(out, "disk.img")
with open(disk, "wb") as f:  # a new, empty disk for the store
    f.truncate(512 << 20)


def fw(name, path):
    return ["-fw_cfg", f"name=opt/os/{name},file={path.replace(os.sep, '/')}"]


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


def boot(n):
    log = os.path.join(out, f"serial{n}.log")
    if os.path.exists(log):
        os.remove(log)
    qargs = [QEMU, "-M", "q35,vmport=on", "-m", "12G", "-smp", "8", "-accel", "whpx,kernel-irqchip=on", "-accel",
             "tcg", "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none",
             "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
             "-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0",
             *fw("mind.json", mind), *fw("facet.xml", desk),
             "-drive", f"id=disk0,file={disk.replace(os.sep, '/')},format=raw,if=none",
             "-device", "ide-hd,drive=disk0,bus=ide.0",
             "-qmp", f"tcp:127.0.0.1:{QPORT},server,nowait", "-serial", "file:" + log.replace(os.sep, "/"),
             "-cdrom", os.environ.get("ISO", os.path.join(ROOT, "build", "os.iso")).replace(os.sep, "/")]
    return subprocess.Popen(qargs, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w")), log


def log_text(log):
    try:
        return open(log, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def wait_for(log, pat, secs, since=0):
    t0 = time.time()
    while time.time() - t0 < secs:
        m = re.search(pat, log_text(log)[since:])
        if m:
            return m
        time.sleep(0.5)
    return None


def report(log):
    for line in re.findall(r"shell: (?:emblem: [^\n]*|body: [^\n]*|presence: [^\n]*|store: [^\n]*)", log_text(log)):
        print("  ", line)


def stop(qemu, q):
    if q:
        try:
            q.cmd("quit")
        except Exception:
            pass
    time.sleep(1)
    if qemu.poll() is None:
        qemu.kill()


results = {}
qemu = q = None
try:
    qemu, log = boot(1)
    q = Qmp()
    results["desktop"] = bool(wait_for(log, r"shell: screen desktop", 300))
    time.sleep(10)
    q.shot("1-starlit.png")
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    q.type("talk to your companion")
    time.sleep(2.5)
    q.key("ret")
    time.sleep(3)

    m = len(log_text(log))
    q.type("wear the glb\n")
    hit = wait_for(log, r"body: fetched [^\n]*(in use|failed)", 240, m)
    results["glb fetched"] = bool(hit and "in use" in hit.group(0))
    results["glb drawn"] = bool(wait_for(log, r"os-figure: model", 90, m))
    time.sleep(25)
    q.key("esc")
    time.sleep(3)
    q.shot("2-glb.png")

    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    q.type("talk to your companion")
    time.sleep(2.5)
    q.key("ret")
    time.sleep(3)
    m = len(log_text(log))
    q.type("wear the vrm\n")
    hit = wait_for(log, r"body: fetched [^\n]*(in use|failed)", 240, m)
    results["vrm fetched"] = bool(hit and "in use" in hit.group(0))
    results["vrm is its face"] = bool(wait_for(log, r"os-figure: vrm", 90, m))
    time.sleep(25)
    q.key("esc")
    time.sleep(3)
    q.shot("3-vrm.png")

    saves = re.findall(r"store: (saved|failed)", log_text(log))
    results["saved"] = bool(saves) and "failed" not in saves
    report(log)
    stop(qemu, q)
    qemu = q = None

    qemu, log = boot(2)
    q = Qmp()
    results["restored"] = bool(wait_for(log, r"store: restored [1-9]\d* files", 300))
    results["desktop again"] = bool(wait_for(log, r"shell: screen desktop", 300))
    time.sleep(30)
    q.shot("4-after-reboot.png")
    report(log)
finally:
    if qemu:
        stop(qemu, q)
    server.kill()
    for k, v in results.items():
        print(f"{k}: {v}")
