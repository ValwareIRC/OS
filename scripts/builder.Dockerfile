FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential musl-tools busybox-static xorriso cpio mtools git curl ca-certificates \
    && rm -rf /var/lib/apt/lists/*
