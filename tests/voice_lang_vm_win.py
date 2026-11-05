"""A mix of voices that is mostly a Chinese one still reads English. Valerie and Mita found it: a mix is named
after its biggest share ("zmix_..." for one mostly zf_xiaoxiao), and os-speak took the reading language from
that first letter, so English came out as silence, or only its digits, in Chinese. Now the line itself decides
(voice.c, speak_lang). The companion (the test's stand-in AI) makes that mix, downloading both voices, then
answers in English: it must be read as English and take as long as a sentence does, with no espeak-ng failure.

  set ISO=build\\dev.iso & python tests\\voice_lang_vm_win.py [OUTDIR]"""
import os, re, struct, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vmkit

out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else os.path.join(vmkit.ROOT, "build", "voice-lang-vm"))
os.makedirs(out, exist_ok=True)
disk = os.path.join(out, "disk.img")
vmkit.Vm.new_disk(disk, 4 << 30)
os.environ["DISK_RW"] = "1"
wav = os.path.join(out, "played.wav")  # what the sound card played (never the speakers): checked for speech
if os.path.exists(wav):
    os.remove(wav)
os.environ["AUDIO_WAV"] = wav
MIX = "[[voice:mix zf_xiaoxiao 65, af_sarah 35]] Making my new voice now."
SAY = "This is me in the new mix, reading English words the way they should sound."
results = {}
replies = [{"when": "make your voice", "reply": MIX}, {"when": "how do you sound", "reply": SAY}] + ["Okay."] * 10
vm = vmkit.Vm(out, replies, memory=os.environ.get("MEM", "8G"), disk=disk, qport=4490, aiport=8934)
try:
    vm.start()
    vm.wait_for(r"shell: screen desktop", 300)
    time.sleep(5)
    vm.focus_companion()
    vm.say("make your voice mostly xiaoxiao", r"shell: ai ", 60)
    made = vm.wait_for(r"voice: now a mix of zf_xiaoxiao 65, af_sarah 35", 240)
    print("mix:", made.group(0) if made else "-", flush=True)
    time.sleep(20)  # (the voices downloaded, the mix made and chosen)
    tries = 0
    spoke = None
    while tries < 4 and not spoke:
        tries += 1
        m = len(vm.log_text())
        vm.focus_companion()
        vm.say("how do you sound now", r"shell: ai This is me", 60)
        spoke = vm.wait_for(r"voice: speaking \((zmix_[0-9a-f]+)[^)]*\), read as ([a-z-]+)\n(?:.*\n)*?"
                            r".*voice: said \(zmix_[0-9a-f]+[^)]*\) in ([0-9.]+)s", 60, m)
        if not spoke:
            time.sleep(15)
    text = vm.log_text()
    for line in re.findall(r"shell: voice: .*", text)[-8:]:
        print("  " + line[:180], flush=True)
    results["the mix is mostly the Chinese voice (a z name)"] = bool(spoke)
    if spoke:
        print(f"spoke with {spoke.group(1)}, read as {spoke.group(2)}, for {spoke.group(3)} s", flush=True)
        results["an English line is read as English"] = spoke.group(2).startswith("en")
        results["it takes as long as a sentence (not 1.5 s of failure)"] = float(spoke.group(3)) >= 3.0
    results["no espeak-ng failure"] = "Failed to set eSpeak-ng voice" not in text
finally:
    vm.stop()
try:
    raw = open(wav, "rb").read()  # (QEMU stopped, not closed: the header's lengths are never filled in)
    ch, rate = struct.unpack_from("<HI", raw, 22)
    data = raw[44:]
    s = struct.unpack(f"<{len(data) // 2}h", data[: len(data) // 2 * 2])[::ch]
    w = rate // 10
    loud = sum(1 for i in range(0, len(s) - w, w) if max(abs(x) for x in s[i:i + w]) > 1500)
    print(f"  recorded {len(s) / rate:.1f} s, {loud / 10:.1f} s of it with sound")
    results["the sound card played speech"] = loud >= 20
except Exception as e:
    print("  no recording:", e)
    results["the sound card played speech"] = False
for k, v in results.items():
    print(f"{'PASS' if v else 'FAIL'} {k}")
print("ALL PASS" if results and all(results.values()) else "SOME FAILED")
