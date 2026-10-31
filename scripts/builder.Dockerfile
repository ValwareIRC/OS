FROM ubuntu:24.04
# The OS's own build (kernel, userland, image), and the base system: Flatpak and its builder bring the
# Freedesktop SDK the base is built in (scripts/build-base.sh), for whichever architecture this runs as.
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential musl-tools busybox-static xorriso cpio mtools git curl ca-certificates \
        flatpak flatpak-builder meson ninja-build cmake rsync python3 xz-utils patch pkg-config \
    && rm -rf /var/lib/apt/lists/*
