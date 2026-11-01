#!/usr/bin/env bash
# Builds the facet tool (tests/facet_cli.c) natively and runs it.
#   tests/facet.sh check FILE...
set -e
cd "$(dirname "$0")/.."
mkdir -p build/host
FX=userland/shell/fx
gcc -O1 -g -std=gnu17 -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
    -o build/host/facet tests/facet_cli.c $FX/parse.c $FX/check.c $FX/val.c $FX/eval.c userland/lib/json.c -lm -lpthread
build/host/facet "$@"
