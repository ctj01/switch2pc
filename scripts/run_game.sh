#!/usr/bin/env bash
# Play a game under hosrun. On Linux it runs hosrun directly: natively on ARM64
# (build/host from scripts/build.sh), through qemu-user elsewhere. From Windows
# (Git Bash or devkitPro's MSYS2) it runs in WSL/qemu and, by default, opens
# its own X11 window through WSLg (HOS_WINDOW=1; needs a cross build with
# libx11-dev:arm64). With --viewer (Windows only) it instead renders into a
# shared BMP that tools/viewer.ps1 shows, feeding the keyboard back through a
# file. Build first: scripts/build.sh and the game's build script.
#
#   scripts/run_game.sh <doom|quake|tyrian> [scale] [--viewer]
#   scripts/run_game.sh deko <1-9> [scale] [--viewer]   (scripts/build_deko_examples.sh)
#
# Keys: arrows = D-pad, Z/Ctrl = A, X/Space = B, C/V = X/Y, Q/E = L/R,
# Enter = Plus, Esc = Minus (window mode also: WASD = left stick).
# Close the window to quit; the game gets the applet exit request.
set -euo pipefail

game=${1:-doom}
shift || true
example=
if [ "$game" = deko ]; then
    example=${1:-1}
    shift || true
fi
scale=2
mode=window
for arg in "$@"; do
    case "$arg" in
    --viewer) mode=viewer ;;
    *) scale=$arg ;;
    esac
done
[ -n "$example" ] && [ $# -eq 0 ] && scale=1 # deko examples render at 1280x720
root=$(cd "$(dirname "$0")/.." && pwd)

case "$game" in
doom) nso="examples/doom/build/doom.nso"; romfs="examples/doom/romfs" ;;
quake) nso="examples/quake/build/quake.nso"; romfs="examples/quake/romfs" ;;
tyrian) nso="examples/tyrian/build/tyrian.nso"; romfs="examples/tyrian/romfs" ;;
deko) nso="build/deko_examples/deko_examples.nso"; romfs="build/deko_examples/romfs" ;;
*)
    echo "unknown game: $game (doom, quake, tyrian, deko)" >&2
    exit 2
    ;;
esac
if [ ! -f "$root/$nso" ]; then
    echo "$nso is missing; run its build script first (build_opentyrian.sh for tyrian," \
        "build_deko_examples.sh for deko)" >&2
    exit 1
fi

sdmc="build/tmp/sdmc"
if [ -n "$example" ]; then
    sdmc="build/tmp/sdmc_deko"
    mkdir -p "$root/$sdmc"
    echo "$example" > "$root/$sdmc/deko_example.txt"
fi
mkdir -p "$root/$sdmc"

if [ "$(uname -s)" = Linux ]; then
    cd "$root"
    if [ "$(uname -m)" = aarch64 ]; then
        run=(build/host/src/hosrun)
    else
        run=(qemu-aarch64 -L /usr/aarch64-linux-gnu build/aarch64/src/hosrun)
    fi
    [ -n "${DISPLAY:-}" ] || echo "no DISPLAY: no window (ssh -X, or HOS_FRAMES=<dir> for dumps)" >&2
    HOS_WINDOW=1 HOS_WINDOW_SCALE="$scale" HOS_AUDIO=auto HOS_LOG_LEVEL=${HOS_LOG_LEVEL:-info} \
        "${run[@]}" --romfs "$romfs" --sdmc "$sdmc" "$nso" || true
    exit 0
fi

root_win=$(cygpath -w "$root")                               # C:\Users\...
# From the Windows path, so it works in Git Bash (/c/...) and in devkitPro's
# MSYS2 shell, where the same folder is /home/<user>/...
root_mixed=$(cygpath -m "$root")                             # C:/Users/...
drive=$(printf '%s' "${root_mixed%%:*}" | tr 'A-Z' 'a-z')
root_wsl="/mnt/$drive${root_mixed#?:}"                       # /mnt/c/Users/...

if [ "$mode" = window ]; then
    wsl.exe -e bash -c "cd '$root_wsl' && \
        HOS_WINDOW=1 HOS_WINDOW_SCALE='$scale' HOS_AUDIO=auto HOS_LOG_LEVEL=info \
        qemu-aarch64 -L /usr/aarch64-linux-gnu build/aarch64/src/hosrun \
            --romfs '$romfs' --sdmc '$sdmc' '$nso'" || true
    exit 0
fi

rm -f "$root/build/tmp/live.bmp"
printf '0 0 0 0 0' > "$root/build/tmp/pad.txt"

powershell.exe -ExecutionPolicy Bypass -File "$root_win\\tools\\viewer.ps1" \
    "$root_win\\build\\tmp\\live.bmp" "$root_win\\build\\tmp\\pad.txt" "$scale" &
viewer=$!

wsl.exe -e bash -c "cd '$root_wsl' && \
    HOS_FRAMES_LIVE='$root_wsl/build/tmp/live.bmp' \
    HOS_INPUT_FILE='$root_wsl/build/tmp/pad.txt' \
    HOS_AUDIO=auto HOS_LOG_LEVEL=info \
    qemu-aarch64 -L /usr/aarch64-linux-gnu build/aarch64/src/hosrun \
        --romfs '$romfs' --sdmc '$sdmc' '$nso'" || true

kill "$viewer" 2>/dev/null || true
