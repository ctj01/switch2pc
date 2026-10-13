#!/usr/bin/env bash
# Pack what only the devkitPro machine can build (example and game NSOs, their
# romfs, the deko3d examples) for a machine without devkitPro, such as a native
# ARM64 server. Game data stays out of git; this archive is for your own
# machines only.
#
#   scripts/pack_artifacts.sh                 # -> build/artifacts.tar.gz
#   scp build/artifacts.tar.gz server:switch2pc/
#   ssh server 'cd switch2pc && tar xzf artifacts.tar.gz && scripts/build.sh'
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
files=()
for f in examples/*/build/*.nso examples/*/romfs build/deko_examples/deko_examples.nso \
    build/deko_examples/romfs; do
    [ -e "$f" ] && files+=("$f")
done
if [ ${#files[@]} -eq 0 ]; then
    echo "nothing to pack: build the examples first" >&2
    exit 1
fi
tar czf build/artifacts.tar.gz "${files[@]}"
echo "build/artifacts.tar.gz: ${#files[@]} entries, $(du -h build/artifacts.tar.gz | cut -f1)"
