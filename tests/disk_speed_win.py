"""How fast the apps' area on the disk takes writes (init's OS_MODE=disk-speed): 2000 small files each made sure of,
2000 without, one 200 MB file. A fresh sparse disk of its own.  python tests\\disk_speed_win.py [ISO]"""
import os, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = vmkit.ROOT
iso = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "dev.iso")
out = os.path.join(ROOT, "build", "disk-speed")
os.makedirs(out, exist_ok=True)
log = os.path.join(out, "serial.log")
if os.path.exists(log):
    os.remove(log)
disk = vmkit.Vm.new_disk(os.path.join(out, "disk.img"), 4 << 30)
args = [vmkit.QEMU, "-M", "q35", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg", "-cpu", "Skylake-Client-noTSX-IBRS",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-m", "8G", "-smp", "4", "-no-reboot", "-display", "none",
        "-fw_cfg", "name=opt/os/mode,string=disk-speed", "-audiodev", "none,id=snd0",
        "-drive", "id=d0,file=" + disk.replace(os.sep, "/") + ",format=raw,if=none", "-device", "ide-hd,drive=d0,bus=ide.0",
        "-serial", "file:" + log.replace(os.sep, "/"), "-cdrom", iso.replace(os.sep, "/")]
if os.path.basename(iso) == "dev.iso":
    args += ["-drive", "id=big,file=" + os.path.join(os.path.dirname(iso), "big.iso").replace(os.sep, "/") +
             ",media=cdrom,if=none,readonly=on", "-device", "ide-cd,drive=big,bus=ide.3"]
p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w"))
try:
    p.wait(timeout=900)
except subprocess.TimeoutExpired:
    p.kill()
import re
text = open(log, encoding="utf-8", errors="replace").read()
for l in text.splitlines():
    if "disk-speed:" in l or "os-data" in l or "ahci" in l.lower():
        print(l[:200])
for name, pat in (("disk_small_synced_per_s", r"each with fsync: [\d.]+ s, (\d+) files/s"),
                  ("disk_big_write_mbs", r"one 200 MB file: [\d.]+ s, ([\d.]+) MB/s")):
    m = re.search(pat, text)
    if m:
        print(f"PERF {name}={m.group(1)}")  # (for the Performance fixes panel)
