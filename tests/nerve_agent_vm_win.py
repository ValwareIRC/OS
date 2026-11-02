"""Its nervous system and its body, in the OS, with Windows QEMU (Hyper-V) and silent sound; tests/fake_ai.py
plays its mind, a small web server here gives it something to download:
  its body: it makes a folder, downloads a picture into it and sets it as the wallpaper, step after step;
  what the person does is theirs: they close a window (Alt+F4); it is felt, never news, nobody woken;
  its reflexes (LFM3 1.7B on the VM itself, decision-model style) judge what the system did by itself: a
  cancelled flight wakes it; the internet coming back doesn't; a full disk is urgent and wakes it at once.
  python tests\\nerve_agent_vm_win.py [OUTDIR]"""
import json, os, re, sys, threading, time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = vmkit.ROOT
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "nerve-vm"))
os.makedirs(out, exist_ok=True)

# something to download: a picture
www = os.path.join(out, "www")
os.makedirs(www, exist_ok=True)
import struct, zlib
w, h = 160, 90
rows = b"".join(b"\x00" + bytes(v for x in range(w) for v in (x * 255 // w, y * 255 // h, 140)) for y in range(h))
chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
open(os.path.join(www, "wall.png"), "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                                                + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b""))


class Quiet(SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=www, **k)

    def log_message(self, *a):
        pass


web = ThreadingHTTPServer(("0.0.0.0", 8931), Quiet)
threading.Thread(target=web.serve_forever, daemon=True).start()

replies = [
    {"when": r"set up a wallpaper", "reply": "On it. [[mkdir: ~/Pictures/walls]] "
                                             "[[fetch: http://10.0.2.2:8931/wall.png -> ~/Pictures/walls/wall.png]]"},
    {"when": r"fetch: http://10\.0\.2\.2:8931/wall\.png[\s\S]*saved", "reply": "[[wallpaper: ~/Pictures/walls/wall.png]] [[ls: ~/Pictures/walls]]"},
    {"when": r"the wallpaper is now", "reply": "Done: it's behind everything now."},
    {"when": r"open files", "reply": "[[open:files]]"},
    {"when": r"test the alarms", "reply": "[[sh]]printf 'system|mail|1|a notification from Mail: \"Your flight tomorrow at 7:10 is cancelled\"\\n"
                                          "system|network|1|the internet is back (it was down for 1 minute)\\n' > /tmp/nerve-inject[[/sh]]"},
    {"when": r"printf 'system\|mail", "reply": "Set."},
    {"when": r"flight tomorrow", "reply": "Your 7:10 flight tomorrow was just cancelled."},
    {"when": r"one more", "reply": "[[sh]]printf 'system|disk|3|the disk is 99%% full: 300 MB left\\n' > /tmp/nerve-inject[[/sh]]"},
    {"when": r"printf 'system\|disk", "reply": "Set."},
    {"when": r"99% full", "reply": "Your disk is nearly full: 300 MB left."},
]
for _ in range(10):
    replies.append("Mm.")

vm = vmkit.Vm(out, replies, alive="proactive=1\n")
results = {}


def log():
    return vm.log_text()


try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    time.sleep(6)
    vm.focus_companion()
    # its body
    vm.say("set up a wallpaper", r"agent: step 1")
    results["step 1 (folder, download)"] = bool(vm.wait_for(r"agent: step 1", 30))
    results["step 2 (wallpaper)"] = bool(vm.wait_for(r"agent: step 2", 90))
    results["step 3 (done, words)"] = bool(vm.wait_for(r"ai Done: it's behind everything now", 90))
    time.sleep(4)
    vm.shot("1-wallpaper.png")
    # the person closes a window: theirs, never news
    vm.say("open files", r"window [^\n]*opened|opened \"")
    time.sleep(3)
    m = len(log())
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "f4"}])
    hit = vm.wait_for(r"nerve: p window \(0\) [^\n]*closed", 15, m)
    results["their closing is theirs"] = bool(hit)
    time.sleep(8)
    results["nobody woken by it"] = "waking it" not in log()[m:]
    # the reflex model on the VM
    ready = vm.wait_for(r"reflex: [^\n]* ready", 900)
    results["reflex model ready on the VM"] = bool(ready)
    if ready:
        print("   ", ready.group(0))
    vm.focus_companion()
    vm.say("test the alarms", r"nerve: injected")
    flight = vm.wait_for(r"nerve: reflex: [^\n]*flight[^\n]*", 120)
    net = vm.wait_for(r"nerve: reflex: [^\n]*internet is back[^\n]*", 120)
    if flight:
        print("   ", flight.group(0))
    if net:
        print("   ", net.group(0))
    results["flight judged wake"] = bool(flight and "-> wake" in flight.group(0) or flight and "-> urgent" in flight.group(0))
    results["internet back not woken"] = bool(net and ("-> remember" in net.group(0) or "-> ignore" in net.group(0)))
    # woken now, or (if the last interruption was too recent) held and told with the next one
    vm.focus_companion()
    vm.say("one more", r"nerve: injected")
    results["disk urgent"] = bool(vm.wait_for(r"nerve: s?o? ?disk[^\n]*-> urgent|disk \(3\)[^\n]*urgent", 30))
    time.sleep(20)
    told = [l for l in open(vm.ailog, encoding="utf-8", errors="replace") if "Your reflexes flagged this" in l]
    results["told of the flight"] = any("flight tomorrow" in l for l in told)
    results["told of the disk at once"] = any("99% full" in l for l in told)
    vm.shot("2-after.png")
    for line in re.findall(r"shell: (?:nerve|agent|reflex): [^\n]*", log())[-40:]:
        print("   ", line[:200])
finally:
    vm.stop()
    web.shutdown()
    for k, v in results.items():
        print(f"{k}: {v}")
