"""An install from Flathub as the system does it (init's OS_MODE=flatpak-install:APP), with Flatpak's and ostree's
own account of it on the console: whether it takes deltas or object after object, and the time each part took.
A fresh sparse disk.  python tests\\flatpak_install_win.py [APP]"""
import os, re, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

APP = sys.argv[1] if len(sys.argv) > 1 else "org.mozilla.firefox"
iso = os.environ.get("ISO", os.path.join(vmkit.ROOT, "build", "dev.iso"))
out = os.path.join(vmkit.ROOT, "build", "flatpak-install")
os.makedirs(out, exist_ok=True)
log = os.path.join(out, "serial.log")
if os.path.exists(log):
    os.remove(log)
disk = vmkit.Vm.new_disk(os.path.join(out, "disk.img"), int(os.environ.get("DISK_GB", "24")) << 30)
args = [vmkit.QEMU, "-M", "q35", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg", "-cpu", "Skylake-Client-noTSX-IBRS",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-m", "8G", "-smp", "8", "-no-reboot", "-display", "none",
        "-fw_cfg", f"name=opt/os/mode,string=flatpak-install:{APP}", "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-audiodev", "none,id=snd0",
        "-drive", "id=d0,file=" + disk.replace(os.sep, "/") + ",format=raw,if=none", "-device", "ide-hd,drive=d0,bus=ide.0",
        "-serial", "file:" + log.replace(os.sep, "/"), "-cdrom", iso.replace(os.sep, "/")]
if os.path.basename(iso) == "dev.iso":
    args += ["-drive", "id=big,file=" + os.path.join(os.path.dirname(iso), "big.iso").replace(os.sep, "/") +
             ",media=cdrom,if=none,readonly=on", "-device", "ide-cd,drive=big,bus=ide.3"]
t0 = time.time()
p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w"))
try:
    p.wait(timeout=int(os.environ.get("LIMIT", "2400")))
except subprocess.TimeoutExpired:
    p.kill()
print(f"{time.time() - t0:.0f} s in all")
text = open(log, encoding="utf-8", errors="replace").read()
for pat in (r"flatpak-install: .*", r".*[Dd]elta.*", r".*Fetching.*", r".*[Ee]rror.*"):
    hits = re.findall(pat, text)
    print(f"{pat}: {len(hits)} lines", *hits[:6], sep="\n  ")
