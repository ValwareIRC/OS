"""Prism opened at full size, as Valerie opens it: how long until it is ready (its own log: Electron ready, its
window shown, its first screen up), twice (the first time, then again), and what the desktop spends meanwhile
(its "frames:" and "time:" lines: frames drawn, how many were only Prism's picture, the share of a core).

  set ISO=build\\dev.iso & python tests\\fullsize_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "fullsize-vm"))
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 8 << 30)
os.environ["DISK_RW"] = "1"
results, perf = {}, {}


def keys(*names):
    vm.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names])
    time.sleep(0.8)


def open_prism(name):
    m = len(vm.log_text())
    keys("alt", "spc")
    vm.type("prism")
    time.sleep(3)
    t0 = time.time()
    vm.key("ret")
    asked = False
    got = {}
    marks = {"electron": r"\] Electron is ready", "window": r"\] window 1: shown", "screen": r"first screen put up"}
    while time.time() - t0 < 180 and len(got) < len(marks):
        text = vm.log_text()[m:]
        for k, pat in marks.items():
            if k not in got and re.search(pat, text):
                got[k] = round(time.time() - t0, 1)
        if not asked and "appapi: asking whether com.valware.prism" in text:  # (its first start: may it use the AI)
            asked = True
            time.sleep(0.5)
            vm.key("ret")
        time.sleep(0.25)
    if not asked and vm.wait_for(r"appapi: asking whether com\.valware\.prism", 15, m):
        time.sleep(0.5)
        vm.key("ret")
    sizes = re.findall(r"app window \d+: .*?Prism.*? (\d+x\d+)", vm.log_text()[m:])
    print(f"PERF {name}: " + ", ".join(f"{k} {got.get(k, '-')} s" for k in marks) + f"; size {sizes[-1] if sizes else '?'}",
          flush=True)
    perf[name] = got
    vm.shot(name + ".png")
    return "screen" in got


def frames_since(m, name):
    text = vm.log_text()[m:]
    for f, t in re.findall(r"shell: (frames: .*)\n(?:.*\n)*?shell: (time: whole frame [^\n]*)", text)[:3]:
        print(f"  {name}: {f[:150]}\n      {t[:200]}", flush=True)


vm = vmkit.Vm(out, ["Okay."] * 10, memory=os.environ.get("MEM", "12G"), disk=disk, qport=4487, aiport=8931)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 300)
    # the companion opens itself a little after the desktop arrives: closed first, so Prism has the front
    if vm.wait_for(r'window \(0\) opened "Companion"', 45):
        time.sleep(2)
        keys("alt", "f4")
    vm.wait_for(r"base: read ahead in", 120)
    time.sleep(5)
    results["Prism opens"] = open_prism("prism-first")
    m = len(vm.log_text())
    time.sleep(65)  # (two of the desktop's half-minute reports with Prism open, doing nothing)
    frames_since(m, "prism open")
    keys("alt", "f4")
    vm.wait_for(r'window \(0\) (?:"[^"]*Prism[^"]*" closed|closed "[^"]*Prism)', 30)
    time.sleep(10)
    results["Prism opens again"] = open_prism("prism-again")
finally:
    vm.stop()
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if all(results.values()) else "SOME FAILED")
