#!/usr/bin/env bash
# Build the libFuzzer harnesses (#11) with clang and run each one for a while
# on a seed corpus generated from the repository's own writers. Host-only; the
# parsers under test are portable C. A crash leaves its input in
# build/fuzz/artifacts/<harness>/ and fails the script.
#
#   scripts/fuzz.sh [SECONDS] [HARNESS...]     (default: 60 s each, all three)
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
secs=${1:-60}
shift || true
harnesses=("$@")
[ ${#harnesses[@]} -gt 0 ] || harnesses=(nso_load mod0_relocate hipc_parse)

build=build/fuzz
cmake -S . -B "$build" -G Ninja -DCMAKE_C_COMPILER="${CC:-clang}" -DS2PC_FUZZ=ON \
    -DS2PC_BUILD_TESTS=OFF > /dev/null
cmake --build "$build"
"$build/fuzz/fuzz_seeds" "$build/seeds"

declare -A dir=([nso_load]=nso [mod0_relocate]=mod0 [hipc_parse]=hipc)
declare -A maxlen=([nso_load]=65536 [mod0_relocate]=65536 [hipc_parse]=256)
status=0
for h in "${harnesses[@]}"; do
    mkdir -p "$build/corpus/$h" "$build/artifacts/$h"
    echo "== fuzz_$h for ${secs}s"
    if ! "$build/fuzz/fuzz_$h" -max_total_time="$secs" -max_len="${maxlen[$h]}" -timeout=10 \
            -rss_limit_mb=4096 -print_final_stats=1 \
            -artifact_prefix="$build/artifacts/$h/" \
            "$build/corpus/$h" "$build/seeds/${dir[$h]}" 2>&1 | tail -15; then
        echo "::error::fuzz_$h found a crash; see $build/artifacts/$h/"
        status=1
    fi
done
exit $status
