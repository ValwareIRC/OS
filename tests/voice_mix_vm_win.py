"""A voice of its own: the companion picks a voice, mixes it with another into a new one, then shapes it (pitch,
formants, an effect), each heard right away; what the sound card played is recorded to a file (never to the
speakers) and checked for speech. tests/fake_ai.py plays the companion; Kokoro voices come from the internet.
Windows QEMU, a fresh disk.

  python tests\\voice_mix_vm_win.py [OUTDIR]"""
import os, re, struct, sys, time, wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "voice-mix-vm"))
os.makedirs(out, exist_ok=True)
wav = os.path.join(out, "played.wav")
if os.path.exists(wav):
    os.remove(wav)
os.environ["AUDIO_WAV"] = wav
replies = [
    {"when": "pick a voice", "reply": "Trying this one. [[voice:use af_heart]]"},
    {"when": "mix it", "reply": "A mix, then. [[voice:mix af_heart 60, bm_george 40]]"},
    {"when": "shape it", "reply": "Lower, in a small room. [[voice:shape pitch -2 formant -1 effect room]]"},
    {"when": "say something", "reply": "This is how I sound now, after all that. Is it right?"},
] + ["Okay."] * 10
results = {}
vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "8G"), qport=4482, aiport=8926)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 240)
    time.sleep(4)
    vm.focus_companion()
    m = len(vm.log_text())
    vm.say("pick a voice", r"shell: ai Trying", 60)
    results["a voice is fetched"] = bool(vm.wait_for(r"voice: af_heart arrived|voice: now af_heart", 60, m))
    time.sleep(3)
    m = len(vm.log_text())
    vm.focus_companion()
    vm.say("mix it", r"shell: ai A mix", 60)
    results["mixed into a new voice"] = bool(vm.wait_for(r"voice: now a mix of af_heart 60, bm_george 40", 90, m))
    time.sleep(3)
    m = len(vm.log_text())
    vm.focus_companion()
    vm.say("shape it", r"shell: ai Lower", 60)
    results["shaped"] = bool(vm.wait_for(r"voice: shaped: pitch=-2.0 formant=-1.0 effect=room", 10, m))
    time.sleep(3)
    m = len(vm.log_text())
    vm.focus_companion()
    vm.say("say something", r"shell: ai This is how I sound", 60)
    said = vm.wait_for(r"voice: said \((amix_[0-9a-f]{8})\) in [0-9.]+s(.*)", 60, m)
    results["spoken in the mix, through the shaping, without an error"] = bool(said) and not said.group(2).strip()
    time.sleep(3)
finally:
    vm.stop()
# what was played: speech-like (loud enough, for long enough)
try:
    raw = open(wav, "rb").read()  # (QEMU is stopped, not closed: the header's lengths are never filled in)
    ch, rate = struct.unpack_from("<HI", raw, 22)
    data = raw[44:]
    s = struct.unpack(f"<{len(data) // 2}h", data[: len(data) // 2 * 2])[::ch]
    win = rate // 10
    loud = sum(1 for i in range(0, len(s) - win, win) if max(abs(x) for x in s[i:i + win]) > 1500)
    print(f"  recorded {len(s) / rate:.1f} s, {loud / 10:.1f} s of it with sound")
    results["the sound card played speech"] = loud >= 20
except Exception as e:
    print("  no recording:", e)
    results["the sound card played speech"] = False
ok = all(results.values())
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
