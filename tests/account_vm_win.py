"""Their account is named after them and their files live in /home/<name>: a disk from before (its home was
/root) boots, its files come back in the new home, the desktop keeps the account's name from the name they gave;
after a restart (same VM, the disk in snapshot mode keeps the session's writes) the account is theirs, its home
/home/<name>, in /etc/passwd, and their files are there. Windows QEMU, silent sound.

  python tests\\account_vm_win.py [OUTDIR]     (DISK= a disk from before; it is never written)"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "account-vm"))
os.makedirs(out, exist_ok=True)
src = os.environ.get("DISK")
disk = os.path.join(out, "disk.img")  # a writable copy: the restart is a second boot of it
os.environ["DISK_RW"] = "1"
replies = [{"when": r"check my files", "reply": "[[sh: id -un; echo HOME=$HOME; ls ~ | tr '\\n' ' '; echo; grep ':1000:' /etc/passwd]]"},
           {"when": r"HOME=", "reply": "Checked."}] + ["Okay."] * 6
vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "20G"), disk=disk, facet=False)
results = {}
try:
    vm.start()
    results["first start: desktop"] = bool(vm.wait_for(r"shell: screen (desktop|choose|onboard)", 240))
    m = vm.wait_for(r"init: the account is (\S+) \(([^)]*)\), at (\S+)", 5)
    print("   first:", m.group(0) if m else "(no account line)")
    kept = vm.wait_for(r"shell: account: (\S+) \(([^)]*)\), from the next start", 30)
    print("   kept:", kept.group(0) if kept else "(the desktop kept no account name)")
    results["the desktop keeps their account name"] = bool(kept)
    vm.wait_for(r"init: the desktop asks: save", 40)
    time.sleep(3)
    vm.stop()  # a restart: a second boot of the same (writable, copied) disk
    vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "20G"), disk=disk, facet=False)
    vm.start()
    since = 0
    m2 = vm.wait_for(r"init: the account is (\S+) \(([^)]*)\), at (\S+)", 240, since)
    print("   after the restart:", m2.group(0) if m2 else "(no account line)")
    name = kept.group(1) if kept else "?"
    results["the account is theirs after the restart"] = bool(m2) and m2.group(1) == name and m2.group(3) == f"/home/{name}"
    vm.wait_for(r"shell: screen (desktop|choose|onboard)", 240, since)
    time.sleep(6)
    vm.focus_companion()
    vm.say("check my files", r"ai Checked", secs=60)
    sent = open(vm.ailog, encoding="utf-8", errors="replace").read()
    sh = re.search(r"exit 0 \| ([^|]*) \| HOME=([^ |]*) \| ([^|]*) \| ([^|]*)", sent.replace("\\n", " | "))
    print("   the shell says:", sh.groups() if sh else "(nothing)")
    results["the shell runs as them, in their home"] = bool(sh) and sh.group(1).strip() == name and sh.group(2) == f"/home/{name}"
    results["their files are in it"] = bool(sh) and "Documents" in sh.group(3)
    results["/etc/passwd has them"] = bool(sh) and sh.group(4).startswith(f"{name}:x:1000:1000:")
    vm.shot("after.png")
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
