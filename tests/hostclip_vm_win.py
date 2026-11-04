"""The host's clipboard reaches the desktop over COM2 (as scripts\\os-win.ps1 connects it), now by interrupt:
Windows QEMU, silent sound. Text is sent the way scripts\\clipboard-bridge.ps1 sends it (0x02 text 0x03), pasted
into the companion's input with Ctrl+V and sent: the fake mind must receive it. And the port isn't read every
frame any more: the desktop's "input" share of its time stays small.

  set ISO=build\\dev.iso & python tests\\hostclip_vm_win.py"""
import os, re, socket, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
out = os.path.join(ROOT, "build", "hostclip")
os.makedirs(out, exist_ok=True)
PORT = 4597
os.environ["HOSTCLIP_PORT"] = str(PORT)
results = {}
vm = vmkit.Vm(out, ["Got it."] * 10, memory=os.environ.get("MEM", "8G"), qport=4488, aiport=8932)
try:
    vm.start()
    results["the desktop starts"] = bool(vm.wait_for(r"shell: screen desktop", 300))
    s = None
    for _ in range(50):
        try:
            s = socket.create_connection(("127.0.0.1", PORT), timeout=5)
            break
        except OSError:
            time.sleep(0.2)
    results["COM2 is there"] = s is not None
    text = "pasted from the PC, by interrupt " + str(int(time.time()))
    if s:
        s.sendall(b"\x02" + text.encode() + b"\x03")
    time.sleep(2)
    vm.focus_companion()
    vm.cmd("send-key", keys=[{"type": "qcode", "data": "ctrl"}, {"type": "qcode", "data": "v"}])
    time.sleep(1)
    vm.key("ret")
    t0 = time.time()
    got = False
    while time.time() - t0 < 60 and not got:
        got = text in open(vm.ailog, encoding="utf-8", errors="replace").read()
        time.sleep(1)
    results["what the PC copied arrives, pasted"] = got
    time.sleep(35)  # (a whole 30 s of the desktop's own timing in the log)
    lines = re.findall(r"shell: time: (?:asleep|whole frame).*", vm.log_text())
    shares = [float(x) for x in re.findall(r"\binput ([0-9.]+)%", lines[-1])] if lines else []
    print(f"  input's share of the desktop's time: {shares or 'too small to be listed'}", flush=True)
    results["reading input is cheap (under 5% of a core)"] = bool(lines) and (not shares or shares[0] < 5)
finally:
    vm.stop()
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
