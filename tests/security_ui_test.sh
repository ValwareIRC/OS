#!/usr/bin/env bash
# h) and i) on the host (tests/security_ui_test.c)
cd "$(dirname "$0")/.." || exit 2
mkdir -p build/host
gcc -O1 -g -Wall -o build/host/security_ui_test tests/security_ui_test.c userland/shell/security.c userland/shell/fx/caps.c \
    -Iuserland/shell || exit 2
build/host/security_ui_test
