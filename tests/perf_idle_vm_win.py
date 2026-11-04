"""What the desktop costs while nothing happens: the desktop up on a fresh disk, a minute to settle, then twenty
quiet seconds measured. From Windows: the QEMU process's processor time (the whole VM). From inside: the kernel's
perfwatch (fw_cfg opt/os/perfwatch) logs every 10 s which processes used the processors and how busy the machine
was, read from the serial log; nothing is asked of the companion, so nothing wakes it.
Prints PERF lines (the Performance fixes panel reads them).
  python tests\\perf_idle_vm_win.py   (ISO= the image, dev.iso by default; ALWAYS_DRAW=1: every frame drawn, as before)"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.join(vmkit.ROOT, "build", "perf-idle")
os.makedirs(out, exist_ok=True)
os.environ.setdefault("ISO", os.path.join(vmkit.ROOT, "build", "dev.iso"))
os.environ["PERFWATCH"] = "1"
disk = vmkit.Vm.new_disk(os.path.join(out, "disk.img"), 8 << 30)
# (the reflex model's first-boot download held back: a settled desktop is what is measured)
alive = "proactive=0\nbackground_downloads=0\n" + ("always_draw=1\n" if os.environ.get("ALWAYS_DRAW") else "")
vm = vmkit.Vm(out, ["Okay."] * 8, alive=alive, memory=os.environ.get("MEM", "8G"), disk=disk, qport=4504, aiport=8948)


def qemu_cpu_s():
    """The QEMU process's processor time so far (all its threads: every virtual CPU), from Windows itself."""
    import ctypes
    from ctypes import wintypes
    c, e, k, u = (wintypes.FILETIME() for _ in range(4))
    ctypes.windll.kernel32.GetProcessTimes(wintypes.HANDLE(vm.qemu._handle), ctypes.byref(c), ctypes.byref(e),
                                           ctypes.byref(k), ctypes.byref(u))
    return sum((t.dwHighDateTime << 32 | t.dwLowDateTime) for t in (k, u)) / 1e7


host, lines = None, []
try:
    vm.start()
    if not vm.wait_for(r"shell: screen desktop", 300):
        print("the desktop didn't come up")
    time.sleep(60)  # (past the start: models settled, the first greeting said)
    m = len(vm.log_text())
    c0, t0 = qemu_cpu_s(), time.time()
    time.sleep(21)
    host = (qemu_cpu_s() - c0) / (time.time() - t0) * 100
    lines = re.findall(r"perfwatch: .*", vm.log_text()[m:])[:2]
finally:
    vm.stop()
for l in lines:
    print(l)
for l in re.findall(r"shell: (?:frames|time): .*", vm.log_text())[-2:]:
    print(l)


def avg(pat):
    v = [float(x) for l in lines for x in re.findall(pat, l)]
    return sum(v) / len(v) if v else None


busy, kern = avg(r"busy ([\d.]+)%"), avg(r"kernel ([\d.]+)%\)")
desk = [sum(float(x) for x in re.findall(r"os-shell\(\d+\) (\d+)%", l)) for l in lines]
if busy is not None:
    print(f"PERF idle_machine_busy_pct={busy:.1f}")
    print(f"PERF idle_kernel_pct={kern:.1f}")
if desk:
    print(f"PERF idle_desktop_cpu_pct={sum(desk) / len(desk):.1f}")
if host is not None:  # (of one of this PC's cores: the whole VM, every virtual CPU and QEMU itself)
    print(f"PERF idle_vm_host_cpu_pct={host:.1f}")
