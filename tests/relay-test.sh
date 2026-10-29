#!/usr/bin/env bash
# Checks each hop between a the OS guest and the host's Ollama:
#   node relay (WSL -> Windows loopback), the socat bridge on 127.0.0.1:11434.
cd "$(dirname "$0")/.."
echo "node: $('/mnt/c/Program Files/nodejs/node.exe' --version 2>&1)"
printf 'GET /api/version HTTP/1.0\r\nHost: x\r\n\r\n' | timeout 15 bash tests/host-relay.sh 11434 > /tmp/relay-out.txt
echo "relay exit=${PIPESTATUS[1]} bytes=$(wc -c < /tmp/relay-out.txt)"
head -c 200 /tmp/relay-out.txt | tr -d '\r'; echo
bash scripts/host-bridge.sh >/dev/null
echo "bridge pid: $(pgrep -f 'socat TCP-LISTEN:11434' | head -1)"
echo "through bridge: $(curl -s -m 20 http://127.0.0.1:11434/api/version)"
