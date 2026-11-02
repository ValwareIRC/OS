"""Their own name, its own look, and starting over, end to end, with Windows
QEMU (Hyper-V) and silent sound. tests/fake_ai.py plays the companion:
  boot 1, a new disk, starlit: they say what to call them ([[user:]]); a
         starter design is applied and must carry their name, never its
         sample "Sam"; it draws itself and places itself where it likes;
  boot 2, the same disk: all of it came back;
  then Settings > Privacy > Reset to factory (two clicks): the computer
  restarts at the very first screen, with nothing kept.

  python tests\\name_reset_vm_win.py [OUTDIR]"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "name-reset-vm"))
os.makedirs(out, exist_ok=True)
QPORT = 4472
W, H = 1600, 900

MOON = ("<canvas>function draw(w, h, t) "
        "local b = 0.5 + 0.5 * math.sin(t * (state == 'speaking' and 8 or 1.2)) "
        "g.glow(w/2, h/2, w * (0.45 + 0.05 * b + 0.1 * level), '#ffc24a44') "
        "local R, r, ox, oy = w * 0.3, w * 0.27, w * 0.13, -w * 0.08 "
        "local pts = {} "
        "for k = -40, 40 do local a = math.atan(-oy, -ox) + k / 40 * 2.6 "
        "local px, py = R * math.cos(a), R * math.sin(a) "
        "if (px - ox) ^ 2 + (py - oy) ^ 2 >= r * r then pts[#pts + 1] = w/2 + px; pts[#pts + 1] = h/2 + py end end "
        "for k = 40, -40, -1 do local a = math.atan(-oy, -ox) + k / 40 * 1.9 "
        "local px, py = ox + r * math.cos(a), oy + r * math.sin(a) "
        "if px * px + py * py <= R * R then pts[#pts + 1] = w/2 + px; pts[#pts + 1] = h/2 + py end end "
        "g.poly(pts, '#ffc94e') end</canvas>")
replies = [
    "Hello again.",
    "Valware it is. [[user:Valware]]",
    "Neon, coming up. [[theme:neon]]",
    "Back to the night, then. [[theme:starlit]]",
    "In a sky like this I'd like to be its moon, up in the corner. [[emblem:draw " + MOON + "]] [[emblem:place 0.82 0.17 150]]",
    "Still here.",
    "Still here.",
]
script = os.path.join(out, "replies.json")
json.dump(replies, open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8916
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                  "model": "fake"}) + "\n")
desk = os.path.join(ROOT, "userland", "rootfs", "usr", "share", "os", "themes", "starlit.xml")
disk = os.path.join(out, "disk.img")
with open(disk, "wb") as f:  # a new, empty disk
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


def boot(n, facet):
    log = os.path.join(out, f"serial{n}.log")
    if os.path.exists(log):
        os.remove(log)
    qargs = [QEMU, "-M", "q35,vmport=on", "-m", "12G", "-smp", "8", "-accel", "whpx,kernel-irqchip=on", "-accel",
             "tcg", "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none",
             "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
             "-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0",
             *fw("mind.json", mind), *(fw("facet.xml", desk) if facet else []),
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


def stop(qemu, q):
    if q:
        try:
            q.cmd("quit")
        except Exception:
            pass
    time.sleep(1)
    if qemu.poll() is None:
        qemu.kill()


def talk(q):
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    q.type("talk to your companion")
    time.sleep(2.5)
    q.key("ret")
    time.sleep(3)


def say(q, log, text, expect, secs=90):
    m = len(log_text(log))
    q.type(text + "\n")
    hit = wait_for(log, expect, secs, m)
    print(f"  {text!r}: {hit.group(0) if hit else 'nothing seen'}", flush=True)
    time.sleep(4)
    return hit


def report(log):
    for line in re.findall(r"shell: (?:emblem: [^\n]*|companion: [^\n]*|store: [^\n]*|reset: [^\n]*|screen [^\n]*)",
                           log_text(log)):
        print("    ", line)


results = {}
qemu = q = None
try:
    qemu, log = boot(1, True)
    q = Qmp()
    results["desktop"] = bool(wait_for(log, r"shell: screen desktop", 300))
    time.sleep(10)
    q.shot("1-starlit-no-look.png")
    talk(q)
    results["named"] = bool(say(q, log, "call me Valware", r"they are called Valware"))
    hit = say(q, log, "apply neon", r"applied starter design neon[^\n]*")
    results["neon keeps the name"] = bool(hit and "Sam" not in hit.group(0))
    q.key("esc")
    time.sleep(3)
    q.shot("2-neon-valware.png")
    talk(q)
    say(q, log, "back to starlit", r"applied starter design starlit")
    results["it drew itself"] = bool(say(q, log, "how about you be the moon", r"emblem: placed freely"))
    q.key("esc")
    time.sleep(6)
    q.shot("3-starlit-it-moon.png")
    time.sleep(8)
    saves = re.findall(r"store: (saved|failed)", log_text(log))
    results["saved"] = bool(saves) and "failed" not in saves
    report(log)
    stop(qemu, q)
    qemu = q = None

    qemu, log = boot(2, False)
    q = Qmp()
    results["restored"] = bool(wait_for(log, r"store: restored [1-9]\d* files", 300))
    results["desktop again"] = bool(wait_for(log, r"shell: screen desktop", 300))
    time.sleep(20)
    q.shot("4-after-reboot.png")
    # Settings > Privacy > Reset to factory, clicked twice
    q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    q.type("reset to factory")
    time.sleep(2.5)
    q.key("ret")
    time.sleep(3)
    m = wait_for(log, r"rect settings-page-Privacy (\d+) (\d+) (\d+) (\d+)", 20, len(log_text(log)) - 20000)
    if m:
        x, y, w, h = map(int, m.groups())
        q.click(x + w // 2, y + h // 2)
        time.sleep(2)
    since = len(log_text(log))
    r = wait_for(log, r"rect settings-factory-reset (\d+) (\d+) (\d+) (\d+)", 20, since)
    if r:
        x, y, w, h = map(int, r.groups())
        q.shot("5-reset-button.png")
        q.click(x + 40, y + h // 2)
        time.sleep(1.5)
        q.click(x + 40, y + h // 2)
    results["reset asked"] = bool(wait_for(log, r"reset: back to factory", 20, since))
    report(log)
    time.sleep(3)
    stop(qemu, q)
    qemu = q = None

    qemu, log = boot(3, False)
    q = Qmp()
    results["new disk after reset"] = bool(wait_for(log, r"store: a new disk", 300))
    results["apps area made new"] = bool(re.search(r"reset to factory: the apps' area is made new", log_text(log)))
    results["first screen"] = bool(wait_for(log, r"shell: screen (choose|onboard|comfort|welcome)", 120))
    time.sleep(10)
    q.shot("6-after-reset.png")
    report(log)
finally:
    if qemu:
        stop(qemu, q)
    server.kill()
    for k, v in results.items():
        print(f"{k}: {v}")
