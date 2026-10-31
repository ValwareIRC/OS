#!/usr/bin/env bash
# Builds the OS's own offline mind: llama.cpp's llama-server (MIT), an
# OpenAI-compatible HTTP server, plus a small open-weights model. Output in
# build/mind; scripts/build-sysroot.sh packs it.
#
#   MIND_MODEL=qwen3-1.7b|qwen3-4b ports/build-mind.sh
set -euo pipefail
# The CPU features llama.cpp and whisper.cpp may count on: AVX2 and FMA on x86-64 (any PC of the last
# decade, and the VM's CPU model); on arm64 the dot-product instructions (every Apple Silicon Mac)
case "$(uname -m)" in
    aarch64|arm64) GGML_CPU="-DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16" ;;
    *) GGML_CPU="-DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON" ;;
esac
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORTS_DIR=${PORTS_DIR:-$HOME/os-ports}
OUT=$ROOT/build/mind
mkdir -p "$PORTS_DIR" "$OUT"

L=$PORTS_DIR/llama.cpp
[ -d "$L" ] || git clone --depth 1 https://github.com/ggml-org/llama.cpp "$L"
if [ ! -x "$OUT/llama-server" ]; then
    cmake -S "$L" -B "$L/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF \
        $GGML_CPU -DGGML_OPENMP=OFF -DLLAMA_CURL=OFF \
        -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=ON >/dev/null
    cmake --build "$L/build" -j"$(nproc)" --target llama-server >/dev/null
    cp "$L/build/bin/llama-server" "$OUT/"
fi

# Qwen3 (Apache-2.0). 1.7B fits comfortably in memory; 4B thinks better.
MODEL=${MIND_MODEL:-qwen3-1.7b}
case "$MODEL" in
qwen3-1.7b) URL=https://huggingface.co/Qwen/Qwen3-1.7B-GGUF/resolve/main/Qwen3-1.7B-Q8_0.gguf ;;
qwen3-4b) URL=https://huggingface.co/Qwen/Qwen3-4B-GGUF/resolve/main/Qwen3-4B-Q4_K_M.gguf ;;
*) echo "unknown model $MODEL"; exit 1 ;;
esac
FILE=$OUT/$(basename "$URL")
[ -f "$FILE" ] || curl -sSL -o "$FILE" "$URL"
ls -la "$OUT"
