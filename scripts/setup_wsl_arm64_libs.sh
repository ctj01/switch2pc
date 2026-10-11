#!/bin/bash
# Optional ARM64 libraries for the cross build on an x86-64 Ubuntu 24.04 host
# (WSL2): libX11 for the window (#25), PulseAudio and ALSA for audio (#27).
# Ubuntu serves arm64 packages from ports.ubuntu.com, not archive.ubuntu.com,
# so the existing sources are pinned to amd64 and a ports source is added.
# Run with sudo. Reconfigure build/aarch64 afterwards (rm -rf build/aarch64).
set -euo pipefail

f=/etc/apt/sources.list.d/ubuntu.sources
dpkg --print-foreign-architectures | grep -qx arm64 || dpkg --add-architecture arm64
if ! grep -q '^Architectures:' "$f"; then
    cp -n "$f" /etc/apt/ubuntu.sources.bak-switch2pc || true
    sed -i 's|^Types: deb$|Types: deb\nArchitectures: amd64|' "$f"
fi
cat > /etc/apt/sources.list.d/ubuntu-ports-arm64.sources <<'S'
Types: deb
URIs: http://ports.ubuntu.com/ubuntu-ports/
Suites: noble noble-updates noble-security
Components: main universe restricted multiverse
Architectures: arm64
Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg
S
apt-get update
apt-get install -y libx11-dev:arm64 libpulse-dev:arm64 libasound2-dev:arm64
