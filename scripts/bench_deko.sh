#!/usr/bin/env bash
# Frames per second of the deko3d examples (scripts/build_deko_examples.sh),
# headless: each runs for SECONDS and the presented frames are counted. Runs
# natively on ARM64 and through qemu-user elsewhere; HOS_GPU_THREADS and
# HOS_PROFILE pass through. Numbers to compare against are in docs/gpu.md.
#
#   scripts/bench_deko.sh [SECONDS] [EXAMPLE...]     (default: 30 s, 3 4 6 7 8)
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
secs=${1:-30}
shift || true
examples=("$@")
[ ${#examples[@]} -gt 0 ] || examples=(3 4 6 7 8)
if [ "$(uname -m)" = aarch64 ]; then
    run=(build/host/src/hosrun)
else
    run=(qemu-aarch64 -L /usr/aarch64-linux-gnu build/aarch64/src/hosrun)
fi
mkdir -p build/tmp
for n in "${examples[@]}"; do
    sd=build/tmp/sdmc_bench$n
    log=build/tmp/bench$n.log
    mkdir -p "$sd"
    echo "$n" > "$sd/deko_example.txt"
    echo "$((secs * 1000)) exit" > "build/tmp/bench_input$n.txt"
    HOS_LOG_LEVEL=debug HOS_INPUT_SCRIPT="build/tmp/bench_input$n.txt" timeout $((secs + 60)) \
        "${run[@]}" --romfs build/deko_examples/romfs --sdmc "$sd" \
        build/deko_examples/deko_examples.nso > "$log" 2>&1 || true
    frames=$(grep -cE "vi   \] [ID]: frame [0-9]+:" "$log" || true)
    awk -v n="$n" -v f="$frames" -v s="$secs" \
        'BEGIN { printf "example %s: %d frames in %d s (%.2f fps)\n", n, f, s, f / s }'
done
