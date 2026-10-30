#!/usr/bin/env bash
# Headless boot test: boots the ISO in QEMU (UEFI, then BIOS), captures the
# serial log and checks that every self-test and the final boot marker appear.
# The kernel exits QEMU through isa-debug-exit: code 1 = success, 3 = panic.
# arm64 boots UEFI only and powers off through PSCI, which has no exit status, so a panic shows only in the log.
set -u

ARCH=${ARCH:-x86_64}
ISO=${ISO:-build/os.iso}
QEMU=${QEMU:-qemu-system-x86_64}
TIMEOUT=${TIMEOUT:-600} # the Flatpak tests download an app from Flathub
OUT=$(dirname "$ISO")
EXPECT=(
    "cpu: gdt, tss and idt loaded"
    "mm: running on kernel page tables"
    "selftest: interrupts ok"
    "selftest: pmm ok"
    "selftest: vmm ok"
    "selftest: heap ok"
    "smp: "
    "selftest: scheduler ok"
    "selftest: preemption ok"
    "hda: codec"
    "OS BOOT OK"
    "init: user space is alive (pid 1)"
    "ahci: port 0: 1024 MiB disk (QEMU HARDDISK), /dev/sda"
    "utest: disk: unaligned write near 1 MiB keeps its neighbours ok"
    "USERLAND TESTS PASSED"
)
[ "$ARCH" = arm64 ] && EXPECT[0]="cpu: exception vectors loaded"

run_one() {
    local name=$1; shift
    local log=$OUT/boot-$name.log
    # A fresh 1 GiB (sparse) scratch disk on the q35 AHCI controller's first port
    # (the -cdrom drive sits on a later port). init's disk tests write to it.
    local disk=$OUT/test-disk.img
    rm -f "$disk" && truncate -s 1G "$disk"
    timeout "$TIMEOUT" "$QEMU" ${QEMU_MACHINE:--M q35 -cpu max -device isa-debug-exit,iobase=0xf4,iosize=0x04} \
        -m 6G -smp 4 -no-reboot -display none \
        -fw_cfg name=opt/os/mode,string=test ${OS_STRACE:+-fw_cfg name=opt/os/strace,string=$OS_STRACE} -netdev user,id=n0 -device e1000,netdev=n0 \
        -drive id=d0,file="$disk",format=raw,if=none -device ide-hd,drive=d0,bus=${DISK_BUS:-ide.0} \
        -audiodev wav,id=snd0,path=$OUT/test-audio.wav -device intel-hda -device hda-duplex,audiodev=snd0 \
        -serial file:"$log" -cdrom "$ISO" "$@"
    local code=$?
    local ok=1
    for line in "${EXPECT[@]}"; do
        if ! grep -qF "$line" "$log"; then
            echo "[$name] missing: $line"
            ok=0
        fi
    done
    if [ "$ARCH" = arm64 ]; then
        if [ "$code" -ne 0 ] || grep -qF "KERNEL PANIC" "$log"; then
            echo "[$name] QEMU exit code $code, or a kernel panic in the log"
            ok=0
        fi
    elif [ "$code" -ne 1 ]; then
        echo "[$name] QEMU exit code $code (expected 1)"
        ok=0
    fi
    # The disk file system the OS wrote (/dev/sda2, from 320 MiB): checked by
    # the host's own e2fsck, read-only and forced, so any damage fails the run.
    if [ $ok -eq 1 ] && grep -qF "ext2 tests done" "$log"; then
        if e2fsck -fn "$disk?offset=$((320 << 20))" > $OUT/boot-$name-fsck.log 2>&1; then
            echo "[$name] e2fsck: the OS's file system is clean"
        else
            echo "[$name] e2fsck found problems:"; sed 's/^/    /' $OUT/boot-$name-fsck.log
            ok=0
        fi
    fi
    if [ $ok -eq 1 ]; then
        echo "[$name] PASS"
    else
        echo "[$name] FAIL, serial log:"; sed 's/^/    /' "$log"
    fi
    return $((1 - ok))
}

status=0
if [ -n "${OVMF:-}" ] && [ -f "$OVMF" ]; then
    cp "${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}" $OUT/ovmf_vars.fd
    run_one uefi -drive if=pflash,format=raw,readonly=on,file="$OVMF" -drive if=pflash,format=raw,file=$OUT/ovmf_vars.fd || status=1
elif [ "$ARCH" = arm64 ]; then
    echo "[uefi] FAIL: AAVMF firmware not found (apt install qemu-efi-aarch64), and arm64 has no BIOS run"
    status=1
else
    echo "[uefi] SKIP: OVMF firmware not found (apt install ovmf)"
fi
[ "$ARCH" = arm64 ] || run_one bios || status=1
exit $status
