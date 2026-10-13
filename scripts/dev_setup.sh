#!/usr/bin/env bash
# First-time setup in the dev container (.devcontainer, GitHub Codespaces):
# build the test homebrew with devkitPro, build switch2pc and run every test,
# then say what to try next. Safe to run again.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
echo "== building the test homebrew (libnx and deko3d examples)"
mkdir -p build
DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} scripts/ci_homebrew.sh > build/homebrew.log 2>&1 \
    || { tail -30 build/homebrew.log; echo "homebrew build failed; see build/homebrew.log"; exit 1; }
echo "== building switch2pc and running the tests"
scripts/build.sh

cat <<'EOF'

All set. Things to try:

  # run a homebrew and dump what it draws
  scripts/action_run.sh build/deko_examples/deko_examples.nro --frames out --duration 8000

  # the same with the nightly compatibility tooling, for one app of compat/apps.json
  compat/run.py --help

  # after changing code: rebuild and test
  scripts/build.sh

Where to start: docs/first-contribution.md
EOF
