#!/usr/bin/env bash
# The Daybook showcase in several designs: build/fxshots/daybook-<design>.png
#   tests/facet2/showcase.sh [design ...]
cd "$(dirname "$0")/../.."
bash tests/facet.sh check tests/facet2/daybook.facet || exit 1
mkdir -p build/fxshots
first=1
for t in ${@:-starlit neon frost paper aqua luna}; do
    if [ $first = 1 ]; then
        FX_W=1100 FX_H=780 bash tests/fx_shot.sh tests/facet2/daybook.facet userland/rootfs/usr/share/os/themes/$t.xml build/fxshots/daybook-$t.png 2>&1 | grep -v 'note:\|warning\|^ \|^In file\|^tests/fx_shot.c'
        first=0
    else
        FX_W=1100 FX_H=780 OS_THEMES=$PWD/userland/rootfs/usr/share/os/themes build/host/fx_shot tests/facet2/daybook.facet userland/rootfs/usr/share/os/themes/$t.xml build/fxshots/daybook-$t.png
    fi
done
