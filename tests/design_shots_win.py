"""A screenshot of the desktop in each of a few designs (after the drawing code changes: that nothing looks
different). Windows QEMU, silent sound, the stand-in AI.  python tests\\design_shots_win.py [DESIGN ...]
(default: frost xp luna); pictures in build/design-shots/<design>.png"""
import os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

os.environ.setdefault("ISO", os.path.join(vmkit.ROOT, "build", "dev.iso"))
out = os.path.join(vmkit.ROOT, "build", "design-shots")
os.makedirs(out, exist_ok=True)
for design in sys.argv[1:] or ["frost", "xp", "luna"]:
    vm = vmkit.Vm(out, ["Okay."] * 6, design=design, memory="8G", qport=4524, aiport=8968)
    try:
        vm.start()
        ok = vm.wait_for(r"shell: screen desktop", 300)
        time.sleep(12)
        vm.shot(design + ".png")
        print(design, "desktop" if ok else "no desktop")
    finally:
        vm.stop()
