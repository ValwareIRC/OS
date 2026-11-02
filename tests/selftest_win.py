"""The OS's own self-tests (init's test mode: kernel, file systems, permissions, namespaces, sockets, disk...) run
on Windows QEMU with hardware acceleration, as tests/boot_test.sh runs them in WSL (where the 6 GB built-in model
makes an emulated boot impractically slow). Silent sound, a fresh scratch disk.
  python tests\\selftest_win.py [ISO]"""
import os, re, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
iso = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build", "os.iso")
out = os.path.join(ROOT, "build", "selftest")
os.makedirs(out, exist_ok=True)
log = os.path.join(out, "serial.log")
disk = os.path.join(out, "test-disk.img")
with open(disk, "wb") as f:
    f.truncate(1 << 30)
if os.path.exists(log):
    os.remove(log)
args = [QEMU, "-M", "q35", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg", "-cpu", "Skylake-Client-noTSX-IBRS",
        "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04", "-m", "12G", "-smp", "4", "-no-reboot", "-display", "none",
        "-fw_cfg", "name=opt/os/mode,string=test", "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-drive", "id=d0,file=" + disk.replace(os.sep, "/") + ",format=raw,if=none", "-device", "ide-hd,drive=d0,bus=ide.0",
        "-audiodev", "none,id=snd0", "-device", "intel-hda", "-device", "hda-duplex,audiodev=snd0",
        "-serial", "file:" + log.replace(os.sep, "/"), "-cdrom", iso.replace(os.sep, "/")]
if os.path.basename(iso) == "dev.iso":  # the development discs: the big files on a second one
    args += ["-drive", "id=big,file=" + os.path.join(os.path.dirname(iso), "big.iso").replace(os.sep, "/") +
             ",media=cdrom,if=none,readonly=on", "-device", "ide-cd,drive=big,bus=ide.3"]
t0 = time.time()
p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=open(os.path.join(out, "qemu.err"), "w"))
try:
    code = p.wait(timeout=1500)
except subprocess.TimeoutExpired:
    p.kill()
    code = -1
text = open(log, encoding="utf-8", errors="replace").read() if os.path.exists(log) else ""
ok = [l for l in text.splitlines() if l.startswith("utest:") and l.endswith(" ok")]
bad = [l for l in text.splitlines() if "FAILED" in l]
print(f"{len(ok)} checks ok, {len(bad)} failed, exit {code} (1 = success), {time.time() - t0:.0f}s")
for l in bad:
    print("  " + l[:200])
for l in text.splitlines():
    if "permissions" in l or "USERLAND TESTS" in l or "sandbox root" in l or "ordinary user" in l:
        print("  " + l[:220])
