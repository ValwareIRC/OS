#!/usr/bin/env bash
# NVIDIA's own programs and libraries for its driver (nvidia-smi, libcuda, libnvidia-ml and the compilers CUDA loads)
# into the base system (BASE_DIR: its /usr is the system's), from NVIDIA's driver package of exactly the kernel part's
# version, unmodified, with NVIDIA's LICENSE (its 1.1(d) allows that for a system under an open source licence:
# docs/nvidia/plan.md). They are glibc programs, as the base system is. Graphics (GL, Vulkan, video) are not here:
# the display is still the firmware's framebuffer.
#   scripts/add-nvidia-userspace.sh EXTRACTED_DIR     (NVIDIA-Linux-x86_64-VERSION.run --extract-only --target DIR)
# Then `make` packs the base system again (the stamp is touched).
set -eu
SRC=${1:?the extracted NVIDIA package}
BASE_DIR=${BASE_DIR:-$HOME/osbuild/base}
VERSION=${NV_VERSION:-620.70.06}
LIB=$BASE_DIR/usr/lib/x86_64-linux-gnu
DOC=$BASE_DIR/usr/share/doc/nvidia-$VERSION
[ -d "$LIB" ] || { echo "no base system at $BASE_DIR"; exit 1; }
[ -f "$SRC/libcuda.so.$VERSION" ] || { echo "$SRC is not NVIDIA's $VERSION package"; exit 1; }

# library (as NVIDIA names it), then the names programs ask for
LIBS="libcuda.so.$VERSION:libcuda.so.1:libcuda.so
libnvidia-ml.so.$VERSION:libnvidia-ml.so.1:libnvidia-ml.so
libnvidia-ptxjitcompiler.so.$VERSION:libnvidia-ptxjitcompiler.so.1:libnvidia-ptxjitcompiler.so
libnvidia-nvvm.so.$VERSION:libnvidia-nvvm.so.4:libnvidia-nvvm.so
libnvidia-nvvm70.so.4
libnvidia-gpucomp.so.$VERSION
libnvidia-tileiras.so.$VERSION"
BINS="nvidia-smi nvidia-debugdump"

mkdir -p "$DOC"
: > "$DOC/FILES"
echo "$LIBS" | while IFS=: read -r file names; do
    install -m 0755 "$SRC/$file" "$LIB/$file"
    echo "/usr/lib/x86_64-linux-gnu/$file" >> "$DOC/FILES"
    for n in $(echo "${names:-}" | tr ':' ' '); do
        ln -sfn "$file" "$LIB/$n"
        echo "/usr/lib/x86_64-linux-gnu/$n -> $file" >> "$DOC/FILES"
    done
done
for b in $BINS; do
    install -m 0755 "$SRC/$b" "$BASE_DIR/usr/bin/$b"
    echo "/usr/bin/$b" >> "$DOC/FILES"
done
install -m 0644 "$SRC/LICENSE" "$DOC/LICENSE"
(cd "$SRC" && sha256sum $(echo "$LIBS" | cut -d: -f1) $BINS) > "$DOC/SHA256SUMS"
touch "$BASE_DIR/.stamp"
echo "NVIDIA $VERSION: $(wc -l < "$DOC/FILES") files into $BASE_DIR ($(du -ch $(sed 's/ ->.*//' "$DOC/FILES" | grep -v -- '->' | sed "s|^|$BASE_DIR|") 2>/dev/null | tail -1 | cut -f1))"
