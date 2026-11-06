#!/usr/bin/env bash
# NVIDIA's Linux glue (open-gpu-kernel-modules, kernel-open/nvidia) compiled against our kernel's Linux-shaped layer
# (kernel/linuxkpi) instead of Linux, file by file: what each still needs from the layer shows as compile errors.
#   tests/nvidia/glue_compile.sh [FILE.c ...]     (default: the main driver's files CUDA needs)
# Needs NVIDIA's source in ~/osbuild/nvidia-open (docs/nvidia/plan.md). Run inside WSL. Writes nothing but its
# objects and logs, in build/nvidia-glue/.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
NV=${NV_SRC:-$HOME/osbuild/nvidia-open}/kernel-open
OUT=$ROOT/build/nvidia-glue
mkdir -p "$OUT"
KPI=$ROOT/kernel/linuxkpi
FILES=${*:-nvidia/nv.c nvidia/nv-pci.c nvidia/nv-dma.c nvidia/nv-mmap.c nvidia/nv-vm.c nvidia/nv-vtophys.c
  nvidia/os-interface.c nvidia/os-mlock.c nvidia/os-pci.c nvidia/os-registry.c nvidia/os-usermap.c nvidia/nv-usermap.c
  nvidia/nv-acpi.c nvidia/nv-msi.c nvidia/nv-kthread-q.c nvidia/nv-memdbg.c nvidia/nv-report-err.c nvidia/nv-caps.c
  nvidia/nv-pci-table.c nvidia/nv-nano-timer.c nvidia/nv_uvm_interface.c nvidia/nv-rsync.c nvidia/nv-procfs.c
  nvidia/nv-i2c.c nvidia/nv-modeset-interface.c}
CFLAGS=(-std=gnu11 -O2 -ffreestanding -fno-stack-protector -fno-pic -fno-pie -m64 -mno-80387 -mno-mmx -mno-sse
  -mno-sse2 -mno-red-zone -mcmodel=kernel -nostdinc -isystem "$(gcc -print-file-name=include)"
  -I"$KPI/include" -I"$KPI" -I"$NV/common/inc" -I"$NV/nvidia" -I"$NV"
  -include "$KPI/include/linux/kconfig.h" -include "$KPI/include/linux/compiler_types.h"
  -D__KERNEL__ -DMODULE -DNVRM -DNV_KERNEL_INTERFACE_LAYER -DNV_UVM_ENABLE -DNDEBUG -DNV_SPECTRE_V2=0
  -DNV_FILESYSTEM_ACCESS_AVAILABLE=1 -DNVIDIA_UNDEF_LEGACY_BIT_MACROS -DNV_VERSION_STRING=\"620.70.06\"
  -DKBUILD_MODNAME=\"nvidia\" -D__KBUILD_MODNAME=nvidia -DLIBSPDM_CONFIG=\"nvspdm_rmconfig.h\"
  -Wno-unused-function -Wno-unused-variable -fmax-errors=0)
total=0 clean=0
for f in $FILES; do
    name=$(basename "$f" .c)
    if gcc "${CFLAGS[@]}" -c "$NV/$f" -o "$OUT/$name.o" > "$OUT/$name.log" 2>&1; then
        clean=$((clean + 1)); n=0
    else
        n=$(grep -c " error: " "$OUT/$name.log")
    fi
    total=$((total + n))
    printf '%-26s %5d errors\n' "$name" "$n"
done
echo "$clean of $(echo $FILES | wc -w) files compile; $total errors in all"
# the names most asked for, across every file: what to write next
cat "$OUT"/*.log | grep -oE "error: (implicit declaration of function|unknown type name|'[A-Za-z_0-9]+' undeclared)[^;]*" \
  | grep -oE "'[A-Za-z_0-9]+'" | sort | uniq -c | sort -rn | head -40 > "$OUT/wanted.txt"
echo "most wanted (build/nvidia-glue/wanted.txt):"; head -15 "$OUT/wanted.txt"
