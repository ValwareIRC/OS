#!/usr/bin/env bash
# Boots a copy of the image whose limine.conf passes os.* boot parameters
# and checks the kernel turned them into the AI config.
set -e
cd "$(dirname "$0")/.."
T=build/cmdline-test
rm -rf $T && cp -r build/iso_root $T
sed -i 's|^    path: boot():/boot/kernel.elf|&\n    cmdline: quiet os.provider=openrouter os.key=sk-or-test os.model=some/model|' $T/boot/limine/limine.conf
grep cmdline $T/boot/limine/limine.conf
xorriso -as mkisofs -R -r -J -b boot/limine/limine-bios-cd.bin -no-emul-boot -boot-load-size 4 -boot-info-table \
    -hfsplus -apm-block-size 2048 --efi-boot boot/limine/limine-uefi-cd.bin -efi-boot-part --efi-boot-image \
    --protective-msdos-label $T -o build/cmdline-test.iso 2>/dev/null
vendor/limine/limine bios-install build/cmdline-test.iso 2>/dev/null
timeout 60 qemu-system-x86_64 -M q35 -m 2G -smp 2 -enable-kvm -cpu host -display none -no-reboot \
    -serial file:build/cmdline-test.log -cdrom build/cmdline-test.iso &
for i in $(seq 1 50); do grep -q "shell: mind" build/cmdline-test.log 2>/dev/null && break; sleep 1; done
kill %1 2>/dev/null || true
grep -E "boot parameters|shell: mind" build/cmdline-test.log
rm -rf $T build/cmdline-test.iso
