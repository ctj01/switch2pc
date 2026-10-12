#!/usr/bin/env bash
# Build devkitPro's deko3d examples (the real-homebrew GPU test: docs/gpu.md)
# from the copy devkitPro installs under examples/switch/graphics/deko3d. The
# only change is a test selector in main.cpp: sdmc:/deko_example.txt names the
# example to run, so no menu navigation is needed. Run from Git Bash on the
# machine with devkitPro. Output: build/deko_examples/deko_examples.nso and
# its romfs (shaders compiled by the Makefile, model and texture data).
#
#   scripts/build_deko_examples.sh
#   scripts/run_game.sh deko <1-9>
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
out="$root/build/deko_examples"
dkp=${DEVKITPRO:-/c/devkitPro}
[ -d "$dkp" ] || dkp=/c/devkitPro
src="$dkp/examples/switch/graphics/deko3d/deko_examples"
[ -d "$src" ] || { echo "$src not found: install devkitPro's switch-examples" >&2; exit 1; }

if [ ! -f "$out/Makefile" ]; then
    mkdir -p "$out"
    cp -r "$src/." "$out/"
fi

main="$out/source/main.cpp"
if ! grep -q "switch2pc test harness" "$main"; then
    # after main()'s opening brace
    awk '{ print }
         /^int main\(int argc, char\* argv\[\]\)$/ { m = 1; next }
         m && /^\{$/ {
             print "    // switch2pc test harness: sdmc:/deko_example.txt names one example to run directly."
             print "    if (FILE* f = fopen(\"sdmc:/deko_example.txt\", \"r\"))"
             print "    {"
             print "        unsigned n = 0;"
             print "        if (fscanf(f, \"%u\", &n) == 1 && n >= 1 && n <= Examples.size())"
             print "        {"
             print "            fclose(f);"
             print "            Examples[n - 1].mainfunc();"
             print "            return 0;"
             print "        }"
             print "        fclose(f);"
             print "    }"
             m = 0
         }' "$main" > "$main.tmp"
    mv "$main.tmp" "$main"
    grep -q "switch2pc test harness" "$main" || { echo "could not patch $main" >&2; exit 1; }
fi

case "$(uname -s)" in
MINGW* | MSYS*) out_m=$(cygpath -m "$out") ;;
*) out_m=$out ;;
esac
# devkitPro's Makefiles expect its MSYS2 environment (make, DEVKITPRO=/opt/devkitpro).
"$dkp/msys2/usr/bin/bash.exe" -lc "export DEVKITPRO=/opt/devkitpro; cd \"\$(cygpath -u '$out_m')\" && make"
"$dkp/tools/bin/elf2nso" "$out/deko_examples.elf" "$out/deko_examples.nso"
echo "built $out/deko_examples.nso"
