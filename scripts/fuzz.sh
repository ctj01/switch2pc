#!/usr/bin/env bash
# Build the libFuzzer harnesses (#11) with clang and run each one for a while
# on a seed corpus generated from the repository's own writers. Host-only; the
# parsers under test are portable C. A crash leaves its input in
# build/fuzz/artifacts/<harness>/ and fails the script.
#
#   scripts/fuzz.sh [SECONDS | --runs [N]] [HARNESS...]   (default: 60 s each, all three)
#
# --runs N is the deterministic form (fixed seed, N iterations, a million by
# default: about half a minute) CI uses for pull requests and branches, so a
# latent bug cannot turn an unrelated change red at random; the time-boxed form
# runs on main with its corpus kept between runs.
# Each run's output goes to build/fuzz/artifacts/<harness>/log.txt.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
mode=(-max_total_time=60)
if [ "${1:-}" = "--runs" ]; then
    shift
    runs=1000000
    if [ $# -gt 0 ] && [ "$1" -eq "$1" ] 2> /dev/null; then
        runs=$1
        shift
    fi
    mode=(-seed=1 -runs="$runs")
elif [ $# -gt 0 ]; then
    mode=(-max_total_time="$1")
    shift
fi
harnesses=("$@")
[ ${#harnesses[@]} -gt 0 ] || harnesses=(nso_load mod0_relocate hipc_parse)

build=build/fuzz
cmake -S . -B "$build" -G Ninja -DCMAKE_C_COMPILER="${CC:-clang}" -DS2PC_FUZZ=ON \
    -DS2PC_BUILD_TESTS=OFF > /dev/null
cmake --build "$build" --target fuzz_nso_load fuzz_mod0_relocate fuzz_hipc_parse fuzz_seeds
"$build/fuzz/fuzz_seeds" "$build/seeds"

declare -A dir=([nso_load]=nso [mod0_relocate]=mod0 [hipc_parse]=hipc)
declare -A maxlen=([nso_load]=65536 [mod0_relocate]=65536 [hipc_parse]=256)
status=0
for h in "${harnesses[@]}"; do
    mkdir -p "$build/corpus/$h" "$build/artifacts/$h"
    echo "== fuzz_$h ${mode[*]}"
    if ! "$build/fuzz/fuzz_$h" "${mode[@]}" -max_len="${maxlen[$h]}" -timeout=10 \
            -rss_limit_mb=4096 -print_final_stats=1 \
            -artifact_prefix="$build/artifacts/$h/" \
            "$build/corpus/$h" "$build/seeds/${dir[$h]}" 2>&1 | tee "$build/artifacts/$h/log.txt" |
            tail -15; then
        echo "::error::fuzz_$h found a crash; see $build/artifacts/$h/ (input and log.txt)"
        status=1
    fi
done
exit $status
