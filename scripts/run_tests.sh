#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build}"
cmake -S "$ROOT" -B "$BUILD"
cmake --build "$BUILD" -j
BUILD="$(cd "$BUILD" && pwd)"
OUT="$ROOT/logs/tests"
mkdir -p "$OUT"


for program in airport_sem airport_cond; do
    for scenario in nominal emergency closure incompatible safety; do
        log="$OUT/${program}_${scenario}.log"
        timeout 15 "$BUILD/$program" -c "$ROOT/configs/test_${scenario}.conf" \
            --delay-ms 10 --quiet --log "$log"
        grep -q '=== SUMMARY ===' "$log"
        echo "PASS $program: $scenario"
    done

    grep -q 'termination=ALL_REQUESTS_COMPLETED' "$OUT/${program}_nominal.log"
    grep -q 'EMERGENCY' "$OUT/${program}_emergency.log"
    grep -q 'CLOSE' "$OUT/${program}_closure.log"
    grep -q 'OPEN' "$OUT/${program}_closure.log"
    grep -q 'DIVERT' "$OUT/${program}_incompatible.log"
    grep -q 'termination=SHIFT_ENDED' "$OUT/${program}_incompatible.log"


    awk '
        /RELEASE/ { split($0, parts, "T="); earliest=parts[3]+0 }
        /START/ { split($0, parts, "T="); if (parts[2]+0 < earliest) exit 1 }
    ' "$OUT/${program}_safety.log"

    set +e
    "$BUILD/$program" -c "$ROOT/configs/test_duplicate.conf" --quiet --no-log \
        2> "$OUT/${program}_duplicate.txt"
    status=$?
    set -e
    test "$status" -eq 2
    echo "PASS $program: duplicate rejected"


    for sig in INT TERM; do
        log="$OUT/${program}_${sig}.log"
        : > "$log"
        "$BUILD/$program" -c "$ROOT/configs/test_interrupt.conf" --quiet --log "$log" &
        pid=$!
        trap 'kill -KILL "$pid" 2>/dev/null || true' EXIT
        for attempt in {1..200}; do
            if grep -q 'REQUEST' "$log"; then break; fi
            sleep 0.01
        done
        grep -q 'REQUEST' "$log"
        kill -"$sig" "$pid"

        (sleep 5; kill -KILL "$pid" 2>/dev/null || true) &
        watchdog=$!
        set +e
        wait "$pid"
        status=$?
        set -e
        kill "$watchdog" 2>/dev/null || true
        wait "$watchdog" 2>/dev/null || true
        trap - EXIT
        test "$status" -eq 130
        grep -q 'termination=USER_INTERRUPT' "$log"
        grep -q 'joined_threads=3 created_threads=3' "$log"
        echo "PASS $program: SIG$sig"
    done
done
echo 'All 16 runs passed.'
