#!/usr/bin/env bash
# Builds the on-device voice pieces for the OS (glibc, from this Ubuntu):
# whisper.cpp (MIT) for speech recognition, with a small English model.
# Output goes to build/voice; scripts/build-sysroot.sh packs it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORTS_DIR=${PORTS_DIR:-$HOME/os-ports}
OUT=$ROOT/build/voice
mkdir -p "$PORTS_DIR" "$OUT"
command -v cmake >/dev/null || { echo "need cmake (apt install cmake)"; exit 1; }

W=$PORTS_DIR/whisper.cpp
if [ ! -d "$W" ]; then
    git clone --depth 1 https://github.com/ggml-org/whisper.cpp "$W"
fi
if [ ! -x "$OUT/whisper-cli" ]; then
    cmake -S "$W" -B "$W/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGGML_NATIVE=OFF \
        -DGGML_AVX=ON -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON -DWHISPER_BUILD_TESTS=OFF \
        -DWHISPER_BUILD_EXAMPLES=ON -DGGML_OPENMP=OFF >/dev/null
    cmake --build "$W/build" -j"$(nproc)" --target whisper-cli >/dev/null
    cp "$W/build/bin/whisper-cli" "$OUT/"
fi
# base.en: 142 MB, good accuracy for a companion; tiny.en (75 MB) is the fallback.
MODEL=${WHISPER_MODEL:-base.en}
if [ ! -f "$OUT/ggml-$MODEL.bin" ]; then
    curl -sSL -o "$OUT/ggml-$MODEL.bin" "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-$MODEL.bin"
fi
ls -la "$OUT"

# Piper (MIT; its phonemizer espeak-ng is GPL-3: revisit before release) for
# the companion's voice, as the prebuilt Linux release with its own libraries.
P=$PORTS_DIR/piper
if [ ! -x "$P/piper" ]; then
    curl -sSL -o "$PORTS_DIR/piper.tar.gz" \
        https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_linux_x86_64.tar.gz
    (cd "$PORTS_DIR" && tar xzf piper.tar.gz)
fi
VOICE=${PIPER_VOICE:-en_US-amy-medium}
if [ ! -f "$OUT/$VOICE.onnx" ]; then
    base=https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/${VOICE#en_US-}
    name=${VOICE#en_US-}; speaker=${name%-*}; quality=${name##*-}
    base=https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/$speaker/$quality
    curl -sSL -o "$OUT/$VOICE.onnx" "$base/$VOICE.onnx"
    curl -sSL -o "$OUT/$VOICE.onnx.json" "$base/$VOICE.onnx.json"
fi
rm -rf "$OUT/piper" && cp -r "$P" "$OUT/piper"
ls -la "$OUT" "$OUT/piper" | head -30
