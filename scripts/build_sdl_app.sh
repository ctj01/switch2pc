#!/usr/bin/env bash
# Build examples/<name>/main.c against the switch2pc SDL2 (scripts/build_sdl2.sh).
# Works from Git Bash on Windows (devkitPro at C:\devkitPro) or Linux with DEVKITPRO.
#
#   scripts/build_sdl_app.sh <example dir name> [extra cflags...]
set -euo pipefail

name=${1:?example name}
shift || true
root=$(cd "$(dirname "$0")/.." && pwd)
src="$root/examples/$name"
out="$src/build"
sdl="$root/third_party/sdl2-install"
mkdir -p "$out"

dkp=${DEVKITPRO:-/c/devkitPro}
case "$(uname -s)" in
MINGW* | MSYS*) dkp_w=$(cygpath -m "$dkp") ;;
*) dkp_w=$dkp ;;
esac
export DEVKITPRO="$dkp_w"

cc="$dkp/devkitA64/bin/aarch64-none-elf-gcc"
arch="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
cflags="-g -O2 -ffunction-sections -fdata-sections $arch -D__SWITCH__ \
  -I$dkp_w/libnx/include -I$sdl/include/SDL2 $*"

objs=""
for c in "$src"/*.c; do
    o="$out/$(basename "${c%.c}").o"
    "$cc" $cflags -c "$c" -o "$o"
    objs="$objs $o"
done
"$cc" -specs="$dkp_w/libnx/switch.specs" -g $arch -Wl,--gc-sections \
    -L"$sdl/lib" -L"$dkp_w/libnx/lib" $objs -lSDL2 -lnx -lm -o "$out/$name.elf"
"$dkp/tools/bin/elf2nso" "$out/$name.elf" "$out/$name.nso"
echo "built $out/$name.nso"
