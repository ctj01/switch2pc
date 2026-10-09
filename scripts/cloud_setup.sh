#!/bin/bash
# Setup script for an Ubuntu 24.04 x86-64 cloud or CI machine:
# the cross toolchain and qemu-user that scripts/build.sh needs. devkitPro is
# not installed, so libnx/SDL examples and game smoke tests are skipped there.
set -e
apt-get update
apt-get install -y --no-install-recommends \
    gcc-aarch64-linux-gnu libc6-dev-arm64-cross binutils-aarch64-linux-gnu \
    qemu-user qemu-user-static cmake ninja-build clang-format gdb-multiarch
