"""The Settings pages every system has (docs/toy-audit.md 17): Mouse, Power, Storage, Apps, Notifications, each
opened and photographed, and used: left-handed buttons switched on and off, the screen off after five minutes away
(and back with a key), the disk measured, Do not disturb by the hours. Keys and clicks through QMP. Windows QEMU,
silent sound.

  python tests\\settings_pages_vm_win.py [OUTDIR]     (QUICK=1: without the five-minute wait)"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "settings-pages-vm"))
os.makedirs(out, exist_ok=True)
results = {}


def rect(vm, name, since=0):
    m = None
    for m in re.finditer(rf"rect {re.escape(name)} (-?\d+) (-?\d+) (\d+) (\d+)", vm.log_text()[since:]):
        pass
    return tuple(int(v) for v in m.groups()) if m else None


def click_rect(vm, name, since=0, at=None):
    r = rect(vm, name, since)
    if r:
        vm.click(r[0] + (r[2] // 2 if at is None else at), r[1] + r[3] // 2)
    return r


def page(vm, name):
    m = len(vm.log_text())
    click_rect(vm, "settings-page-" + name)
    time.sleep(2.5)
    vm.shot(f"{name.lower()}.png")
    return len(vm.log_text())


vm = vmkit.Vm(out, ["Okay."] * 5, memory=os.environ.get("MEM", "20G"), qport=4478, aiport=8922)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 240)
    time.sleep(4)
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    vm.type("settings")
    time.sleep(0.8)
    vm.key("ret")
    time.sleep(3)
    names = re.findall(r"rect settings-page-(\S+) ", vm.log_text())
    results["the pages are there"] = all(n in names for n in ["Mouse", "Power", "Storage", "Apps", "Notifications"])
    # Mouse: left-handed on (the next click is a right one), then off with the right button... by the switch's log
    m = page(vm, "Mouse")
    time.sleep(1.2)
    click_rect(vm, "mouse-left", m)
    results["left-handed switched on"] = bool(vm.wait_for(r"settings: mouse_left on", 5, m))
    # with the buttons swapped, a left click is a right click: switching it off needs the other button
    r = rect(vm, "mouse-left", m)
    if r:
        vm.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "right"}}])
        time.sleep(0.1)
        vm.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "right"}}])
    results["the right button now clicks (and switched it off)"] = bool(vm.wait_for(r"settings: mouse_left off", 5, m))
    # Storage: measured
    m = page(vm, "Storage")
    results["the disk is measured"] = bool(vm.wait_for(r"settings: storage measured", 60, m))
    time.sleep(1.5)
    vm.shot("storage-measured.png")
    page(vm, "Apps")
    # Notifications: by the hours
    m = page(vm, "Notifications")
    time.sleep(1.2)
    click_rect(vm, "notify-hours", m)
    time.sleep(1.5)
    vm.shot("notifications-hours.png")
    # Power: the screen off after five minutes
    m = page(vm, "Power")
    time.sleep(1.2)
    r = rect(vm, "power-0", m)
    if r:  # the second of six: "5 min"
        vm.click(r[0] + r[2] * 3 // 12, r[1] + r[3] // 2)
    results["screen off after 5 minutes chosen"] = bool(vm.wait_for(r"settings: screen_off 5", 5, m))
    if not os.environ.get("QUICK"):
        m = len(vm.log_text())
        results["the screen goes off after five minutes away"] = bool(vm.wait_for(r"power: the screen is off", 330, m))
        vm.shot("screen-off.png")
        vm.key("shift")
        results["a key brings it back"] = bool(vm.wait_for(r"power: the screen is back", 5, m))
        time.sleep(1.5)
        vm.shot("screen-back.png")
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
