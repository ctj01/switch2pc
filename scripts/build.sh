#!/usr/bin/env bash
# Full build: native tool, then the ARM64 runtime (cross-compiled when the host
# is not ARM64), then every test. The native tool packages the example NSOs.
# S2PC_NO_TESTS=1 builds without running the tests (the GitHub Action does).
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
gen=${S2PC_GENERATOR:-Ninja}

cmake -S "$root" -B "$root/build/host" -G "$gen"
cmake --build "$root/build/host"

run_tests=1
[ "${S2PC_NO_TESTS:-0}" = 1 ] && run_tests=0

if [ "$(uname -m)" = "aarch64" ]; then
    # Native ARM64: the host build already contains the runtime.
    [ $run_tests = 1 ] && ctest --test-dir "$root/build/host" --output-on-failure
    exit 0
fi

cmake -S "$root" -B "$root/build/aarch64" -G "$gen" \
    -DCMAKE_TOOLCHAIN_FILE="$root/cmake/aarch64-linux-gnu.cmake" \
    -DS2PC_HOST_TOOL="$root/build/host/src/switch2pc"
cmake --build "$root/build/aarch64"

if [ $run_tests = 1 ]; then
    ctest --test-dir "$root/build/host" --output-on-failure
    ctest --test-dir "$root/build/aarch64" --output-on-failure
fi
