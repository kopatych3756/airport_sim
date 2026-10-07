#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build" "$ROOT/logs"
cmake -S "$ROOT" -B "$ROOT/build"
cmake --build "$ROOT/build"
"$ROOT/build/airport_sim" --config "$ROOT/configs/default.conf" --delay-ms 100 --log "$ROOT/logs/demo.log"
