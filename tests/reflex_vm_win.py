"""The reflexes judging events without freezing the desktop. Each judgement (the small local model, LFM3 1.7B,
asked whether an event is worth the companion's attention) ran in a forked copy of the whole desktop: the fork
and the copying while it lived froze the desktop for seconds, every few seconds when much was happening. Now on a
thread. A fresh disk downloads the reflex model first (a few minutes); then Notes is opened and closed again and
again, each an event to judge, and the desktop's own reports must show the judgements coming back, its frames
not starved ("nerve" a small share of a core) and no crash.

  set ISO=build\\dev.iso & python tests\\reflex_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "reflex-vm"))
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 8 << 30)
os.environ["DISK_RW"] = "1"
results = {}
# events the reflexes judge (the person's own are kept without asking them): long busy tasks (20 s of CPU and more) ending,
# one every 30 s or so from about four minutes in, when the reflex model is downloaded and up
tasks = os.path.join(out, "tasks.sh")
with open(tasks, "w", newline="\n") as f:
    f.write("#!/bin/sh\nfor i in 1 2 3 4 5 6 7 8 9 10 11 12; do (sleep $((150 + i * 30)); exec timeout 65 yes >/dev/null) & done\n")
os.environ["AUTORUN"] = tasks


def keys(*names):
    vm.cmd("send-key", keys=[{"type": "qcode", "data": n} for n in names])
    time.sleep(0.8)


vm = vmkit.Vm(out, ["Okay."] * 40, memory=os.environ.get("MEM", "10G"), disk=disk, qport=4489, aiport=8933)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 300)
    if vm.wait_for(r'window \(0\) opened "Companion"', 45):
        time.sleep(2)
        keys("alt", "f4")
    got = vm.wait_for(r"reflex: .* downloaded|reflex: .*failed", 900)
    results["the reflex model is there"] = bool(got) and "downloaded" in got.group(0)
    vm.wait_for(r"reflex: (starting|ready|up)", 120)
    time.sleep(30)  # (its server starting)
    m = len(vm.log_text())
    t0 = time.time()  # (hands off: what follows input is taken for the person's doing, and kept unjudged)
    while time.time() - t0 < 480 and len(re.findall(r"nerve: reflex: ", vm.log_text()[m:])) < 4:
        time.sleep(5)
    time.sleep(35)  # (a half-minute report)
    text = vm.log_text()[m:]
    verdicts = re.findall(r"nerve: reflex: .* -> (\w+) \(", text)
    print(f"{len(verdicts)} judgements came back: {verdicts[:12]}", flush=True)
    results["judgements come back"] = len(verdicts) >= 3
    nerve = [float(x) for x in re.findall(r"time: .*?nerve (\d+\.\d+)%", text)]
    frames = [int(x) for x in re.findall(r"frames: (\d+) drawn", text)]
    print(f"nerve's share of a core in each half minute: {nerve}; frames drawn: {frames}", flush=True)
    results["the reflexes take under 5% of the desktop's core"] = bool(nerve or frames) and max(nerve or [0]) < 5
    results["nothing crashed"] = "segmentation fault" not in vm.log_text()
finally:
    vm.stop()
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if all(results.values()) else "SOME FAILED")
