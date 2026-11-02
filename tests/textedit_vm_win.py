"""Editing text in the OS, with Windows QEMU (silent sound), keys through QMP: the launcher's field takes
arrows, typing in the middle, Ctrl+A and typing over it, Shift+arrows and Backspace on a selection, Ctrl+Backspace,
undo; what it searches for is read from the shell's log.
  python tests\\textedit_vm_win.py [OUTDIR]"""
import os, re, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "textedit-vm"))
os.makedirs(out, exist_ok=True)
vm = vmkit.Vm(out, ["Mm."] * 4, qport=4475, aiport=8919)
results = {}


def chord(*keys):
    vm.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in keys])
    time.sleep(0.12)


def query_is(text, label):
    m = vm.wait_for(r'launcher: query "([^"]*)"[^\n]*\n(?![\s\S]*launcher: query)', 6)
    got = m.group(1) if m else None
    results[label] = got == text
    print(f"  {label}: {got!r}", flush=True)


try:
    vm.start()
    results["desktop"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    time.sleep(8)
    chord("alt", "spc")
    time.sleep(1.5)
    vm.type("hello world")
    time.sleep(1)
    for _ in range(6):
        vm.key("left")
    vm.type(" big")
    time.sleep(1)
    query_is("hello big world", "typing in the middle")
    chord("ctrl", "a")
    vm.type("x")
    time.sleep(1)
    query_is("x", "ctrl+a, typed over")
    vm.type("yz")
    vm.key("left", shift=True)
    vm.key("left", shift=True)
    vm.key("backspace")
    time.sleep(1)
    query_is("x", "shift+left selection deleted")
    vm.type(" two words")
    chord("ctrl", "backspace")
    time.sleep(1)
    query_is("x two ", "ctrl+backspace a word")
    chord("ctrl", "z")
    time.sleep(1)
    query_is("x two words", "undo")
    vm.key("home")
    chord("ctrl", "shift", "right")
    time.sleep(0.5)
    vm.shot("1-selection.png")
    vm.key("delete")
    time.sleep(1)
    query_is(" two words", "ctrl+shift+right, delete")
    # the right-click menu: Copy chosen with the pointer, Paste with the arrows and Enter
    field = re.findall(r"rect launcher-field (\d+) (\d+) (\d+) (\d+)", vm.log_text())
    if field:
        fx, fy, fw, fh = map(int, field[-1])
        chord("ctrl", "a")

        def right_click(x, y):
            vm.cmd("input-send-event", events=[
                {"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / (vmkit.W - 1))}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / (vmkit.H - 1))}}])
            time.sleep(0.3)
            vm.cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "right"}}])
            time.sleep(0.1)
            vm.cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "right"}}])
            time.sleep(0.8)

        mark = len(vm.log_text())
        right_click(fx + 40, fy + fh // 2)
        menu = vm.wait_for(r"rect text-menu (\d+) (\d+) (\d+) (\d+)", 5, mark)
        results["right-click menu opens"] = bool(menu)
        if menu:
            mx, my, mw, mh = map(int, menu.groups())
            row = (mh - 12) // 5
            vm.shot("2-menu.png")
            vm.click(mx + 40, my + 6 + 2 * row + row // 2)  # Copy
            time.sleep(0.8)
            right_click(fx + fw - 20, fy + fh // 2)  # past the end of the text: the caret goes there
            vm.key("down")  # Undo (there is history); Cut and Copy are off without a selection, so next is Paste
            vm.key("down")
            vm.key("ret")
            time.sleep(1)
            query_is(" two words two words", "menu: copy, then paste")
    vm.key("esc")
finally:
    vm.stop()
    for k, v in results.items():
        print(f"{k}: {v}")
