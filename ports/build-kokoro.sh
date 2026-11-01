#!/usr/bin/env bash
# The companion's voice: Kokoro (Apache-2.0) through sherpa-onnx (Apache-2.0),
# on this computer, for English, Portuguese and Chinese.
#   - sherpa-onnx's C library, built from source at a fixed version
#   - the multi-language Kokoro model (kokoro-multi-lang-v1_0)
#   - os-speak (userland/tools/os-speak.c), which keeps it loaded and speaks lines
#   - a short clip of every English, Portuguese and Chinese voice saying the same words, for choosing by ear
# The voices themselves are not shipped: the one chosen is downloaded (about 0.5 MB, from Kokoro's
# published per-voice files), and this checks those files are the very same as the model's own.
# Output in build/voice/kokoro; scripts/build-base.sh voice packs it.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
PORTS_DIR=${PORTS_DIR:-$HOME/os-ports}
OUT=$ROOT/build/voice/kokoro
SHERPA_VERSION=${SHERPA_VERSION:-v1.13.8} # 2.x dropped espeak-ng: no Portuguese, and English only as far as its lexicon goes
MODEL=kokoro-multi-lang-v1_0
VOICES_URL=https://huggingface.co/onnx-community/Kokoro-82M-v1.0-ONNX/resolve/main/voices
mkdir -p "$PORTS_DIR" "$OUT/clips"
command -v cmake >/dev/null || { echo "need cmake"; exit 1; }

# 1. sherpa-onnx, its C library and the onnxruntime it brings
S=$PORTS_DIR/sherpa-onnx-$SHERPA_VERSION
if [ ! -d "$S" ]; then
    git clone --depth 1 -b "$SHERPA_VERSION" https://github.com/k2-fsa/sherpa-onnx "$S"
fi
if [ ! -f "$S/install/lib/libsherpa-onnx-c-api.so" ]; then
    cmake -S "$S" -B "$S/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON -DSHERPA_ONNX_ENABLE_C_API=ON \
        -DSHERPA_ONNX_ENABLE_PYTHON=OFF -DSHERPA_ONNX_ENABLE_TESTS=OFF -DSHERPA_ONNX_ENABLE_PORTAUDIO=OFF \
        -DSHERPA_ONNX_ENABLE_WEBSOCKET=OFF -DSHERPA_ONNX_ENABLE_BINARY=OFF -DSHERPA_ONNX_ENABLE_TTS=ON \
        -DCMAKE_INSTALL_PREFIX="$S/install" >/dev/null
    cmake --build "$S/build" -j"$(nproc)" >/dev/null
    cmake --install "$S/build" >/dev/null
fi
mkdir -p "$OUT/lib"
find "$OUT/lib" -name "*.so*" -delete
cp -a "$S"/install/lib/*.so* "$OUT/lib/"

# 2. the model
M=$PORTS_DIR/$MODEL
if [ ! -f "$M/model.onnx" ]; then
    curl -sSL -o "$PORTS_DIR/$MODEL.tar.bz2" "https://github.com/k2-fsa/sherpa-onnx/releases/download/tts-models/$MODEL.tar.bz2"
    (cd "$PORTS_DIR" && tar xjf "$MODEL.tar.bz2")
fi
mkdir -p "$OUT/model"
for f in model.onnx tokens.txt lexicon-us-en.txt lexicon-gb-en.txt lexicon-zh.txt date-zh.fst number-zh.fst phone-zh.fst LICENSE; do
    [ -e "$M/$f" ] && cp "$M/$f" "$OUT/model/"
done
find "$OUT/model/espeak-ng-data" "$OUT/model/dict" -delete 2>/dev/null || true
cp -r "$M/espeak-ng-data" "$M/dict" "$OUT/model/"

# 3. os-speak
gcc -O2 -I"$S/install/include" -o "$OUT/os-speak" "$ROOT/userland/tools/os-speak.c" \
    -L"$S/install/lib" -lsherpa-onnx-c-api -Wl,-rpath,'$ORIGIN/lib'

# 4. are the published voices the model's own? (af_heart is speaker 3)
VB=510*256*4
curl -sSL -o "$PORTS_DIR/af_heart.bin" "$VOICES_URL/af_heart.bin"
python3 - "$M/voices.bin" "$PORTS_DIR/af_heart.bin" <<'PY'
import sys
allv = open(sys.argv[1], "rb").read(); one = open(sys.argv[2], "rb").read()
size = 510 * 256 * 4
print("build-kokoro: voices.bin holds", len(allv) // size, "voices; af_heart.bin is", len(one), "bytes")
same = one == allv[3 * size:4 * size]
print("build-kokoro: the published af_heart is the model's own speaker 3:", same)
sys.exit(0 if same else 3)
PY

# 5. a clip of every English, Portuguese and Chinese voice (sid from the model's own table)
EN="Hi. This is how I would sound, if you chose me."
PT="Oi. É assim que eu soaria, se você me escolhesse."
ZH="你好。如果你选择我，我就会是这样的声音。"
i=0
for v in af_alloy af_aoede af_bella af_heart af_jessica af_kore af_nicole af_nova af_river af_sarah af_sky \
         am_adam am_echo am_eric am_fenrir am_liam am_michael am_onyx am_puck am_santa \
         bf_alice bf_emma bf_isabella bf_lily bm_daniel bm_fable bm_george bm_lewis; do
    case $v in b*) L=en ;; *) L=en-us ;; esac # espeak-ng: "en" is British English
    [ -s "$OUT/clips/$v.raw" ] || OS_SPEAK_LANG=$L LD_LIBRARY_PATH="$OUT/lib" "$OUT/os-speak" "$OUT/model" "$M/voices.bin:$i" 1.0 --say "$EN" "$OUT/clips/$v.raw"
    i=$((i + 1))
done
declare -A SID=([pf_dora]=42 [pm_alex]=43 [pm_santa]=44 [zf_xiaobei]=45 [zf_xiaoni]=46 [zf_xiaoxiao]=47 [zf_xiaoyi]=48
                [zm_yunjian]=49 [zm_yunxi]=50 [zm_yunxia]=51 [zm_yunyang]=52)
for v in "${!SID[@]}"; do
    case $v in p*) T=$PT L=pt-br ;; *) T=$ZH L=zh ;; esac
    [ -s "$OUT/clips/$v.raw" ] || OS_SPEAK_LANG=$L LD_LIBRARY_PATH="$OUT/lib" "$OUT/os-speak" "$OUT/model" "$M/voices.bin:${SID[$v]}" 1.0 --say "$T" "$OUT/clips/$v.raw"
done
LD_LIBRARY_PATH="$OUT/lib" "$OUT/os-speak" "$OUT/model" --rate "$PORTS_DIR/af_heart.bin" > "$OUT/clips/RATE"
ls "$OUT/clips" | wc -l
du -sh "$OUT"
