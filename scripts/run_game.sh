#!/usr/bin/env bash
# Play a game under hosrun from Windows. hosrun runs in WSL/qemu and, by
# default, opens its own X11 window through WSLg (HOS_WINDOW=1; needs a cross
# build with libx11-dev:arm64). With --viewer it instead renders into a shared
# BMP that tools/viewer.ps1 shows, feeding the keyboard back through a file.
# Run from Git Bash or devkitPro's MSYS2 after scripts/build.sh and the
# game's build script.
#
#   scripts/run_game.sh <doom|quake|tyrian> [scale] [--viewer]
#
# Keys: arrows = D-pad, Z/Ctrl = A, X/Space = B, C/V = X/Y, Q/E = L/R,
# Enter = Plus, Esc = Minus (window mode also: WASD = left stick).
# Close the window to quit; the game gets the applet exit request.
set -euo pipefail

game=${1:-doom}
scale=${2:-2}
mode=window
[ "${3:-}" = "--viewer" ] || [ "${2:-}" = "--viewer" ] && mode=viewer
[ "${2:-}" = "--viewer" ] && scale=2
root=$(cd "$(dirname "$0")/.." && pwd)                       # /c/Users/...
root_win=$(cygpath -w "$root")                               # C:\Users\...
# From the Windows path, so it works in Git Bash (/c/...) and in devkitPro's
# MSYS2 shell, where the same folder is /home/<user>/...
root_mixed=$(cygpath -m "$root")                             # C:/Users/...
drive=$(printf '%s' "${root_mixed%%:*}" | tr 'A-Z' 'a-z')
root_wsl="/mnt/$drive${root_mixed#?:}"                       # /mnt/c/Users/...

case "$game" in
doom) nso="examples/doom/build/doom.nso"; romfs="examples/doom/romfs" ;;
quake) nso="examples/quake/build/quake.nso"; romfs="examples/quake/romfs" ;;
tyrian) nso="examples/tyrian/build/tyrian.nso"; romfs="examples/tyrian/romfs" ;;
*)
    echo "unknown game: $game (doom, quake, tyrian)" >&2
    exit 2
    ;;
esac
if [ ! -f "$root/$nso" ]; then
    echo "$nso is missing; run scripts/build_$game.sh first (build_opentyrian.sh for tyrian)" >&2
    exit 1
fi

mkdir -p "$root/build/tmp/sdmc"

if [ "$mode" = window ]; then
    wsl.exe -e bash -c "cd '$root_wsl' && \
        HOS_WINDOW=1 HOS_WINDOW_SCALE='$scale' HOS_LOG_LEVEL=info \
        qemu-aarch64 -L /usr/aarch64-linux-gnu build/aarch64/src/hosrun \
            --romfs '$romfs' --sdmc build/tmp/sdmc '$nso'" || true
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
    HOS_LOG_LEVEL=info \
    qemu-aarch64 -L /usr/aarch64-linux-gnu build/aarch64/src/hosrun \
        --romfs '$romfs' --sdmc build/tmp/sdmc '$nso'" || true

kill "$viewer" 2>/dev/null || true
