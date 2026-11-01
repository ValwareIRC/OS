"""Turns Kokoro clips (raw 16-bit mono at RATE) into 16 kHz WAVs for whisper-cli, and
reports each clip's length. Usage: voice_clip_check.py CLIPDIR OUTDIR [names...]"""
import os
import struct
import sys
import wave

clips, out = sys.argv[1], sys.argv[2]
rate = int(open(os.path.join(clips, "RATE")).read().strip()) if os.path.exists(os.path.join(clips, "RATE")) else 24000
os.makedirs(out, exist_ok=True)
names = sys.argv[3:] or sorted(n[:-4] for n in os.listdir(clips) if n.endswith(".raw"))
for n in names:
    raw = open(os.path.join(clips, n + ".raw"), "rb").read()
    s = struct.unpack("<%dh" % (len(raw) // 2), raw[: len(raw) // 2 * 2])
    step = rate / 16000
    o = []
    i = 0.0
    while int(i) + 1 < len(s):
        a, b = s[int(i)], s[int(i) + 1]
        f = i - int(i)
        o.append(int(a + (b - a) * f))
        i += step
    peak = max((abs(v) for v in s), default=0)
    w = wave.open(os.path.join(out, n + ".wav"), "wb")
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(16000)
    w.writeframes(struct.pack("<%dh" % len(o), *o))
    w.close()
    print("%-12s %.2fs peak %d" % (n, len(s) / rate, peak))
