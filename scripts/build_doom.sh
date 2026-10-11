#!/usr/bin/env bash
# Build DOOM (doomgeneric + examples/doom/main.c) with devkitA64 + libnx.
# Needs third_party/doomgeneric (git clone https://github.com/ozkl/doomgeneric)
# and examples/doom/romfs/doom1.wad (the shareware IWAD; Debian ships it as
# doom-wad-shareware). Works from Git Bash on Windows or from Linux with
# DEVKITPRO set. Output: examples/doom/build/doom.nso
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
dg="$root/third_party/doomgeneric/doomgeneric"
src="$root/examples/doom"
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
cflags="-g -O2 -ffunction-sections -fdata-sections $arch -D__SWITCH__ -DNORMALUNIX -DLINUX \
  -D_DEFAULT_SOURCE -DDOOMGENERIC_RESX=640 -DDOOMGENERIC_RESY=400 \
  -I$dkp_w/libnx/include -I$dg -Wno-unused-result"

# The engine sources from doomgeneric's Makefile, minus the X11 platform file.
sources="dummy am_map doomdef doomstat dstrings d_event d_items d_iwad d_loop d_main d_mode d_net \
  f_finale f_wipe g_game hu_lib hu_stuff info i_cdmus i_endoom i_joystick i_scale i_sound \
  i_system i_timer memio m_argv m_bbox m_cheat m_config m_controls m_fixed m_menu m_misc \
  m_random p_ceilng p_doors p_enemy p_floor p_inter p_lights p_map p_maputl p_mobj p_plats \
  p_pspr p_saveg p_setup p_sight p_spec p_switch p_telept p_tick p_user r_bsp r_data r_draw \
  r_main r_plane r_segs r_sky r_things sha1 sounds statdump st_lib st_stuff s_sound tables \
  v_video wi_stuff w_checksum w_file w_main w_wad z_zone w_file_stdc i_input i_video doomgeneric"

objs=""
for s in $sources; do
    "$cc" $cflags -c "$dg/$s.c" -o "$out/obj/$s.o"
    objs="$objs $out/obj/$s.o"
done
# Two builds of the platform layer: doom.nso runs until the viewer closes,
# doom_test.nso stops after 400 frames for the headless smoke test.
for variant in doom:0 doom_test:400; do
    name=${variant%%:*}
    limit=${variant##*:}
    "$cc" $cflags -DDOOM_FRAME_LIMIT=$limit -c "$src/main.c" -o "$out/obj/main_$name.o"
    "$cc" -specs="$dkp_w/libnx/switch.specs" -g $arch -Wl,--gc-sections -L"$dkp_w/libnx/lib" \
        $objs "$out/obj/main_$name.o" -lnx -lm -o "$out/$name.elf"
    "$dkp/tools/bin/elf2nso" "$out/$name.elf" "$out/$name.nso"
    echo "built $out/$name.nso"
done
