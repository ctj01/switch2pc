#!/usr/bin/env bash
# Build OpenTyrian (an unmodified SDL2 game) against the switch2pc SDL2.
# Needs third_party/opentyrian (git clone https://github.com/opentyrian/opentyrian)
# and examples/tyrian/romfs/data/ with the freeware Tyrian 2.1 data files
# (camanis.net/tyrian/tyrian21.zip). Output: examples/tyrian/build/tyrian.nso
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
ot="$root/third_party/opentyrian"
out="$root/examples/tyrian/build"
sdl="$root/third_party/sdl2-install"
mkdir -p "$out/obj"

dkp=${DEVKITPRO:-/c/devkitPro}
case "$(uname -s)" in
MINGW* | MSYS*) dkp_w=$(cygpath -m "$dkp") ;;
*) dkp_w=$dkp ;;
esac
export DEVKITPRO="$dkp_w"

cc="$dkp/devkitA64/bin/aarch64-none-elf-gcc"
arch="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
[ -f "$ot/src/opentyrian_version.h" ] || : > "$ot/src/opentyrian_version.h"
cflags="-g -O2 -std=gnu99 -ffunction-sections -fdata-sections $arch -D__SWITCH__ \
  -DTARGET_UNIX -DTYRIAN_DIR=\"romfs:/data\" -DNDEBUG \
  -Wno-missing-field-initializers -Wno-format-truncation \
  -I$dkp_w/libnx/include -I$sdl/include/SDL2 -I$ot/src"

objs=""
"$cc" $cflags -c "$root/ports/sdl2/s2pc_prelude.c" -o "$out/obj/s2pc_prelude.o"
objs="$out/obj/s2pc_prelude.o"
for c in "$ot"/src/*.c; do
    o="$out/obj/$(basename "${c%.c}").o"
    "$cc" $cflags -c "$c" -o "$o"
    objs="$objs $o"
done
"$cc" -specs="$dkp_w/libnx/switch.specs" -g $arch -Wl,--gc-sections \
    -L"$sdl/lib" -L"$dkp_w/libnx/lib" $objs -lSDL2 -lnx -lm -o "$out/tyrian.elf"
"$dkp/tools/bin/elf2nso" "$out/tyrian.elf" "$out/tyrian.nso"
echo "built $out/tyrian.nso"
