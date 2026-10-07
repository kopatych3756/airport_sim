  #!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-test"
OUT="$ROOT/logs/tests"
mkdir -p "$OUT"

cmake -S "$ROOT" -B "$BUILD"
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure

run_case() {
    "$BUILD/airport_sim" --config "$ROOT/configs/$1.conf" --log "$OUT/$1.log" --quiet
}

run_case test_nominal
run_case test_emergency
run_case test_closure
run_case test_incompatible
run_case test_safety

grep -q "COMPLETE" "$OUT/test_nominal.log"
grep -q "termination=ALL_REQUESTS_COMPLETED" "$OUT/test_nominal.log"
grep -q "EMERGENCY" "$OUT/test_emergency.log"
grep -q "assigned A001.*EMERGENCY" "$OUT/test_emergency.log"
grep -q "current operation continues" "$OUT/test_closure.log"
grep -q "OPEN" "$OUT/test_closure.log"
grep -q "DIVERT" "$OUT/test_incompatible.log"
grep -q "SHIFT-END" "$OUT/test_incompatible.log"
grep -q "termination=SHIFT_ENDED" "$OUT/test_incompatible.log"
grep -q "next start not before T=4" "$OUT/test_safety.log"
grep -q "\[T=0004\].*started TAKEOFF" "$OUT/test_safety.log"

# Duplicate aircraft numbers must be rejected before the simulation starts.
if "$BUILD/airport_sim" --config "$ROOT/configs/test_duplicate.conf" --no-log --quiet \
    >"$OUT/test_duplicate.stdout" 2>"$OUT/test_duplicate.stderr"; then
    echo "Duplicate aircraft test unexpectedly succeeded." >&2
    exit 1
fi
grep -q "Duplicate aircraft number: A001" "$OUT/test_duplicate.stderr"

# Unlimited mode must stop cleanly after SIGINT and write the STOP event.
"$BUILD/airport_sim" --config "$ROOT/configs/test_interrupt.conf" --log "$OUT/test_interrupt.log" --quiet &
pid=$!
sleep 0.12
kill -INT "$pid"
set +e
wait "$pid"
status=$?
set -e
if [ "$status" -ne 130 ]; then
    echo "SIGINT test returned $status instead of 130." >&2
    exit 1
fi
grep -q "STOP" "$OUT/test_interrupt.log"
grep -q "termination=USER_INTERRUPT" "$OUT/test_interrupt.log"

echo "All tests passed."
