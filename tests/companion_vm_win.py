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
LOOK = bool(os.environ.get("LOOK"))  # Prism takes on the desktop's look, and follows a redesign
json.dump(["Welcome back.", "Here it is. [[theme:luna]]"] if LOOK else ["Welcome back."], open(script, "w"))
ailog = os.path.join(out, "ai.log")
open(ailog, "w").close()
port = 8912
server = subprocess.Popen([sys.executable, os.path.join(ROOT, "tests", "fake_ai.py"), str(port), script, ailog])
mind = os.path.join(out, "mind.json")
BUILTIN = bool(os.environ.get("BUILTIN"))  # the model that comes with the system, with no network at all
if BUILTIN:
    open(mind, "w").write(json.dumps({"provider": "builtin", "base_url": "", "api_key": "", "model": "os"}) + "\n")
else:
    open(mind, "w").write(json.dumps({"provider": "llamacpp", "base_url": f"http://10.0.2.2:{port}/v1", "api_key": "",
                                      "model": "fake"}) + "\n")
check = os.path.join(out, "autorun.sh")
if BUILTIN:
    open(check, "w", newline="\n").write(open(os.path.join(ROOT, "tests", "companion_builtin_check.sh"),
                                              encoding="utf-8").read())
elif os.environ.get("REAL_CJS"):  # Prism's own checks (its run.mjs --real), from inside its Flatpak, then Prism itself
    import base64
    b64 = base64.b64encode(open(os.environ["REAL_CJS"], "rb").read()).decode()
    open(check, "w", newline="\n").write(
        "#!/bin/sh\nsleep 20\nbase64 -d > /root/real.cjs <<'B64'\n" + b64 + "\nB64\n"
        "flatpak run --command=sh com.valware.prism -c 'ELECTRON_RUN_AS_NODE=1 /app/prism/prism /root/real.cjs' 2>&1"
        " | grep -v 'document portal' | sed 's/^/REALCHECK /'\n"
        "echo 'APPCHECK done'\n"
        "flatpak run com.valware.prism >/tmp/prism.log 2>&1 &\n"
        "sleep 90\necho \"PRISMLOG $(tail -c 1500 /tmp/prism.log | tr '\\n' '|')\"\n")
elif LOOK:
    open(check, "w", newline="\n").write("#!/bin/sh\nsleep 20\nflatpak run com.valware.prism >/tmp/prism.log 2>&1 &\n"
                                          "sleep 600\n")
elif os.environ.get("AGENT"):  # Prism's agent takes one turn on the OS's AI, driven over its test-only debug port
    import base64
    cdp = base64.b64encode(open(os.environ.get("CDP_MJS", os.path.join(ROOT, "..", "prism-work", "scripts", "room-test",
                                                                       "cdp.mjs")), "rb").read()).decode()
    start = ("(async () => { await prism.workspace.set('/root/Projects/ostest'); await new Promise(r => setTimeout(r, 2500));"
             " const s = await prism.sessions.create('OS test');"
             " await prism.agent.send(s.id, 'Read README.md and tell me its first line.',"
             " { model: { endpointId: 'os-companion', modelId: 'os' }, mode: 'auto-edit' }); return s.id })()")
    open(check, "w", newline="\n").write(
        "#!/bin/sh\nsleep 20\nmkdir -p /root/Projects/ostest\n"
        "printf 'Lighthouse keeper notes\\nsecond line\\n' > /root/Projects/ostest/README.md\n"
        "base64 -d > /root/cdp.mjs <<'B64'\n" + cdp + "\nB64\n"
        "cat > /root/turn-start.js <<'JS'\n" + start + "\nJS\n"
        "flatpak run --env=PRISM_DEBUG_PORT=9223 com.valware.prism >/tmp/prism.log 2>&1 &\n"
        "sleep 70\n"
        "CDP='ELECTRON_RUN_AS_NODE=1 /app/prism/prism /root/cdp.mjs eval'\n"
        "ID=$(flatpak run --command=sh com.valware.prism -c \"$CDP \\\"\\$(cat /root/turn-start.js)\\\"\" 2>/dev/null | tr -d '\"\\n ')\n"
        "echo \"AGENTCHECK session $ID\"\n"
        "sleep 45\n"
        "echo \"prism.sessions.items('$ID').then(i => JSON.stringify(i).slice(-3500))\" > /root/turn-items.js\n"
        "flatpak run --command=sh com.valware.prism -c \"$CDP \\\"\\$(cat /root/turn-items.js)\\\"\" 2>&1 | tr '\\n' ' ' | sed 's/^/AGENTCHECK items /'\n"
        "echo\necho 'APPCHECK done'\n")
else:
    open(check, "w", newline="\n").write(open(os.path.join(ROOT, "tests", "companion_check.sh"), encoding="utf-8").read())
presence = os.path.join(out, "presence")
open(presence, "w", newline="\n").write("face\n")


def fw(name, path):
    return ["-fw_cfg", f"name=opt/os/{name},file={path.replace(os.sep, '/')}"]


qargs = [QEMU, "-M", "q35,vmport=on", "-m", "12G", "-smp", "8" if BUILTIN else "4", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg",
         "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none",
         *(["-nic", "none"] if BUILTIN else ["-netdev", "user,id=n0", "-device", "e1000,netdev=n0"]),
         "-audiodev", "none,id=a0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=a0",
         *fw("mind.json", mind), *fw("facet.xml", desk), *fw("autorun.sh", check),
         *(fw("figure.vrm", os.environ["VRM"]) + fw("presence", presence) if os.environ.get("VRM") else []),
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

    def type(self, text):
        names = {" ": "spc", ".": "dot", ",": "comma", "\n": "ret"}
        for ch in text:
            if ch.isupper():
                self.cmd("send-key", keys=[{"type": "qcode", "data": "shift"}, {"type": "qcode", "data": ch.lower()}])
            else:
                self.key(names.get(ch, ch))
            time.sleep(0.05)


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
    if not LOOK:
        wait_for(r"APPCHECK done", 1500 if BUILTIN else 400)
    time.sleep(1)
    if os.environ.get("REAL_CJS"):  # Prism itself, opened on the desktop
        wait_for(r"PRISMLOG", 150)
        q.shot("prism.png")
    if LOOK:  # Prism as it opens in the design, then after the companion switches to Luna
        time.sleep(45)
        q.shot("look-1-frost.png")
        q.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
        time.sleep(1.5)
        q.type("talk to your companion")
        time.sleep(2.5)
        q.type("make it look like luna\n")
        print("theme asked:", bool(wait_for(r"theme[^\n]*luna|luna", 90)), flush=True)
        time.sleep(12)
        q.shot("look-2-luna.png")
    if os.environ.get("AGENT"):
        q.shot("agent.png")
finally:
    text = log_text()
    for line in re.findall(r"APPCHECK [^\n]*|REALCHECK [^\n]*|AGENTCHECK [^\n]*|PRISMLOG [^\n]*|shell: appapi: [^\n]*"
                           r"|shell: models: [^\n]*|shell: mind: [^\n]*|mm: [0-9]+ MiB[^\n]*|shell: browsers: [^\n]*"
                           r"|wayland: [^\n]*|[^\n]*theme[^\n]*luna[^\n]*", text):
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
