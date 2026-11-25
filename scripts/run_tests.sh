#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
case "${1:-tcp}" in
    tcp|all) mode=${1:-tcp} ;;
    *) printf '%s\n' "Usage: $0 [tcp|all]" >&2; exit 2 ;;
esac
if command -v cmake >/dev/null 2>&1; then
    cmake -S . -B build
    cmake --build build -j 4
else
    make -j4 all
fi
python3 tests/test_tcp.py --bin-dir build
if [ "$mode" = all ]; then
    python3 tests/run_tests.py
fi
