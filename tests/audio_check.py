#!/usr/bin/env python3
"""Checks QEMU's recording of tests/audio_test.sh: a 440 Hz tone followed by
an 880 Hz tone, each about a second long, and nothing else that is loud.

Frequencies come from zero-crossing counts in 50 ms windows (no numpy
needed). Usage: audio_check.py build/audio-test.wav
"""
import struct
import sys


def read_wav(path):
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        sys.exit(f"{path}: not a WAV file")
    pos, fmt, pcm = 12, None, None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = pos + 8
        if cid == b"fmt ":
            fmt = struct.unpack("<HHIIHH", data[body:body + 16])
        elif cid == b"data":
            # QEMU writes the final sizes at exit; trust the file if it didn't.
            if size == 0 or body + size > len(data):
                size = len(data) - body
            pcm = data[body:body + size]
            break
        pos = body + size + (size & 1)
    if not fmt or pcm is None:
        sys.exit(f"{path}: no fmt/data chunk")
    _, channels, rate, _, _, bits = fmt
    if bits != 16:
        sys.exit(f"{path}: {bits}-bit samples, expected 16")
    frames = len(pcm) // (2 * channels)
    samples = struct.unpack(f"<{frames * channels}h", pcm[:frames * channels * 2])
    return rate, samples[0::channels]


def precise_hz(x, rate):
    """Frequency from the first and last interpolated upward zero crossings."""
    ups = []
    for i in range(1, len(x)):
        if x[i - 1] < 0 <= x[i]:
            ups.append(i - 1 + -x[i - 1] / (x[i] - x[i - 1]))
    if len(ups) < 2:
        return 0.0
    return (len(ups) - 1) / ((ups[-1] - ups[0]) / rate)


def main():
    rate, left = read_wav(sys.argv[1])
    win = rate // 20
    print(f"recording: {len(left) / rate:.2f} s at {rate} Hz")
    kinds = []  # per window: None (quiet), 440, 880 or "other"
    for start in range(0, len(left) - win + 1, win):
        w = left[start:start + win]
        rms = (sum(s * s for s in w) / win) ** 0.5
        if rms < 500:
            kinds.append(None)
            continue
        # Count sign changes with a little hysteresis so noise can't add any.
        crossings, state = 0, 0
        for s in w:
            if s > 200 and state <= 0:
                crossings += state < 0
                state = 1
            elif s < -200 and state >= 0:
                crossings += state > 0
                state = -1
        hz = crossings / 2 / (win / rate)
        kind = "other"
        for f in (440, 880):
            if abs(hz - f) < f * 0.06:
                kind = f
        kinds.append(kind)

    secs = {f: kinds.count(f) / 20 for f in (440, 880)}
    other = kinds.count("other")
    ok = True
    for f in (440, 880):
        if not secs[f]:
            continue
        first = kinds.index(f)
        last = len(kinds) - 1 - kinds[::-1].index(f)
        hz = precise_hz(left[first * win:(last + 1) * win], rate)
        print(f"{f} Hz tone: {secs[f]:.2f} s, measured {hz:.1f} Hz")
        if abs(hz - f) > f * 0.01:
            print(f"FAIL: {f} Hz tone is off by more than 1%")
            ok = False
    print(f"loud windows of any other frequency: {other}")
    # Within a tone a clean recording never jumps by more than the 880 Hz
    # sine's steepest slope; a skipped or replayed buffer or garbage does.
    # Steps to or from exact silence are where a tone starts or ends (gaps
    # inside a tone show up in the durations above instead).
    slope = 2 * 3.14159265 * 880 * 12000 / rate
    jump = max((abs(left[i] - left[i - 1]) for i in range(1, len(left)) if left[i] and left[i - 1]), default=0)
    print(f"largest sample-to-sample step: {jump} (a clean 880 Hz tone: {slope:.0f})")
    if jump > slope * 1.5:
        print("FAIL: discontinuity (click, dropout or garbage)")
        ok = False
    if secs[440] < 0.8 or secs[880] < 0.8:
        print("FAIL: expected about 1 s of each tone")
        ok = False
    if secs[440] and secs[880] and kinds.index(440) > kinds.index(880):
        print("FAIL: 880 Hz came before 440 Hz")
        ok = False
    if other > 4:  # a window or two where a tone starts or stops is fine
        print("FAIL: unexpected sound (garbage, clicks or a wrong rate)")
        ok = False
    sys.exit(0 if ok else 1)


main()
