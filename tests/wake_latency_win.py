"""How long a waiting program takes to notice what it waits for (init's OS_MODE=wake-latency): a byte passed back
and forth between two processes through poll, then epoll, and a 1 ms sleep, timed; and whether getrusage tells a
process its own time. Prints PERF lines (the Performance fixes panel reads them).  python tests\\wake_latency_win.py [ISO]"""
import os, re, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = vmkit.ROOT
iso = sys.argv[1] if len(sys.argv) > 1 else os.environ.get("ISO", os.path.join(ROOT, "build", "dev.iso"))
out = os.path.join(ROOT, "build", "wake-latency")
os.makedirs(out, exist_ok=True)
log = os.path.join(out, "serial.log")
if os.path.exists(log):
    os.remove(log)
args = [vmkit.QEMU, "-M", "q35", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg", "-cpu", "Skylake-Client-noTSX-IBRS",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-m", "4G", "-smp", "4", "-no-reboot", "-display", "none",
        "-fw_cfg", "name=opt/os/mode,string=wake-latency", "-audiodev", "none,id=snd0",
        "-serial", "file:" + log.replace(os.sep, "/"), "-cdrom", iso.replace(os.sep, "/")]
if os.path.basename(iso) == "dev.iso":
    args += ["-drive", "id=big,file=" + os.path.join(os.path.dirname(iso), "big.iso").replace(os.sep, "/") +
             ",media=cdrom,if=none,readonly=on", "-device", "ide-cd,drive=big,bus=ide.3"]
p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w"))
try:
    p.wait(timeout=300)
except subprocess.TimeoutExpired:
    p.kill()
text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
lines = re.findall(r"^PERF \S+=\S+", text, re.M)
print("\n".join(lines) if lines else "no PERF lines: the mode didn't run (see build/wake-latency/serial.log)")
