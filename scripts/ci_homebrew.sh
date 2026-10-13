#!/usr/bin/env bash
# Build the homebrew the test suite runs (libnx examples, deko3d examples) and
# pack it into build/artifacts.tar.gz, on Linux with devkitPro, as CI does in
# the devkitpro/devkita64 container. Games (DOOM, Quake, OpenTyrian) need
# third-party sources and game data and are left out.
#
#   DEVKITPRO=/opt/devkitpro scripts/ci_homebrew.sh
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
for ex in hello console services romfs input sockets; do
    "$root/scripts/build_libnx.sh" "$ex"
done
for ex in deko3d deko3d_triangle deko3d_cube deko3d_depthblend deko3d_texture; do
    S2PC_EXTRA_LIBS=-ldeko3d "$root/scripts/build_libnx.sh" "$ex"
done
"$root/scripts/build_deko_examples.sh"
"$root/scripts/pack_artifacts.sh"
