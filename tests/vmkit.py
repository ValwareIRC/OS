"""What the Windows VM tests share: QEMU (Hyper-V, silent sound, the image in
build/os.iso or $ISO), QMP for keys, clicks and screenshots (never the real
mouse), the serial log, and tests/fake_ai.py as the companion."""
import json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
W, H = 1600, 900


class Vm:
    def __init__(self, out, replies, alive="proactive=0\n", design=None, qport=4474, aiport=8918, model="fake",
                 design_path=None, disk=None, mind=None, memory="12G"):
        """disk: a disk image used in snapshot mode (never written); mind: the mind.json to hand over (default:
        tests/fake_ai.py on aiport)."""
        self.out, self.qport, self.aiport, self.disk, self.memory = out, qport, aiport, disk, memory
        self.script = os.path.join(out, "replies.json")
        json.dump(replies, open(self.script, "w"))
        self.ailog = os.path.join(out, "ai.log")
        open(self.ailog, "w").close()
        self.mind = os.path.join(out, "mind.json")
        open(self.mind, "w").write(json.dumps(mind or {"provider": "llamacpp", "base_url": f"http://10.0.2.2:{aiport}/v1",
                                                       "api_key": "", "model": model}) + "\n")
        self.alive = os.path.join(out, "alive.conf")
        open(self.alive, "w").write(alive)
        self.desk = design_path or os.path.join(ROOT, "userland", "rootfs", "usr", "share", "os", "themes",
                                                (design or os.environ.get("DESIGN", "starlit")) + ".xml")
        self.log = os.path.join(out, "serial.log")
        if os.path.exists(self.log):
            os.remove(self.log)
        self.qemu = self.server = self.s = None

    def start(self):
        self.server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(self.aiport),
                                        self.script, self.ailog])

        def fw(name, path):
            return ["-fw_cfg", f"name=opt/os/{name},file={path.replace(os.sep, '/')}"]

        qargs = [QEMU, "-M", "q35,vmport=on", "-m", self.memory, "-smp", "8", "-accel", "whpx,kernel-irqchip=on", "-accel",
                 "tcg", "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none",
                 "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
                 "-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0",
                 *fw("mind.json", self.mind), *fw("facet.xml", self.desk), *fw("alive.conf", self.alive),
                 "-qmp", f"tcp:127.0.0.1:{self.qport},server,nowait", "-serial", "file:" + self.log.replace(os.sep, "/"),
                 "-cdrom", os.environ.get("ISO", os.path.join(ROOT, "build", "os.iso")).replace(os.sep, "/")]
        iso = os.environ.get("ISO", os.path.join(ROOT, "build", "os.iso"))
        if os.path.basename(iso) == "dev.iso":  # the development discs: the big files on a second one
            qargs += ["-drive", "id=big,file=" + os.path.join(os.path.dirname(iso), "big.iso").replace(os.sep, "/") +
                      ",media=cdrom,if=none,readonly=on", "-device", "ide-cd,drive=big,bus=ide.3"]
        if self.disk:
            qargs += ["-drive", "id=disk0,file=" + self.disk.replace(os.sep, "/").replace(",", ",,") +
                      ",format=raw,if=none,snapshot=on", "-device", "ide-hd,drive=disk0,bus=ide.0"]
        self.qemu = subprocess.Popen(qargs, stdout=subprocess.DEVNULL,
                                     stderr=open(os.path.join(self.out, "qemu.err"), "w"))
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", self.qport))
                break
            except OSError:
                time.sleep(0.2)
        self.f = self.s.makefile("rw")
        self.f.readline()
        self.cmd("qmp_capabilities")

    def stop(self):
        try:
            if self.s:
                self.cmd("quit")
        except Exception:
            pass
        time.sleep(1)
        if self.qemu and self.qemu.poll() is None:
            self.qemu.kill()
        if self.server:
            self.server.kill()

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
        p = os.path.join(self.out, name)
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
        names = {" ": "spc", ".": "dot", ",": "comma", "\n": "ret", "?": "slash"}
        for ch in text:
            if ch.isupper() or ch == "?":
                self.key(names.get(ch, ch.lower()), True)
            else:
                self.key(names.get(ch, ch))

    def log_text(self):
        try:
            return open(self.log, encoding="utf-8", errors="replace").read()
        except OSError:
            return ""

    def wait_for(self, pat, secs, since=0):
        t0 = time.time()
        while time.time() - t0 < secs:
            m = re.search(pat, self.log_text()[since:])
            if m:
                return m
            time.sleep(0.5)
        return None

    def say(self, text, expect, secs=90):
        m = len(self.log_text())
        self.type(text + "\n")
        hit = self.wait_for(expect, secs, m)
        print(f"  {text!r}: {hit.group(0) if hit else 'nothing seen'}", flush=True)
        time.sleep(4)
        return hit

    def focus_companion(self):
        """The conversation's input gets the keys (its window opened if it isn't)."""
        r = re.findall(r"rect companion-input (\d+) (\d+) (\d+) (\d+)", self.log_text())
        if not r:
            self.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
            time.sleep(1.5)
            self.type("talk to your companion")
            time.sleep(2.5)
            self.key("ret")
            time.sleep(3)
            r = re.findall(r"rect companion-input (\d+) (\d+) (\d+) (\d+)", self.log_text())
        if r:
            x, y, w, h = map(int, r[-1])
            self.click(x + w // 2, y + h // 2)
            time.sleep(1)
