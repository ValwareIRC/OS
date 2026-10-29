#!/usr/bin/env bash
# Runs the OS in a window (QEMU with KVM inside WSL, shown through WSLg).
# A model server on the Windows host (Ollama on 127.0.0.1:11434) is made
# reachable from the guest at 10.0.2.2:11434, where the OS looks by default.
#
#   scripts/os-vm.sh [path/to/os.iso]
#
# os.conf (next to Operating System.cmd; see os.conf.example) can hand the OS its
# AI settings at boot. COM2 listens on 127.0.0.1:4555 for the host clipboard
# (scripts/clipboard-bridge.ps1), so Ctrl+V works inside the OS.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ISO=${1:-$ROOT/build/os.iso}
[ -f "$ISO" ] || { echo "No image at $ISO. Build it first with: make"; exit 1; }

ACCEL=(-cpu max)
if [ -w /dev/kvm ]; then ACCEL=(-enable-kvm -cpu host); fi

# The guest reaches the Windows host's Ollama at 10.0.2.2:11434 through a
# small bridge (see scripts/host-bridge.sh).
bash "$ROOT/scripts/host-bridge.sh" >/dev/null 2>&1 || true
NET="user,id=n0"

# Boot parameters: os.conf becomes the AI config inside the OS, handed
# over through QEMU's fw_cfg. The key never goes into the disk image.
EXTRA=()
CONF="$ROOT/os.conf"
if [ -f "$CONF" ]; then
    get() { sed -n "s/^$1=//p" "$CONF" | tail -1 | tr -d '\r'; }
    esc() { printf '%s' "$1" | tr -d '"\\'; }
    MIND=$(mktemp /tmp/os-mind.XXXXXX)
    chmod 600 "$MIND"
    printf '{"provider":"%s","base_url":"%s","api_key":"%s","model":"%s"}\n' \
        "$(esc "$(get provider)")" "$(esc "$(get url)")" "$(esc "$(get key)")" "$(esc "$(get model)")" > "$MIND"
    EXTRA+=(-fw_cfg "name=opt/os/mind.json,file=$MIND")
    echo "Using AI settings from os.conf ($(get provider))"
fi

# The person's disk: what the OS remembers across reboots lives here (/dev/sda
# inside). Created empty, 1 GiB and sparse, on first start; OS_DISK picks
# another file. the OS.cmd's Windows launcher uses the same file.
DISK=${OS_DISK:-$ROOT/build/os-disk.img}
if [ ! -f "$DISK" ]; then
    mkdir -p "$(dirname "$DISK")"
    : > "$DISK"
    # On a Windows drive NTFS only keeps the file sparse if told so before
    # it grows; on Linux file systems truncate alone is enough.
    case "$DISK" in
    /mnt/*) fsutil.exe sparse setflag "$(wslpath -w "$DISK")" >/dev/null 2>&1 || true ;;
    esac
    truncate -s 1G "$DISK"
    echo "Created an empty disk for the OS at $DISK"
fi
# Sound and microphone (the companion speaks and listens): an Intel HDA card
# on WSLg's PulseAudio server, or a local PulseAudio/PipeWire, else silent.
AUDIO=(-audiodev none,id=snd0)
if [ -S /mnt/wslg/PulseServer ]; then
    AUDIO=(-audiodev pa,id=snd0,server=unix:/mnt/wslg/PulseServer)
elif [ -n "${PULSE_SERVER:-}" ] || [ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/pulse/native" ]; then
    AUDIO=(-audiodev pa,id=snd0)
fi
AUDIO+=(-device intel-hda -device hda-duplex,audiodev=snd0)

exec qemu-system-x86_64 -M q35,vmport=on -m 6G -smp 4 "${ACCEL[@]}" \
    -vga std -display gtk,zoom-to-fit=on \
    -netdev "$NET" -device e1000,netdev=n0 \
    -drive id=disk0,file="$DISK",format=raw,if=none -device ide-hd,drive=disk0,bus=ide.0 \
    "${AUDIO[@]}" \
    -serial file:"$ROOT/build/os-vm-serial.log" \
    -serial tcp:127.0.0.1:4555,server=on,wait=off \
    "${EXTRA[@]}" \
    -cdrom "$ISO"
