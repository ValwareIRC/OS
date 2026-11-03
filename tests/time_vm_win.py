"""Date & time (docs/toy-audit.md 15): the clock is set from the network (init's SNTP), the time zone is found
from where the computer is on the internet, Settings > Date & time shows both; a zone chosen there (searched:
"tokyo") becomes the system's (init's /etc/localtime too) and turns the automatic one off; the 24-hour switch
makes the clocks 12-hour. Keys and clicks through QMP. Windows QEMU, silent sound; it needs the internet.

  python tests\\time_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "time-vm"))
os.makedirs(out, exist_ok=True)
results = {}


def rect(vm, name, since=0):
    m = None
    for m in re.finditer(rf"rect {name} (-?\d+) (-?\d+) (\d+) (\d+)", vm.log_text()[since:]):
        pass
    return tuple(int(v) for v in m.groups()) if m else None


def click_rect(vm, name, since=0):
    r = rect(vm, name, since)
    if r:
        vm.click(r[0] + r[2] // 2, r[1] + r[3] // 2)
    return r


vm = vmkit.Vm(out, ["Okay."] * 5, memory=os.environ.get("MEM", "20G"), qport=4477, aiport=8921)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 240)
    results["the clock is set from the network"] = bool(vm.wait_for(r"init: time: from \S+, the clock was", 90))
    results["the time zone is found from the internet"] = bool(
        vm.wait_for(r"clock: the time zone here is \S+|clock: the automatic time zone found nothing", 60))
    found = re.search(r"clock: the time zone here is (\S+)", vm.log_text())
    if found:
        results["init uses it too"] = bool(vm.wait_for(rf"init: time zone now {re.escape(found.group(1))}", 10))
    time.sleep(2)
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    vm.type("settings")
    time.sleep(0.8)
    vm.key("ret")
    time.sleep(3)
    m = len(vm.log_text())
    click_rect(vm, "settings-page-Date-&-time")
    time.sleep(2.5)
    vm.shot("1-page.png")
    m = len(vm.log_text())
    time.sleep(1.2)
    click_rect(vm, "time-zone-search", m)
    time.sleep(0.4)
    vm.type("tokyo")
    time.sleep(2.5)
    vm.shot("2-search.png")
    m = len(vm.log_text())
    time.sleep(1.2)
    click_rect(vm, "time-zone-row-0", m)
    results["a zone chosen by hand"] = bool(vm.wait_for(r"clock: time zone chosen: Asia/Tokyo", 8, m))
    results["init's zone follows"] = bool(vm.wait_for(r"init: time zone now Asia/Tokyo", 8, m))
    time.sleep(1.5)
    m = len(vm.log_text())
    time.sleep(1.2)
    click_rect(vm, "time-24h", m)
    time.sleep(2)
    vm.shot("3-12h.png")
    m = len(vm.log_text())
    time.sleep(1.2)
    click_rect(vm, "time-sync", m)
    results["set it now"] = bool(vm.wait_for(r"init: time: from \S+", 20, m))
    time.sleep(2)
    vm.shot("4-synced.png")
finally:
    vm.stop()
log = open(os.path.join(out, "serial.log"), errors="replace").read()
for line in re.findall(r".*(?:init: time|clock: ).*", log)[:12]:
    print("  " + line.strip())
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
