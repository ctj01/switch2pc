#!/usr/bin/env bash
# Build examples/libnx_<name> with devkitA64 + libnx. Works from Git Bash on
# Windows (default devkitPro at C:\devkitPro) or from Linux with DEVKITPRO set.
# Produces examples/libnx_<name>/build/<name>.elf and <name>.nso (devkitPro's
# elf2nso). Usage: scripts/build_libnx.sh [hello|console|...]
# Extra libraries before -lnx: S2PC_EXTRA_LIBS="-ldeko3d" scripts/build_libnx.sh deko3d
set -euo pipefail

name=${1:-hello}
root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/examples/libnx_$name"
out="$src/build"
if [ ! -f "$src/main.c" ]; then
    echo "no such example: $src" >&2
    exit 1
fi
mkdir -p "$out"

dkp=${DEVKITPRO:-/c/devkitPro}
# The Windows installer sets DEVKITPRO=/opt/devkitpro, which only exists inside
# devkitPro's own MSYS2; from Git Bash fall back to the default install path.
[ -d "$dkp" ] || dkp=/c/devkitPro
case "$(uname -s)" in
MINGW* | MSYS*) dkp_w=$(cygpath -m "$dkp") ;;
*) dkp_w=$dkp ;;
esac
export DEVKITPRO="$dkp_w"

# GLSL shaders next to main.c (name_vsh.glsl, name_fsh.glsl, ...) are compiled with
# uam into romfs/shaders/name.dksh, and their raw Maxwell code into build/ for
# disassembly (docs/gpu.md).
for g in "$src"/*.glsl; do
    [ -f "$g" ] || continue
    base=$(basename "${g%.glsl}")
    case "$base" in
    *_vsh) stage=vert ;; *_fsh) stage=frag ;; *_gsh) stage=geom ;;
    *_csh) stage=comp ;; *_tcsh) stage=tess_ctrl ;; *_tesh) stage=tess_eval ;;
    *) echo "unknown shader stage for $g" >&2; exit 1 ;;
    esac
    mkdir -p "$src/romfs/shaders"
    "$dkp/tools/bin/uam" -s "$stage" -o "$src/romfs/shaders/$base.dksh" -r "$out/$base.bin" "$g"
done

cc="$dkp/devkitA64/bin/aarch64-none-elf-gcc"
arch="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"

"$cc" -g -Wall -O2 -ffunction-sections $arch -D__SWITCH__ -I"$dkp_w/libnx/include" \
    -c "$src/main.c" -o "$out/main.o"
"$cc" -specs="$dkp_w/libnx/switch.specs" -g $arch -L"$dkp_w/libnx/lib" \
    "$out/main.o" ${S2PC_EXTRA_LIBS:-} -lnx -lm -o "$out/$name.elf"
"$dkp/tools/bin/elf2nso" "$out/$name.elf" "$out/$name.nso"
echo "built $out/$name.elf and $out/$name.nso"
