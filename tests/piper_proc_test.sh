#!/usr/bin/env bash
# Boots the image with OS_MODE=piper: init runs /opt/piper/piper with no
# LD_LIBRARY_PATH (its RUNPATH $ORIGIN must resolve through /proc/self/exe)
# and reports whether a WAV file came out.
cd "$(dirname "$0")/.."
LOG=build/piper-proc.log
KVM="-cpu max"
[ -w /dev/kvm ] && KVM="-enable-kvm -cpu host"
timeout ${TIMEOUT:-600} qemu-system-x86_64 -M q35 -m 6G -smp 4 $KVM -no-reboot -display none \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -fw_cfg name=opt/os/mode,string=piper -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial file:"$LOG" -cdrom build/os.iso
grep -a -E "PIPER|piper|error|Error|terminate|wav|segmentation|exception" "$LOG" | tail -30
grep -q "PIPER OK" "$LOG"
