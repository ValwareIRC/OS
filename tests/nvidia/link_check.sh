#!/usr/bin/env bash
# What NVIDIA's driver (its glue, compiled by glue_compile.sh, and the prebuilt core nv-kernel.o) together with the
# Linux-shaped layer (kernel/linuxkpi/src, host) still lacks: the symbols nobody defines, our kernel's own aside.
#   tests/nvidia/link_check.sh        (run inside WSL, after glue_compile.sh)
# Writes build/nvidia-glue/link/: the layer's objects, undefined.txt.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
K=$ROOT/kernel
OUT=$ROOT/build/nvidia-glue/link
CORE=${NV_SRC:-$HOME/osbuild/nvidia-open}/src/nvidia/_out/Linux_x86_64/nv-kernel.o
mkdir -p "$OUT"
COMMON=(-std=gnu17 -O2 -Wall -Wextra -Werror -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 -mno-80387
  -mno-mmx -mno-sse -mno-sse2 -mno-red-zone -mcmodel=kernel)
LKPI=("${COMMON[@]}" -Wno-unused-parameter -nostdinc -isystem "$(gcc -print-file-name=include)" -I"$K/linuxkpi/include"
  -I"$K/linuxkpi" -include "$K/linuxkpi/include/linux/kconfig.h" -include "$K/linuxkpi/include/linux/compiler_types.h"
  -D__KERNEL__)
HOST=("${COMMON[@]}" -I"$K/include" -I"$K/arch/x86_64/include" -I"$K/third_party")
fail=0
for f in "$K"/linuxkpi/src/*.c; do
    gcc "${LKPI[@]}" -c "$f" -o "$OUT/src-$(basename "$f" .c).o" || fail=1
done
for f in "$K"/linuxkpi/host/*.c; do
    gcc "${HOST[@]}" -c "$f" -o "$OUT/host-$(basename "$f" .c).o" || fail=1
done
[ $fail = 0 ] || { echo "the layer does not compile"; exit 1; }
ld -r -o "$OUT/all.o" "$OUT"/src-*.o "$OUT"/host-*.o "$ROOT"/build/nvidia-glue/*.o "$CORE"
# our kernel's own symbols (whatever its built objects define)
find "$ROOT/build/kernel" -name '*.o' -not -path '*/linuxkpi/*' -print0 2>/dev/null | xargs -0 -r nm -g --defined-only 2>/dev/null \
  | awk '{print $3}' | sort -u > "$OUT/kernel-defines.txt"
nm -u "$OUT/all.o" | awk '{print $2}' | sort -u > "$OUT/undefined-all.txt"
comm -23 "$OUT/undefined-all.txt" "$OUT/kernel-defines.txt" > "$OUT/undefined.txt"
# defined twice: the layer and our kernel both (would not link)
nm -g --defined-only "$OUT/all.o" | awk '{print $3}' | sort -u | comm -12 - "$OUT/kernel-defines.txt" > "$OUT/clashes.txt"
echo "undefined: $(wc -l < "$OUT/undefined.txt") (build/nvidia-glue/link/undefined.txt)"
echo "defined by both the layer and our kernel: $(wc -l < "$OUT/clashes.txt") (clashes.txt)"
