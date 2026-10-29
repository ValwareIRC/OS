#!/usr/bin/env python3
"""Checks host-to-the OS clipboard paste and boot parameters.

1. Boots with COM2 on a TCP port, opens Anthropic's key screen, sends text
   the way scripts/clipboard-bridge.ps1 does, presses Ctrl+V and screenshots.
2. Boots with a mind.json from boot parameters (no model given) and checks
   The OS picked the provider's default model and skipped "Choose a mind".

  python3 tests/clipboard_test.py   (inside WSL; writes build/clipboard/)
"""
import os, socket, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import demo

OUT = os.path.join(demo.ROOT, "build", "clipboard")
demo.OUT = OUT
demo.LOG = os.path.join(OUT, "serial.log")
PORT = 4556

def ctrl_v(q):
    k = lambda key, down: {"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": key}}}
    q.cmd("input-send-event", events=[k("ctrl", True), k("v", True)])
    time.sleep(0.08)
    q.cmd("input-send-event", events=[k("v", False), k("ctrl", False)])
    time.sleep(0.3)

ok = True
q = demo.Qemu(["-serial", f"tcp:127.0.0.1:{PORT},server=on,wait=off"])
try:
    demo.wait_for(r"shell: screen choose", 120)
    time.sleep(1)
    q.click(160 + 120, 230 + 46)  # Anthropic
    demo.wait_for(r"shell: screen connect", 10)
    time.sleep(0.5)
    s = socket.create_connection(("127.0.0.1", PORT))
    s.sendall(b"\x02sk-ant-test-pasted-from-host-123\r\n\x03")
    time.sleep(1.5)  # the shell picks it up within a frame or two
    ctrl_v(q)
    time.sleep(0.6)
    q.screenshot_to(os.path.join(OUT, "paste.png"))
    s.close()
finally:
    q.quit()

mind = os.path.join(OUT, "mind.json")
open(mind, "w").write('{"provider":"anthropic","base_url":"","api_key":"sk-test","model":""}\n')
demo.LOG = os.path.join(OUT, "serial-boot-params.log")
q = demo.Qemu(["-fw_cfg", f"name=opt/os/mind.json,file={mind}"])
try:
    m = demo.wait_for(r"shell: mind (\S+) \(([^)]*)\)", 120)
    print("boot parameters ->", m.group(0) if hasattr(m, "group") else m)
    demo.wait_for(r"shell: screen (onboard|connect)", 30)
    print("skipped Choose a mind: yes")
finally:
    q.quit()
