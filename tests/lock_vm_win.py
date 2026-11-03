"""The lock screen and the optional password, used as a person would (keys and clicks through QMP, never the
real mouse): Super+L locks and, with no password, Enter opens it; a password set in Settings > Users; locked
again, a wrong one is refused and the right one opens it; after a restart (a second boot of the same disk) it
asks for the password before the desktop. Windows QEMU, silent sound; a fresh disk of its own.

  python tests\\lock_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "lock-vm"))
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 1 << 30)  # new, empty, sparse
os.environ["DISK_RW"] = "1"
PW = "Correct horse 7"
results = {}


def super_l(vm):
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "meta_l"}, {"type": "qcode", "data": "l"}])


def rect(vm, name, since=0):
    m = None
    for m in re.finditer(rf"rect {name} (-?\d+) (-?\d+) (\d+) (\d+)", vm.log_text()[since:]):
        pass
    return tuple(int(v) for v in m.groups()) if m else None


def click_rect(vm, name, since=0):
    r = rect(vm, name, since)
    if r:
        vm.click(r[0] + r[2] // 2, r[1] + r[3] // 2)
    return bool(r)


vm = vmkit.Vm(out, ["Okay."] * 10, memory=os.environ.get("MEM", "20G"), disk=disk)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 240)
    time.sleep(4)
    m = len(vm.log_text())
    super_l(vm)
    results["Super+L locks"] = bool(vm.wait_for(r"lock: locked", 10, m))
    time.sleep(1)
    vm.shot("1-locked.png")
    vm.key("ret")
    results["with no password, Enter opens it"] = bool(vm.wait_for(r"lock: opened", 10, m))
    # Settings > Users: a password
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    vm.type("settings")
    time.sleep(0.8)
    vm.key("ret")
    time.sleep(3)
    m = len(vm.log_text())
    click_rect(vm, "settings-page-Users")
    time.sleep(2.5)
    vm.shot("2-users.png")
    click_rect(vm, "users-new", m)
    time.sleep(0.4)
    vm.type(PW)
    click_rect(vm, "users-again", m)
    time.sleep(0.4)
    vm.type(PW)
    time.sleep(0.6)
    click_rect(vm, "users-set", m)
    results["a password is set"] = bool(vm.wait_for(r"init: their password was set", 15, m))
    time.sleep(2)
    vm.shot("3-set.png")
    # locked again: a wrong one, then the right one
    m = len(vm.log_text())
    super_l(vm)
    vm.wait_for(r"lock: locked", 10, m)
    time.sleep(1)
    vm.type("wrong guess\n")
    results["a wrong password is refused"] = bool(vm.wait_for(r"lock: a wrong password", 15, m))
    time.sleep(1)
    vm.shot("4-wrong.png")
    vm.type(PW + "\n")
    results["the right one opens it"] = bool(vm.wait_for(r"lock: opened", 15, m))
    vm.wait_for(r"init: the desktop asks: save", 30, m)
    time.sleep(4)
    vm.stop()
    time.sleep(4)  # (QEMU lets go of its sockets)
    # a restart: it asks first (its own folder for the second boot's log; the same disk)
    out2 = out + "-2"
    os.makedirs(out2, exist_ok=True)
    vm = vmkit.Vm(out2, ["Okay."] * 10, memory=os.environ.get("MEM", "20G"), disk=disk, qport=4475, aiport=8919)
    vm.start()
    results["after a restart, it asks for the password first"] = bool(vm.wait_for(r"lock: signing in", 240))
    vm.wait_for(r"shell: screen desktop", 60)
    time.sleep(3)
    vm.shot("../lock-vm/5-sign-in.png")
    m = len(vm.log_text())
    vm.type(PW + "\n")
    results["signed in with it"] = bool(vm.wait_for(r"lock: opened", 15, m))
    time.sleep(2)
    vm.shot("../lock-vm/6-in.png")
    results["the password never appears in the log"] = PW not in vm.log_text() and "orrect horse" not in vm.log_text()
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
