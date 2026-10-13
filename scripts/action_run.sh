#!/usr/bin/env bash
# Run one homebrew under hosrun and check what it did: the GitHub Action
# (action.yml) calls this, and it works the same by hand after scripts/build.sh.
#
#   scripts/action_run.sh <app.nro|app.nso> [options]
#     --romfs <dir|file>    data storage (default for an .nro: its own RomFS)
#     --sdmc <dir>          SD card directory (default: an empty one)
#     --input <file>        input script: lines "ms buttons_hex [lx ly rx ry]"
#     --duration <ms>       ask the app to exit after this long (default 10000)
#     --frames <dir>        dump presented frames here as PPM
#     --every <n>           dump every n-th frame (default 30)
#     --expect <regex>      fail unless the log matches (extended regex)
#     --reference <dir>     compare dumped frames with same-named PPMs here
#     --tolerance <n>       per-channel difference allowed (default 2)
#     --log <file>          where to write the log (default: <frames or tmp>/hosrun.log)
#
# Fails when hosrun crashes or does not exit after the exit request, when the
# expected text is missing, or when a frame differs from its reference.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
app=""
romfs="" sdmc="" input="" duration=10000 frames="" every=30 expect="" reference=""
tolerance=2 log=""
while [ $# -gt 0 ]; do
    case "$1" in
    --romfs) romfs=$2; shift 2 ;;
    --sdmc) sdmc=$2; shift 2 ;;
    --input) input=$2; shift 2 ;;
    --duration) duration=$2; shift 2 ;;
    --frames) frames=$2; shift 2 ;;
    --every) every=$2; shift 2 ;;
    --expect) expect=$2; shift 2 ;;
    --reference) reference=$2; shift 2 ;;
    --tolerance) tolerance=$2; shift 2 ;;
    --log) log=$2; shift 2 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) app=$1; shift ;;
    esac
done
[ -n "$app" ] || { echo "usage: $0 <app.nro> [options]" >&2; exit 2; }
[ -f "$app" ] || { echo "$app not found" >&2; exit 2; }

if [ -n "${S2PC_HOSRUN:-}" ]; then
    read -r -a run <<< "$S2PC_HOSRUN"
elif [ "$(uname -m)" = aarch64 ]; then
    run=("$root/build/host/src/hosrun")
else
    run=(qemu-aarch64 -L /usr/aarch64-linux-gnu "$root/build/aarch64/src/hosrun")
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
if [ -z "$sdmc" ]; then
    sdmc="$work/sdmc"
    mkdir -p "$sdmc"
fi
script="$work/input.txt"
{ [ -n "$input" ] && cat "$input"; echo "$duration exit"; } > "$script"
if [ -z "$log" ]; then
    log="${frames:-$work}/hosrun.log"
fi
mkdir -p "$(dirname "$log")"
[ -n "$frames" ] && mkdir -p "$frames"

args=(--sdmc "$sdmc")
[ -n "$romfs" ] && args+=(--romfs "$romfs")
env_vars=(HOS_INPUT_SCRIPT="$script")
[ -n "$frames" ] && env_vars+=(HOS_FRAMES="$frames" HOS_FRAMES_EVERY="$every")

limit=$(( duration / 1000 + 60 ))
set +e
env "${env_vars[@]}" timeout "$limit" "${run[@]}" "${args[@]}" "$app" > "$log" 2>&1
status=$?
set -e
frames_shown=$(sed -n 's/.*frames presented: \([0-9]*\).*/\1/p' "$log" | tail -1)
echo "switch2pc: $(basename "$app") exited with $status after ${duration} ms; log: $log"
grep -E "hosrun\] I: .* by |\] E: |Panic|crash|fatal" "$log" | head -20 || true

fail=0
if [ "$status" -eq 124 ]; then
    echo "::error::$(basename "$app") did not exit within ${limit}s of the exit request"
    fail=1
elif [ "$status" -ne 0 ]; then
    echo "::error::$(basename "$app") stopped with status $status (see $log)"
    tail -30 "$log"
    fail=1
fi
if [ -n "$expect" ] && ! grep -qE "$expect" "$log"; then
    echo "::error::the log does not match: $expect"
    fail=1
fi
if [ -n "$reference" ]; then
    [ -n "$frames" ] || { echo "::error::--reference needs --frames"; exit 1; }
    python3 "$root/tools/compare_frames.py" "$frames" "$reference" "$tolerance" || fail=1
fi
echo "switch2pc: ${frames_shown:-0} frames presented"
exit "$fail"
