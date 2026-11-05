"""The keyboard and the pointer work after a boot during which the host's mouse kept moving over the window, as
Valerie's does in full screen: QEMU queues the absolute pointer's moves from the moment the kernel asks for it
(its first step), so the second ask, when input starts, must read through them (it took them for no vmmouse,
and neither the pointer nor the keyboard worked). Also: the clock calibrated well.

  set ISO=build\\dev.iso & python tests\\input_vm_win.py [OUTDIR]"""
import os, random, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "input-vm"))
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 4 << 30)
results = {}

vm = vmkit.Vm(out, ["Okay."] * 10, memory=os.environ.get("MEM", "8G"), disk=disk, qport=4488, aiport=8932)
try:
    vm.start()
    # the host's mouse moving over the window all through the boot
    t0 = time.time()
    moves = 0
    while time.time() - t0 < 300 and "shell: screen desktop" not in vm.log_text():
        try:
            vm.cmd("input-send-event", events=[
                {"type": "abs", "data": {"axis": "x", "value": random.randint(0, 32767)}},
                {"type": "abs", "data": {"axis": "y", "value": random.randint(0, 32767)}}])
            moves += 1
        except Exception:
            pass  # (QMP not up yet)
        time.sleep(0.05)
    print(f"{moves} pointer moves during the boot", flush=True)
    text = vm.log_text()
    clock = re.search(r"counter (\d+) kHz \(sure to (\d+) ppm, against ([^)]*)\)", text)
    print("clock:", clock.group(0) if clock else "?", flush=True)
    results["the clock calibrated to within 0.2%"] = bool(clock) and int(clock.group(2)) <= 2000
    results["the pointer is absolute (vmmouse)"] = "absolute (vmmouse) mouse" in text
    time.sleep(8)
    # the keyboard: Alt+Space opens the launcher, Esc closes it
    m = len(vm.log_text())
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "spc"}])
    results["the keyboard: Alt+Space opens the launcher"] = bool(vm.wait_for(r"launcher: open", 10, m))
    m = len(vm.log_text())
    vm.key("esc")
    results["the keyboard: Esc closes it"] = bool(vm.wait_for(r"launcher: close", 10, m))
    time.sleep(2)
    # the pointer: a click on the dock's Notes opens it
    rects = re.findall(r"rect ui-app-notes (\d+) (\d+) (\d+) (\d+)", vm.log_text())
    rects = [tuple(map(int, r)) for r in rects if int(r[2]) > 0]
    if rects:
        x, y, w, h = rects[-1]
        m = len(vm.log_text())
        vm.click(x + w // 2, y + h // 2)
        results["the pointer: a click on the dock opens Notes"] = bool(
            vm.wait_for(r'ui action open:notes|window \(0\) opened "Notes"', 10, m))
        time.sleep(2)
        vm.cmd("send-key", keys=[{"type": "qcode", "data": "alt"}, {"type": "qcode", "data": "f4"}])
        time.sleep(2)
        # right-click: a menu (not a question to the companion), and its "Open Notes" opens it
        m = len(vm.log_text())
        vm.cmd("input-send-event", events=[
            {"type": "abs", "data": {"axis": "x", "value": int((x + w // 2) * 32767 / (vmkit.W - 1))}},
            {"type": "abs", "data": {"axis": "y", "value": int((y + h // 2) * 32767 / (vmkit.H - 1))}}])
        time.sleep(0.3)
        vm.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "right"}}])
        time.sleep(0.1)
        vm.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "right"}}])
        item = vm.wait_for(r"rect context-menu-0 (\d+) (\d+) (\d+) (\d+)", 10, m)
        results["right-click opens a menu"] = bool(item)
        results["right-click asks the companion nothing"] = "Why does the" not in vm.log_text()[m:]
        vm.shot("context-menu.png")
        if item:
            ix, iy, iw, ih = map(int, item.groups())
            m = len(vm.log_text())
            vm.click(ix + iw // 2, iy + ih // 2)
            results["its Open opens Notes"] = bool(vm.wait_for(r'context menu: open:notes', 10, m))
    else:
        results["the pointer: a click on the dock opens Notes"] = False
        print("(no Notes on the dock in this design)")
    # the companion answers (its turn is asked on a thread now, not a fork of the desktop)
    time.sleep(2)
    vm.focus_companion()
    results["the companion answers"] = bool(vm.say("hello there", r"shell: ai Okay", 60))
    results["nothing crashed"] = "segmentation fault" not in vm.log_text()
    vm.shot("end.png")
finally:
    vm.stop()
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if all(results.values()) else "SOME FAILED")
