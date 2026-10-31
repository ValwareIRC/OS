"""The companion API end to end, with Windows QEMU (Hyper-V): boots to a
desktop with tests/fake_ai.py as the mind, runs tests/companion_check.sh as
the person (fw_cfg autorun), which talks to the API from inside Prism's
Flatpak sandbox, answers the "wants to use your AI" prompt with Enter over
QMP, and prints how each check went. Sound goes to a silent device.

  python tests\\companion_vm_win.py [OUTDIR]"""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "companion-vm"))
os.makedirs(out, exist_ok=True)
log = os.path.join(out, "serial.log")
if os.path.exists(log):
    os.remove(log)
QPORT = 4468

desk = os.path.join(ROOT, "userland", "rootfs", "usr", "share", "os", "themes", "frost.xml")
script = os.path.join(out, "replies.json")
json.dump(["Welcome back."], open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8912
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                  "model": "fake"}) + "\n")
check = os.path.join(out, "autorun.sh")
open(check, "w", newline="\n").write(open(os.path.join(ROOT, "tests", "companion_check.sh"), encoding="utf-8").read())


def fw(name, path):
    return ["-fw_cfg", f"name=opt/os/{name},file={path.replace(os.sep, '/')}"]


qargs = [QEMU, "-M", "q35,vmport=on", "-m", "6G", "-smp", "4", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg",
         "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none",
         "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
         "-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0",
         *fw("mind.json", mind), *fw("facet.xml", desk), *fw("autorun.sh", check),
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
        time.sleep(0.3)
        return p

    def key(self, qcode):
        self.cmd("send-key", keys=[{"type": "qcode", "data": qcode}])


def log_text():
    try:
        return open(log, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def wait_for(pat, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        m = re.search(pat, log_text())
        if m:
            return m
        time.sleep(0.5)
    return None


q = None
try:
    q = Qmp()
    print("desktop:", bool(wait_for(r"shell: screen desktop", 300)), flush=True)
    print("socket:", (wait_for(r"appapi: com\.valware\.prism can reach[^\n]*", 30) or [None])[0], flush=True)
    if wait_for(r"appapi: asking whether com\.valware\.prism", 240):
        time.sleep(4)  # the request is held meanwhile
        q.shot("prompt.png")
        q.key("ret")
        print("prompt answered with Enter", flush=True)
    else:
        print("the prompt never came", flush=True)
    wait_for(r"APPCHECK done", 400)
    time.sleep(1)
finally:
    text = log_text()
    for line in re.findall(r"APPCHECK [^\n]*|shell: appapi: [^\n]*", text):
        print(line)
    print("--- the mind saw:")
    for line in open(ailog, encoding="utf-8").read().splitlines():
        print(line[:300])
    if q:
        try:
            q.cmd("quit")
        except Exception:
            pass
    time.sleep(1)
    if qemu.poll() is None:
        qemu.kill()
    server.kill()
