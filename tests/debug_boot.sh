#!/usr/bin/env bash
# Boots once under BIOS with system-call tracing and prints the serial log.
# Usage: tests/debug_boot.sh [seconds] [extra qemu args...]
T=${1:-20}; shift || true
timeout "$T" qemu-system-x86_64 -M q35 -m 2G -smp ${SMP:-4} -cpu max -no-reboot -display none \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -fw_cfg name=opt/os/mode,string=test -fw_cfg name=opt/os/strace,string=${STRACE:-1} \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -serial file:build/debug.log -cdrom build/os.iso "$@"
echo "exit=$?"
sed -n '/OS BOOT OK/,$p' build/debug.log
