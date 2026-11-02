"""Every surface, opened and photographed, for an audit by eye: the launcher, each app (Files, Notes, Terminal,
Settings and its pages, About), the companion's window, the tray's menus. tests/fake_ai.py opens them through
the companion ([[open:...]]); keys through QMP (never the real mouse). Windows QEMU, silent sound.

  python tests\\surfaces_vm_win.py [OUTDIR]     (DISK= a disk to boot from, in snapshot mode)"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "surfaces-vm"))
os.makedirs(out, exist_ok=True)
APPS = ["files", "notes", "terminal", "settings", "about", "companion"]
replies = [{"when": rf"open {a}\b", "reply": f"[[open:{a}]]"} for a in APPS] + ["Okay."] * 20
disk = os.environ.get("DISK") or None
vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "20G"), disk=disk, facet=not disk)
try:
    vm.start()
    vm.wait_for(r"shell: screen (desktop|choose|onboard)", 240)
    time.sleep(5)
    vm.shot("00-desktop.png")
    # the launcher (Alt+Space), its empty state and a search
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    vm.shot("01-launcher.png")
    vm.type("set")
    time.sleep(1.2)
    vm.shot("02-launcher-search.png")
    vm.key("esc")
    time.sleep(0.8)
    for i, a in enumerate(APPS):  # as a person opens them: the launcher, the name, Enter
        vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
        time.sleep(1.2)
        vm.type({"companion": "luna"}.get(a, a))
        time.sleep(0.8)
        vm.key("ret")
        time.sleep(3)
        vm.shot(f"{10 + i:02d}-{a}.png")
        vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "f4"}])
        time.sleep(1)
    vm.shot("20-after.png")
    if os.environ.get("HANGDUMP"):  # the kernel's account of each thread comes every minute
        time.sleep(75)
finally:
    vm.stop()
