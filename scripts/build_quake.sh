#!/usr/bin/env bash
# Build Quake (quakegeneric + examples/quake/main.c) with devkitA64 + libnx.
# Needs third_party/quakegeneric (git clone https://github.com/erysdren/quakegeneric)
# and examples/quake/romfs/id1/pak0.pak (the shareware data from quake106.zip).
# Output: examples/quake/build/quake.nso (plays until the viewer closes) and
# quake_test.nso (stops after 300 frames).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
qg="$root/third_party/quakegeneric/source"
src="$root/examples/quake"
out="$src/build"
mkdir -p "$out/obj"

dkp=${DEVKITPRO:-/c/devkitPro}
# The Windows installer sets DEVKITPRO=/opt/devkitpro, which only exists inside
# devkitPro's own MSYS2; from Git Bash fall back to the default install path.
[ -d "$dkp" ] || dkp=/c/devkitPro
case "$(uname -s)" in
MINGW* | MSYS*) dkp_w=$(cygpath -m "$dkp") ;;
*) dkp_w=$dkp ;;
esac
export DEVKITPRO="$dkp_w"

cc="$dkp/devkitA64/bin/aarch64-none-elf-gcc"
arch="-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE"
cflags="-g -O2 -std=gnu99 -ffunction-sections -fdata-sections $arch -D__SWITCH__ \
  -I$dkp_w/libnx/include -I$qg -Wno-unused-result -Wno-pointer-to-int-cast \
  -Wno-int-to-pointer-cast -fcommon"

# The engine sources from quakegeneric's CMakeLists.txt, minus the platform files.
sources="cd_null chase cl_demo cl_input cl_main cl_parse cl_tent cmd common console crc cvar \
  d_edge d_fill d_init d_modech d_part d_polyse d_scan d_sky d_sprite d_surf d_vars d_zpoint \
  draw host_cmd host in_null keys mathlib menu model net_loop net_main net_none net_vcr \
  nonintel pr_cmds pr_edict pr_exec r_aclip r_alias r_bsp r_draw r_edge r_efrag r_light \
  r_main r_misc r_part r_sky r_sprite r_surf r_vars sbar screen snd_null sv_main sv_move \
  sv_phys sv_user sys_null vid_null view wad world zone quakegeneric"

objs=""
for s in $sources; do
    "$cc" $cflags -c "$qg/$s.c" -o "$out/obj/$s.o"
    objs="$objs $out/obj/$s.o"
done

for variant in quake:0 quake_test:300; do
    name=${variant%%:*}
    limit=${variant##*:}
    "$cc" $cflags -DQUAKE_FRAME_LIMIT=$limit -c "$src/main.c" -o "$out/obj/main_$name.o"
    "$cc" -specs="$dkp_w/libnx/switch.specs" -g $arch -Wl,--gc-sections -L"$dkp_w/libnx/lib" \
        $objs "$out/obj/main_$name.o" -lnx -lm -o "$out/$name.elf"
    "$dkp/tools/bin/elf2nso" "$out/$name.elf" "$out/$name.nso"
    echo "built $out/$name.nso"
done
