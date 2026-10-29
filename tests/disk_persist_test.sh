#!/usr/bin/env bash
# Disk persistence across a power cycle.
#   boot 1: init (OS_MODE=persist-write:TOKEN) writes a fresh token to
#           /dev/sda, fsyncs and powers off
#   boot 2: same disk image, init (persist-check:TOKEN) must read it back;
#           under the other firmware when OVMF is available, so the data
#           also survives a change of boot path
#   boot 3: a check for a token that was never written must fail, so the
#           check cannot pass by accident
# The mode reaches init through fw_cfg (opt/os/mode), like the boot test.
set -u
cd "$(dirname "$0")/.."

ISO=${ISO:-build/os.iso}
QEMU=${QEMU:-qemu-system-x86_64}
OVMF=${OVMF:-$(ls /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd 2>/dev/null | head -1)}
TIMEOUT=${TIMEOUT:-240}
DISK=build/persist-disk.img
TOKEN=$(printf '%08x%04x' "$(date +%s)" "$RANDOM")

rm -f "$DISK" && truncate -s 64M "$DISK"

uefi_args=()
if [ -n "$OVMF" ] && [ -f "$OVMF" ]; then
    cp "${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}" build/persist-vars.fd
    uefi_args=(-drive if=pflash,format=raw,readonly=on,file="$OVMF" -drive if=pflash,format=raw,file=build/persist-vars.fd)
fi

# boot NAME MODE EXPECTED-LINE [firmware args...]
boot() {
    local name=$1 mode=$2 expect=$3; shift 3
    local log=build/persist-$name.log
    timeout "$TIMEOUT" "$QEMU" -M q35 -m 6G -smp 2 -cpu max -no-reboot -display none -nic none \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 -fw_cfg name=opt/os/mode,string="$mode" \
        -drive id=d0,file="$DISK",format=raw,if=none -device ide-hd,drive=d0,bus=ide.0 \
        -serial file:"$log" -cdrom "$ISO" "$@"
    local code=$?
    if [ "$code" -eq 1 ] && grep -qaF "$expect" "$log"; then
        echo "[$name] PASS: $(grep -a 'PERSIST' "$log")"
        return 0
    fi
    echo "[$name] FAIL (QEMU exit $code, expected \"$expect\"), serial log:"
    sed 's/^/    /' "$log"
    return 1
}

status=0
boot write "persist-write:$TOKEN" "PERSIST WRITE OK" "${uefi_args[@]}" || status=1
# The token must be in the image file itself, not just in a cache.
if grep -qa "OS-PERSIST $TOKEN" "$DISK"; then
    echo "[host] PASS: token $TOKEN is in $DISK"
else
    echo "[host] FAIL: token $TOKEN not found in $DISK"
    status=1
fi
boot check "persist-check:$TOKEN" "PERSIST CHECK OK" || status=1
boot control "persist-check:0000wrong000" "PERSIST CHECK FAILED" || status=1
exit $status
