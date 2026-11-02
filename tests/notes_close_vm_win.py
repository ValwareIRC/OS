"""Closing Notes with Alt+F4 must leave the desktop running: it froze there (the log stopped, no key did anything
after). Opens Notes from the launcher, closes it, then opens the launcher again and checks the desktop answers.
Windows QEMU, silent sound; HANGDUMP=1 for the kernel's account of each thread if it hangs.

  python tests\notes_close_vm_win.py [OUTDIR]     (DISK= a disk to boot from, in snapshot mode)"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "notes-close-vm"))
os.makedirs(out, exist_ok=True)
disk = os.environ.get("DISK") or None
vm = vmkit.Vm(out, ["Okay."] * 5, memory=os.environ.get("MEM", "20G"), disk=disk, facet=not disk)
ok = True
try:
    vm.start()
    vm.wait_for(r"shell: screen (desktop|choose|onboard)", 240)
    time.sleep(4)
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.2)
    vm.type("notes")
    time.sleep(0.8)
    vm.key("ret")
    vm.wait_for(r"opened \"Notes\"", 20)
    time.sleep(2)
    m = len(vm.log_text())
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "f4"}])
    time.sleep(3)
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    hit = vm.wait_for(r"launcher: open", 15, m)
    print("PASS the desktop answers after Notes closes" if hit else "FAIL the desktop stopped after Notes closed")
    ok = bool(hit)
    if not hit and os.environ.get("HANGDUMP"):
        time.sleep(70)
    vm.shot("after.png")
finally:
    vm.stop()
sys.exit(0 if ok else 1)
