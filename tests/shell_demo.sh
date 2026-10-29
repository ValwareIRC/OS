#!/usr/bin/env bash
# Boots the OS in interactive mode and types a few commands into the serial
# console, printing the transcript. Handy smoke test for the shell path.
cd "$(dirname "$0")/.."
{
    sleep 8
    printf 'uname -a\r'; sleep 1
    printf 'cd /tmp && echo made on the OS > note.txt && cat note.txt\r'; sleep 1
    printf 'ls -la /tmp; free -m | head -2\r'; sleep 1
    printf 'for i in 1 2 3; do echo loop $i; done; echo $((2**10))\r'; sleep 2
} | timeout 16 qemu-system-x86_64 -M q35 -m 2G -smp 4 -cpu max -display none \
        -serial stdio -cdrom build/os.iso 2>/dev/null |
    tr -d '\r' | sed -n '/user space is alive/,$p' | head -40
