"""What glass window frames cost with app windows open: Windows QEMU, silent sound, a design whose frames are
glass (frost). Two Wayland apps (NetSurf, big; weston-smoke, always moving, so frames keep coming) are opened
by an autorun script; the desktop's own accounting (its "time:" lines, every 30 s) says what share of a core
the glass took and the whole frame.

  set ISO=build\\dev.iso & python tests\\glass_vm_win.py"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(ROOT, "build", "glass")
os.makedirs(out, exist_ok=True)
script = os.path.join(out, "autorun.sh")
open(script, "w", newline="\n").write("netsurf-gtk about:blank &\nsleep 4\nweston-smoke &\n")
os.environ["AUTORUN"] = script
results = {}
vm = vmkit.Vm(out, ["Okay."] * 5, design=os.environ.get("DESIGN", "frost"), memory=os.environ.get("MEM", "8G"),
              qport=4489, aiport=8933)
try:
    vm.start()
    results["the desktop starts"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    m = len(vm.log_text())
    two = None
    t0 = time.time()
    while time.time() - t0 < 120:
        if len(re.findall(r"shell: app window \d+: ", vm.log_text()[m:])) >= 2:
            two = True
            break
        time.sleep(1)
    results["two app windows open"] = bool(two)
    vm.shot("1-windows.png")
    m = len(vm.log_text())
    time.sleep(75)  # (two whole 30 s accountings with both open)
    text = vm.log_text()[m:]
    glass = [float(x) for x in re.findall(r"painting boxes: .*?glass ([0-9.]+)%", text)]
    whole = [float(x) for x in re.findall(r"time: whole frame ([0-9.]+)%", text)]
    frames = [int(x) for x in re.findall(r"frames: (\d+) drawn", text)]
    print(f"  glass {glass}, whole frame {whole}, frames drawn per 30 s {frames}", flush=True)
    if glass:
        print(f"PERF glass_pct={glass[-1]}", flush=True)
    if whole:
        print(f"PERF whole_frame_pct={whole[-1]}", flush=True)
    results["measured"] = bool(glass and whole)
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
