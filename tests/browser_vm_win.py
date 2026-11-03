"""A browser, as they would get one: Firefox found in the launcher, installed from Flathub, opened; its sandbox
starts (bubblewrap locked it down and was refused once, so no browser ever opened) and it draws a window.
Windows QEMU, silent sound, a fresh writable disk (the apps' area on it), the internet.

  python tests\\browser_vm_win.py [OUTDIR]   (APP=org.chromium.Chromium for another)"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

APP = os.environ.get("APP", "org.mozilla.firefox")
out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "browser-vm"))
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 24 << 30)  # new, empty, sparse
os.environ["DISK_RW"] = "1"
results = {}
vm = vmkit.Vm(out, ["Okay."] * 10, memory=os.environ.get("MEM", "12G"), disk=disk, qport=4484, aiport=8928)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 300)
    results["Flathub's list arrives"] = bool(vm.wait_for(r"launcher: Flathub offers [1-9]\d* apps", 600))
    time.sleep(3)
    m = len(vm.log_text())
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    vm.type("firefox" if "firefox" in APP else "chromium")
    time.sleep(6)  # (Flathub's offers come in)
    vm.shot("1-launcher.png")
    vm.key("ret")
    got = vm.wait_for(rf"launcher: installed {re.escape(APP)}|launcher: .*install.* failed|couldn't install", 1200, m)
    results["installed from Flathub"] = bool(got) and "installed" in got.group(0) and "fail" not in got.group(0)
    launched = vm.wait_for(rf"(spawned|launched) \S+ \(pid \d+\): .*{re.escape(APP)}", 60, m)
    if not launched:  # (opened by hand when the install didn't open it)
        m2 = len(vm.log_text())
        vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
        time.sleep(1.5)
        vm.type("firefox" if "firefox" in APP else "chromium")
        time.sleep(3)
        vm.key("ret")
        launched = vm.wait_for(rf"(spawned|launched) \S+ \(pid \d+\): .*{re.escape(APP)}", 30, m2)
    results["opened"] = bool(launched)
    m = len(vm.log_text()) if not launched else launched.start() + m
    window = vm.wait_for(r"wayland: client \S+ window: first buffer", 120, m)
    results["its sandbox starts (no bubblewrap refusal)"] = "bwrap: cannot open" not in vm.log_text()
    results["it draws a window"] = bool(window)
    time.sleep(8)
    vm.shot("2-browser.png")
finally:
    vm.stop()
for l in re.findall(r".*(?:bwrap|flatpak-|F: |launcher: install|wayland: client).*", vm.log_text())[-14:]:
    print("  " + l[:200])
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
