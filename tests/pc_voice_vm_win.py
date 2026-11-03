"""The PC's expressive voice, end to end: with scripts/voice-gpu-server.py running on this PC (Chatterbox on its
graphics card), the companion (tests/fake_ai.py) switches to a cloned voice and says a line with feeling; the OS
sees the PC's voice, asks it to speak with the tone's strength, and the VM's sound card plays it (recorded to a
file, never to the speakers). Windows QEMU, a fresh disk.

  python tests\\pc_voice_vm_win.py [OUTDIR]   (the server must be running: it listens on 127.0.0.1:4572)"""
import json, os, re, struct, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "pc-voice-vm"))
os.makedirs(out, exist_ok=True)
st = json.load(urllib.request.urlopen("http://127.0.0.1:4572/status", timeout=5))
assert st.get("ready"), st
SERVER_LOG = os.path.join(vmkit.ROOT, "build", "voice-gpu", "server.log")
log0 = os.path.getsize(SERVER_LOG)
wav = os.path.join(out, "played.wav")
if os.path.exists(wav):
    os.remove(wav)
os.environ["AUDIO_WAV"] = wav
replies = [
    {"when": "use your new voice", "reply": "All right, this one. [[voice:use pc:heart-xiaoxiao]]"},
    {"when": "how do you feel", "reply": "[[tone:excited]] Honestly? Amazing. You actually got it working!"},
    {"when": "and now", "reply": "And this is me, just talking normally again."},
] + ["Okay."] * 10
results = {}
vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "8G"), qport=4483, aiport=8927)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 240)
    results["the OS sees the PC's voice"] = bool(vm.wait_for(r"voice: the PC's expressive voice is here \(heart-xiaoxiao\)", 90))
    time.sleep(2)
    vm.focus_companion()
    m = len(vm.log_text())
    vm.say("use your new voice", r"shell: ai All right", 60)
    results["switched to it"] = bool(vm.wait_for(r"voice: now pc:heart-xiaoxiao", 10, m))
    time.sleep(8)
    for ask, tone in (("how do you feel", "excited"), ("and now", "")):
        m = len(vm.log_text())
        vm.focus_companion()
        vm.say(ask, r"shell: ai ", 60)
        said = vm.wait_for(r"voice: said \(pc:heart-xiaoxiao" + (", " + tone if tone else "") + r"\) in ([0-9.]+)s(.*)", 90, m)
        results[f"said {'with ' + tone if tone else 'plainly'}, without an error"] = bool(said) and not said.group(2).strip()
        if said:
            print(f"  {ask!r}: spoken in {said.group(1)} s")
        time.sleep(3)
finally:
    vm.stop()
srv = open(SERVER_LOG, encoding="utf-8").read()[log0:]
print("  server:", *[l for l in srv.splitlines() if "said" in l], sep="\n    ")
results["the PC said the excited line strongly"] = "exaggeration 1.1" in srv
results["and the plain one plainly"] = "exaggeration 0.5" in srv
try:
    raw = open(wav, "rb").read()
    ch, rate = struct.unpack_from("<HI", raw, 22)
    s = struct.unpack(f"<{(len(raw) - 44) // 2}h", raw[44: 44 + (len(raw) - 44) // 2 * 2])[::ch]
    win = rate // 10
    loud = sum(1 for i in range(0, len(s) - win, win) if max(abs(x) for x in s[i:i + win]) > 1500)
    print(f"  recorded {len(s) / rate:.1f} s, {loud / 10:.1f} s of it with sound")
    results["the sound card played it"] = loud >= 30
except Exception as e:
    print("  no recording:", e)
    results["the sound card played it"] = False
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
