"""How fast the VM downloads from this PC, nothing else in the way (init's OS_MODE=net-speed): a plain HTTP server
here streams 200 MB, the VM times it. Firefox from Flathub took 22 minutes (about 0.5 MB/s), so this splits our
network card driver and TCP from Flatpak and the disk.  python tests\\net_speed_win.py [ISO]"""
import os, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
iso = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "dev.iso")
out = os.path.join(ROOT, "build", "net-speed")
os.makedirs(out, exist_ok=True)
log = os.path.join(out, "serial.log")
if os.path.exists(log):
    os.remove(log)
PORT, SIZE = 8931, 200 << 20
block = os.urandom(1 << 20)
served = {}


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        self.send_response(200)
        self.send_header("Content-Length", str(SIZE))
        self.end_headers()
        t = time.time()
        for _ in range(SIZE // len(block)):
            self.wfile.write(block)
        served["secs"] = time.time() - t


if os.environ.get("TARGET"):  # the same file, fetched here first: what the internet gives this PC
    import urllib.request
    host, path = os.environ["TARGET"].split(":", 1)
    t = time.time()
    n = 0  # (measured once already: 6.2 MB/s)
    if n:
        print(f"this PC: {n / 1e6:.1f} MB in {time.time() - t:.1f} s = {n / 1e6 / (time.time() - t):.1f} MB/s")
srv = ThreadingHTTPServer(("0.0.0.0", PORT), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
args = [QEMU, "-M", "q35", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg", "-cpu", "Skylake-Client-noTSX-IBRS",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-m", "8G", "-smp", "4", "-no-reboot", "-display", "none",
        "-fw_cfg", "name=opt/os/mode,string=net-speed:" + (f"80:{os.environ['TARGET']}" if os.environ.get("TARGET") else str(PORT)) +
        (f"*{os.environ['MANY']}" if os.environ.get("MANY") else ""), "-netdev", "user,id=n0", "-device",
        os.environ.get("NIC", "e1000") + ",netdev=n0", "-audiodev", "none,id=snd0",
        "-serial", "file:" + log.replace(os.sep, "/"), "-cdrom", iso.replace(os.sep, "/")]
if os.path.basename(iso) == "dev.iso":
    args += ["-drive", "id=big,file=" + os.path.join(os.path.dirname(iso), "big.iso").replace(os.sep, "/") +
             ",media=cdrom,if=none,readonly=on", "-device", "ide-cd,drive=big,bus=ide.3"]
p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w"))
try:
    p.wait(timeout=600)
except subprocess.TimeoutExpired:
    p.kill()
text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
for l in text.splitlines():
    if "net-speed:" in l or "e1000" in l:
        print(l)
import re
speeds = [float(x) for x in re.findall(r"net-speed: [\d.]+ MB in [\d.]+ s = ([\d.]+) MB/s", text)]
if speeds:  # (for the Performance fixes panel)
    print(f"PERF net_{'together' if len(speeds) > 1 else 'single'}_mbs={sum(speeds):.1f}")
if "secs" in served:
    print(f"(this PC sent it in {served['secs']:.1f} s)")
