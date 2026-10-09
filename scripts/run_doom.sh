#!/usr/bin/env bash
# Kept for the README: plays DOOM. See run_game.sh.
exec "$(dirname "$0")/run_game.sh" doom "${1:-2}"
