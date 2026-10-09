#!/usr/bin/env bash
# Play a game under hosrun from Windows: hosrun runs in WSL/qemu and renders
# into a shared BMP, a PowerShell window shows it and feeds the keyboard back.
# Run from Git Bash after scripts/build.sh and the game's build script.
#
#   scripts/run_game.sh <doom|quake|tyrian> [scale]
#
# Keys in the window: arrows move, Z fires, X jumps/uses, C/V/Q/E = X/Y/L/R,
# Enter = Plus, Esc = Minus. Close the window to quit.
set -euo pipefail

game=${1:-doom}
scale=${2:-2}
root=$(cd "$(dirname "$0")/.." && pwd)                       # /c/Users/...
root_win=$(cygpath -w "$root")                               # C:\Users\...
root_wsl="/mnt/$(echo "$root" | sed -E 's|^/([a-z])/|\1/|')" # /mnt/c/Users/...

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
