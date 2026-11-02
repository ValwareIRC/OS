"""The boot screen, seen: screenshots of the kernel's splash as the system starts, the steps behind Esc (pressed
through QMP), os-boot carrying it on once the person's things are back, and the desktop fading in from it.
Windows QEMU, silent sound; tests/fake_ai.py as the mind.

  python tests\\bootscreen_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "bootscreen-vm"))
os.makedirs(out, exist_ok=True)
disk = os.environ.get("DISK") or None
vm = vmkit.Vm(out, ["Welcome back."], memory=os.environ.get("MEM", "20G"), disk=disk, facet=not disk)
try:
    vm.start()
    t0 = time.time()
    vm.wait_for(r"kernel starting", 60)
    time.sleep(1.5)
    vm.shot("1-kernel-splash.png")
    vm.wait_for(r"medium: /boot/UEFI/base.cpio", 60)
    time.sleep(2)
    vm.key("esc")
    time.sleep(0.8)
    vm.shot("2-kernel-steps.png")
    def soon(pat, secs):  # the log, watched closely (vm.wait_for looks every half second)
        end = time.time() + secs
        while time.time() < end:
            if re.search(pat, vm.log_text()):
                return True
            time.sleep(0.03)
        return False
    soon(r"os-boot: on the screen", 90)
    time.sleep(0.5)
    vm.shot("3-os-boot-steps.png")
    m = soon(r"display: taking over", 240)
    print(f"desktop after {time.time() - t0:.1f}s" if m else "no desktop", flush=True)
    for i in range(4):
        vm.shot(f"5-fade-{i}.png")
        time.sleep(0.08)
    time.sleep(3)
    vm.shot("6-desktop.png")
    with open(os.path.join(out, "steps.txt"), "w") as f:
        f.write("\n".join(re.findall(r"init: [^\n]*", vm.log_text())))
finally:
    vm.stop()
