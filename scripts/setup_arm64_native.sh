#!/bin/bash
# Setup for a native ARM64 Linux machine (Ubuntu 24.04 arm64), the project's
# real target: no cross toolchain or qemu, scripts/build.sh builds and tests
# natively. Optional libraries give the X11 window and sound; DejaVu is the
# TrueType font served as the Switch's shared font. devkitPro does
# not run here: build the homebrew on the Windows machine and bring it over
# with scripts/pack_artifacts.sh.
#
#   sudo scripts/setup_arm64_native.sh
set -e
if [ "$(uname -m)" != aarch64 ]; then
    echo "this is for ARM64 hosts; x86-64 uses scripts/cloud_setup.sh" >&2
    exit 1
fi
apt-get update
apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build clang-format lld gdb git python3 \
    libx11-dev libpulse-dev libasound2-dev fonts-dejavu-core
