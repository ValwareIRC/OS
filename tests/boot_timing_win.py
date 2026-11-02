"""How long the stages of a boot take with Windows QEMU (Hyper-V, silent sound, no window): QEMU getting ready
(memory set aside), the firmware and bootloader (until the kernel's first words), and the kernel until the
desktop starts. Firmware: bios (SeaBIOS) or uefi (edk2).

  python tests\\boot_timing_win.py bios|uefi MEMORY [ISO]"""
import os, socket, subprocess, sys, tempfile, time, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU = r"C:\Program Files\qemu\qemu-system-x86_64.exe"
SHARE = r"C:\Program Files\qemu\share"
fw, mem = sys.argv[1], sys.argv[2]
iso = sys.argv[3] if len(sys.argv) > 3 else os.path.join(ROOT, "build", "os.iso")
work = tempfile.mkdtemp(prefix="boot-timing-")
log = os.path.join(work, "serial.log")
args = [QEMU, "-M", "q35", "-m", mem, "-smp", "8", "-accel", "whpx,kernel-irqchip=on", "-accel", "tcg",
        "-cpu", "Skylake-Client-noTSX-IBRS", "-vga", "std", "-display", "none", "-audiodev", "none,id=a0",
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-qmp", "tcp:127.0.0.1:4479,server,nowait", "-serial", "file:" + log.replace(os.sep, "/"),
        "-cdrom", iso.replace(os.sep, "/")]
if fw == "uefi":
    vars_ = os.path.join(work, "vars.fd")
    shutil.copy(os.path.join(SHARE, "edk2-i386-vars.fd"), vars_)
    args += ["-drive", "if=pflash,format=raw,readonly=on,file=" + os.path.join(SHARE, "edk2-x86_64-code.fd").replace(os.sep, "/"),
             "-drive", "if=pflash,format=raw,file=" + vars_.replace(os.sep, "/")]


def text():
    try:
        return open(log, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


t0 = time.time()
q = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=open(os.path.join(work, "qemu.err"), "w"))
marks = {}
try:
    while "qemu ready" not in marks and time.time() - t0 < 600:
        try:
            socket.create_connection(("127.0.0.1", 4479), timeout=1).close()
            marks["qemu ready"] = time.time() - t0
        except OSError:
            time.sleep(0.1)
    for name, pat in [("kernel starts", "kernel starting"), ("user space", "user space is alive"),
                      ("desktop", "shell: ")]:
        while time.time() - t0 < 900 and pat not in text():
            if q.poll() is not None or "KERNEL PANIC" in text():
                break
            time.sleep(0.2)
        if pat in text():
            marks[name] = time.time() - t0
finally:
    q.kill()
last = 0
print(f"{fw} {mem}:", flush=True)
for k, v in marks.items():
    print(f"  {k:14s} at {v:6.1f}s  (+{v - last:.1f}s)", flush=True)
    last = v
for line in text().splitlines():
    if any(w in line for w in ("medium:", "initramfs:", "ahci:", "modules:", "mm:", "PANIC")):
        print("   ", line[:160])
if "kernel starts" not in marks:
    print("  the kernel never started; qemu said:", open(os.path.join(work, "qemu.err")).read()[-500:])
