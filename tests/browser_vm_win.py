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


def open_it():
    """Alt+Space, its name, Enter: as a person opens an app."""
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    time.sleep(1.5)
    vm.type("firefox" if "firefox" in APP else "chromium")
    time.sleep(3)
    vm.key("ret")


def content_shows(name):
    """Whether the window shows its page and toolbar (hundreds of colours), not one flat colour (an empty or black
    window: one or two; its content in a subsurface once showed black)."""
    ppm = os.path.join(out, "content.ppm")
    vm.cmd("screendump", filename=ppm.replace(os.sep, "/"))
    time.sleep(0.2)
    data = open(ppm, "rb").read().split(b"\n", 3)
    w, px = int(data[1].split()[0]), data[3]
    colours = {px[(y * w + x) * 3:(y * w + x) * 3 + 3] for y in range(200, 800, 5) for x in range(300, 1300, 5)}
    if len(colours) > 40:
        vm.shot(name + ".png")
        return True
    return False


def time_open(since, t0, name):
    """Seconds from t0 to its first window, and to its page showing (None: not within 90 s)."""
    win = page = None
    while time.time() - t0 < 90 and page is None:
        if win is None and re.search(r"wayland: client \S+ window: first buffer", vm.log_text()[since:]):
            win = time.time() - t0
        if win is not None and content_shows(name):
            page = time.time() - t0
        time.sleep(0.5)
    print(f"  {name}: window after {win if win is not None else '-'} s, page after {page if page is not None else '-'} s",
          flush=True)
    return win, page

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
    t_install = time.time()
    got = vm.wait_for(rf"launcher: installed {re.escape(APP)}|launcher: .*install.* failed|couldn't install", 1200, m)
    results["installed from Flathub"] = bool(got) and "installed" in got.group(0) and "fail" not in got.group(0)
    # alone it takes about two and a half minutes (tests/flatpak_install_win.py): the desktop's own downloads wait
    print(f"install took {time.time() - t_install:.0f} s", flush=True)
    print(f"PERF firefox_install_s={time.time() - t_install:.0f}", flush=True)
    results["installed within 5 minutes"] = time.time() - t_install < 300
    launched = vm.wait_for(rf"(spawned|launched) \S+ \(pid \d+\): .*{re.escape(APP)}", 60, m)
    t_open = time.time()
    if not launched:  # (opened by hand when the install didn't open it)
        m2 = len(vm.log_text())
        open_it()
        t_open = time.time()
        launched = vm.wait_for(rf"(spawned|launched) \S+ \(pid \d+\): .*{re.escape(APP)}", 30, m2)
    results["opened"] = bool(launched)
    m = len(vm.log_text()) if not launched else launched.start() + m
    win_s, page_s = time_open(m, t_open, "2-browser")
    results["its sandbox starts (no bubblewrap refusal)"] = "bwrap: cannot open" not in vm.log_text()
    results["it draws a window"] = win_s is not None
    results["its content shows (not a black window)"] = page_s is not None
    print(f"PERF firefox_first_window_s={win_s if win_s is not None else 999:.1f}", flush=True)
    print(f"PERF firefox_first_page_s={page_s if page_s is not None else 999:.1f}", flush=True)
    # the second time: quit it (Ctrl+Q), open it again from the launcher
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"}, {"type": "qcode", "data": "q"}])
    time.sleep(10)
    m = len(vm.log_text())
    open_it()
    win2, page2 = time_open(m, time.time(), "3-again")
    results["opens a second time"] = page2 is not None
    print(f"PERF firefox_again_window_s={win2 if win2 is not None else 999:.1f}", flush=True)
    print(f"PERF firefox_again_page_s={page2 if page2 is not None else 999:.1f}", flush=True)
finally:
    vm.stop()
for l in re.findall(r".*(?:bwrap|flatpak-|F: |launcher: install|wayland: client).*", vm.log_text())[-14:]:
    print("  " + l[:200])
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
