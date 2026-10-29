#!/usr/bin/env bash
# End-to-end sound test: boots the OS with QEMU's HDA controller recording its
# output to a WAV file. init (OS_MODE=audio) plays 1 s of 440 Hz at 48 kHz
# stereo, then 1 s of 880 Hz as 22.05 kHz mono (converted by the driver), and
# tests/audio_check.py checks both tones are in the recording.
#
# use-timer=off: QEMU's codec then fetches DMA data as its audio backend
# consumes it. In the default timer mode QEMU's codec occasionally drops its
# own 8 KiB buffer when the host is busy (an emulator-side overrun), which
# would make this test flaky for reasons outside the OS.
set -u
cd "$(dirname "$0")/.."
ISO=${ISO:-build/os.iso}
QEMU=${QEMU:-qemu-system-x86_64}
WAV=build/audio-test.wav
LOG=build/audio-test.log
rm -f "$WAV" "$LOG"

ACCEL=(-cpu max)
if [ -w /dev/kvm ]; then ACCEL=(-enable-kvm -cpu host); fi

timeout "${TIMEOUT:-120}" "$QEMU" -M q35 -m 6G -smp 4 "${ACCEL[@]}" -no-reboot -display none \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -fw_cfg name=opt/os/mode,string=audio \
    -audiodev wav,id=snd0,path="$WAV" -device intel-hda -device hda-duplex,audiodev=snd0,use-timer=off \
    -serial file:"$LOG" -cdrom "$ISO"

grep -E "^hda:|^audio:" "$LOG"
if ! grep -q "AUDIO TEST DONE" "$LOG"; then
    echo "[audio] FAIL: the guest did not finish playing, serial log:"
    sed 's/^/    /' "$LOG"
    exit 1
fi
if python3 tests/audio_check.py "$WAV"; then
    echo "[audio] PASS"
else
    echo "[audio] FAIL"
    exit 1
fi
