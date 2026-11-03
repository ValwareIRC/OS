"""os-voicefx on the host: a tone shifted by the right number of semitones at the same length, formants moved
without the pitch, and every effect running on a real voice clip without silence, clipping or blow-ups.

  python3 tests/voicefx_test.py BIN CLIP.raw OUTDIR   (BIN: a host build of userland/tools/os-voicefx.c)"""
import math, os, struct, subprocess, sys, wave

binp, clip, out = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
R = 24000
ok = True


def run(pcm, *args):
    return subprocess.run([binp, str(R), *args], input=pcm, capture_output=True, check=True).stdout


def samples(b):
    return struct.unpack(f"<{len(b) // 2}h", b[: len(b) // 2 * 2])


def freq(s):  # by zero crossings, in the steady middle
    mid = s[len(s) // 4: 3 * len(s) // 4]
    z = sum(1 for a, b in zip(mid, mid[1:]) if a < 0 <= b)
    return z / (len(mid) / R)


def check(name, cond, detail=""):
    global ok
    ok &= bool(cond)
    print(f"{'PASS' if cond else 'FAIL'} {name} {detail}")


tone = struct.pack(f"<{R * 2}h", *[int(9000 * math.sin(2 * math.pi * 220 * i / R)) for i in range(R * 2)])
f0 = freq(samples(tone))
for st in (-5, 4, 7):
    o = samples(run(tone, f"pitch={st}"))
    want = 220 * 2 ** (st / 12)
    got = freq(o)
    check(f"pitch {st:+d}", abs(got - want) / want < 0.04, f"({got:.0f} Hz, want {want:.0f})")
    check(f"pitch {st:+d} keeps the length", abs(len(o) - R * 2 - 1024) <= 1024, f"({len(o)} samples)")
o = samples(run(tone, "formant=4"))
check("formant alone keeps the pitch", abs(freq(o) - f0) / f0 < 0.04, f"({freq(o):.0f} Hz)")
voice = open(clip, "rb").read()
src = samples(voice)
rms0 = math.sqrt(sum(x * x for x in src) / len(src))
for args in (["pitch=-3"], ["pitch=3", "formant=2"], ["formant=-3"], ["warmth=0.8"], ["warmth=-0.8"], ["breath=0.6"],
             ["effect=robot"], ["effect=whisper"], ["effect=radio"], ["effect=phone"], ["effect=room"], ["effect=hall"],
             ["pitch=-2", "formant=-2", "warmth=0.4", "breath=0.2", "effect=room"]):
    o = run(voice, *args)
    s = samples(o)
    rms = math.sqrt(sum(x * x for x in s) / max(1, len(s)))
    clipped = sum(1 for x in s if abs(x) >= 32760) / max(1, len(s))
    name = "_".join(a.replace("=", "") for a in args)
    with wave.open(os.path.join(out, name + ".wav"), "wb") as w:
        w.setnchannels(1), w.setsampwidth(2), w.setframerate(R), w.writeframes(o)
    check(" ".join(args), 0.2 * rms0 < rms < 3 * rms0 and clipped < 0.001,
          f"(level {rms / rms0:.2f} of the voice, {clipped * 100:.2f}% at the limit)")
with wave.open(os.path.join(out, "original.wav"), "wb") as w:
    w.setnchannels(1), w.setsampwidth(2), w.setframerate(R), w.writeframes(voice)
print("ALL PASS" if ok else "SOME FAILED")
sys.exit(0 if ok else 1)
