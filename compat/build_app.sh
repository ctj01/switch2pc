#!/usr/bin/env bash
# Build one app from compat/apps.json with devkitPro, the way the nightly
# compatibility workflow does inside the devkitpro/devkita64 container.
#
#   compat/build_app.sh <name> <out dir>          (DEVKITPRO must be set)
#
# Clones the pinned ref of the app's repository (shallow, with submodules),
# installs the extra dkp-pacman packages the entry lists, runs its build
# commands from the repository root and copies the .nro to <out dir>/<name>.nro.
# A build failure is a result, not an error: the script always exits 0 and
# writes <out dir>/build.json ({"name", "ok", "seconds", "log_tail"}) plus the
# complete log in <out dir>/build.log. Locally, without devkitPro, run it in the
# container:
#
#   docker run --rm -v "$PWD:/work" devkitpro/devkita64:latest \
#       /work/compat/build_app.sh hbmenu /work/build/compat/hbmenu
set -uo pipefail

name=${1:?usage: build_app.sh <name> <out dir>}
out=${2:?usage: build_app.sh <name> <out dir>}
root=$(cd "$(dirname "$0")/.." && pwd)
apps="$root/compat/apps.json"
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export DEVKITA64=${DEVKITA64:-$DEVKITPRO/devkitA64}
export DEVKITARM=${DEVKITARM:-$DEVKITPRO/devkitARM}
export PATH="$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH"
timeout_s=${COMPAT_BUILD_TIMEOUT:-2400}

mkdir -p "$out"
log="$out/build.log"
: > "$log"

# One field of the entry, printed as a shell-safe string (lists joined by newlines).
field() {
    python3 - "$apps" "$name" "$1" <<'PY'
import json, sys
apps, name, key = sys.argv[1:]
for e in json.load(open(apps))["apps"]:
    if e["name"] == name:
        v = e.get(key, "")
        print("\n".join(v) if isinstance(v, list) else v)
        break
else:
    sys.exit(f"{name}: not in {apps}")
PY
}

finish() {
    local ok=$1 start=$2
    local secs=$(( $(date +%s) - start ))
    python3 - "$out/build.json" "$name" "$ok" "$secs" "$log" <<'PY'
import json, sys
path, name, ok, secs, log = sys.argv[1:]
lines = open(log, errors="replace").read().splitlines()
json.dump({"name": name, "ok": ok == "1", "seconds": int(secs), "log_tail": lines[-40:]},
          open(path, "w"), indent=1)
PY
    echo "$name: $([ "$ok" = 1 ] && echo built || echo BUILD FAILED) in ${secs}s (log: $log)"
    exit 0
}

start=$(date +%s)
repo=$(field repo) || finish 0 "$start"
ref=$(field ref)
nro=$(field nro)
pkgs=$(field dkp_packages | tr '\n' ' ')
build=$(field build)
case "$repo" in
http*) url=$repo ;;
*) url="https://github.com/$repo" ;;
esac
echo "== $name: $url @ $ref" | tee -a "$log"

# devkitPro Makefiles name the output after the directory, so clone under the
# repository's own name: hbmenu.nro comes out of nx-hbmenu/ as it does upstream.
src="$out/src/$(basename "${repo%.git}")"
rm -rf "$out/src"
mkdir -p "$src"
{
    git -C "$src" init -q &&
        git -C "$src" remote add origin "$url" &&
        # a tag or a commit sha, never a branch: GitHub serves both by name
        git -C "$src" fetch -q --depth 1 origin "$ref" &&
        git -C "$src" -c advice.detachedHead=false checkout -q FETCH_HEAD &&
        git -C "$src" submodule update -q --init --recursive --depth 1
} >> "$log" 2>&1 || {
    echo "checkout of $url @ $ref failed" | tee -a "$log"
    finish 0 "$start"
}
echo "checked out $(git -C "$src" rev-parse HEAD)" | tee -a "$log"

if [ -n "${pkgs// /}" ]; then
    echo "== installing: $pkgs" | tee -a "$log"
    # shellcheck disable=SC2086
    dkp-pacman -S --noconfirm --needed $pkgs >> "$log" 2>&1 ||
        { echo "dkp-pacman failed" | tee -a "$log"; finish 0 "$start"; }
fi

echo "== build:" | tee -a "$log"
printf '%s\n' "$build" | tee -a "$log"
if ! (cd "$src" && timeout "$timeout_s" bash -euo pipefail -c "$build") >> "$log" 2>&1; then
    echo "build command failed" | tee -a "$log"
    tail -15 "$log"
    finish 0 "$start"
fi
if [ ! -f "$src/$nro" ]; then
    echo "build finished but $nro is missing" | tee -a "$log"
    finish 0 "$start"
fi
cp "$src/$nro" "$out/$name.nro"
rm -rf "$out/src"
echo "== $out/$name.nro ($(stat -c %s "$out/$name.nro") bytes)" | tee -a "$log"
finish 1 "$start"
