#!/usr/bin/env bash
# Boots the OS interactively with the host-model relay and runs network/AI
# commands in its shell, printing the transcript.
cd "$(dirname "$0")/.."
bash scripts/host-bridge.sh >/dev/null 2>&1
{
    sleep 8
    printf 'os-ai get https://api.anthropic.com/v1/models\r'; sleep 6
    printf 'os-ai get https://www.wikipedia.org/ | head -3\r'; sleep 6
    printf 'os-ai models ollama - - | tail -4\r'; sleep 5
    printf 'os-ai chat ollama - - qwen3:8b "Name one colour of the sky at dusk."\r'; sleep ${CHAT_WAIT:-60}
} | timeout $((30 + ${CHAT_WAIT:-60})) qemu-system-x86_64 -M q35 -m 2G -smp 4 -cpu max -display none \
        -netdev user,id=n0 -device e1000,netdev=n0 \
        -serial stdio -cdrom build/os.iso 2>/dev/null |
    tr -d '\r' | sed -n '/development shell/,$p'
