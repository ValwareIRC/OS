#!/usr/bin/env bash
# Development bridge (WSL only): makes the Windows host's Ollama answer on
# WSL's 127.0.0.1:11434, which a QEMU guest with user networking reaches as
# 10.0.2.2:11434 (the OS's default "On your network" address). Each
# connection is relayed by tests/host-relay.js running on Windows, because
# WSL's NAT networking cannot reach the Windows loopback directly.
#
# Starts the bridge in the background if it isn't running; prints its pid.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
NODE="/mnt/c/Program Files/nodejs/node.exe"
if ! [ -x "$NODE" ] || ! command -v socat >/dev/null; then
    echo "bridge: needs node.exe on Windows and socat in WSL" >&2
    exit 1
fi
if (exec 3<>/dev/tcp/127.0.0.1/11434) 2>/dev/null; then
    echo "bridge: 127.0.0.1:11434 already answers" >&2
    exit 0
fi
# Only when an Ollama really listens on Windows: WSL forwards Windows'
# localhost ports to its own, so with none there the relay connects back to
# itself, each connection spawning another, until WSL stops answering.
owner=$(powershell.exe -NoProfile -Command \
    "(Get-NetTCPConnection -LocalPort 11434 -State Listen -ErrorAction SilentlyContinue | ForEach-Object { (Get-Process -Id \$_.OwningProcess).Name }) -join ','" 2>/dev/null | tr -d '\r')
case ",$owner," in
    *,ollama*) ;;
    *) echo "bridge: no Ollama listening on Windows (port 11434: ${owner:-nothing}); not starting" >&2
       exit 1 ;;
esac
# setsid -f: detach fully, so the bridge outlives the shell that started it
# (a plain background job dies when a `wsl -- bash -c` session ends).
setsid -f socat TCP-LISTEN:11434,bind=127.0.0.1,reuseaddr,fork \
    EXEC:"$ROOT/tests/host-relay.sh 11434" >/dev/null 2>&1 </dev/null
sleep 0.5
pgrep -f "socat TCP-LISTEN:11434" | head -1
