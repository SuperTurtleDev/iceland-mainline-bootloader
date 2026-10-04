# Bootloader reproducible-build image (podman).
#
# Base is pinned by digest so the toolchain is fully captured by the image id
# recorded in build/buildinfo.txt.  Rebuild the image with:
#   podman build -t localhost/mainline-bootloader-builder:v1 .
# (scripts/build-bootloader.sh does this automatically when the tag is missing
#  or REBUILD_IMAGE=1 is set; the EFI build itself never needs the network.)
FROM docker.io/library/ubuntu:26.04@sha256:88a381d5b5eeb2b35d3ad70925a362c37ce569daf43ede89ff818ec20e4d3794

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=UTC \
    LC_ALL=C

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        bison \
        flex \
        uuid-dev \
        python3 \
        python-is-python3 \
        clang \
        lld \
        llvm \
        git \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /work
