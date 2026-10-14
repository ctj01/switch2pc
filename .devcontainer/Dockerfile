# Everything switch2pc needs in one image: devkitPro (devkitA64, libnx, deko3d)
# to build homebrew, and the ARM64 cross toolchain with qemu-user to build and
# run switch2pc on an x86-64 machine. Used by GitHub Codespaces and VS Code's
# Dev Containers; see docs/first-contribution.md.
FROM devkitpro/devkita64:latest

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        gcc-aarch64-linux-gnu libc6-dev-arm64-cross binutils-aarch64-linux-gnu \
        qemu-user qemu-user-static cmake ninja-build gdb-multiarch lld \
        clang libclang-rt-18-dev \
        fonts-dejavu-core python3 python3-pip git \
    && rm -rf /var/lib/apt/lists/*

# The same clang-format as CI (Ubuntu 24.04's 18.x): versions format differently.
RUN pip3 install --break-system-packages --no-cache-dir clang-format==18.1.3
