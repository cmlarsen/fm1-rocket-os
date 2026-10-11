#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# T2c: the UI fuzz driver. Builds the host simulator, runs `fuzz SEED N` for seeds 1..SEEDS,
# and on a failure (a hung voice or a runner crash) minimises the reproduction into
# build/fuzz/seed_<S>.min.x0x alongside the full sequence seed_<S>.full.x0x. Exits non-zero
# if any seed fails. `make fuzz-ui` is the long run (200 x 2000); `make check` is the short one.
#   tests/fuzz/run_fuzz.sh [SEEDS] [EVENTS]     (defaults 20 500)
set -u
cd "$(dirname "$0")/../.."
SEEDS="${1:-20}"
EVENTS="${2:-500}"
HOST=build/host/x0x_host
OUT=build/fuzz
mkdir -p "$OUT"
[ -x "$HOST" ] || sh host/build_host.sh
# bound a stuck seed (a UI infinite loop) if `timeout` is available; the shrinker has its own
TO=""
command -v timeout >/dev/null 2>&1 && TO="timeout 600"
fail=0
i=0
while [ "$i" -lt "$SEEDS" ]; do
    s=$((i + 1))
    sc="$OUT/seed_$s.x0x"
    printf 'fuzz %d %d\n' "$s" "$EVENTS" > "$sc"
    if $TO "$HOST" "$sc" "$OUT" > "$OUT/seed_$s.log" 2>&1; then
        echo "  ok   fuzz seed $s"
    else
        echo "  FAIL fuzz seed $s (see $OUT/seed_$s.log)"
        tail -4 "$OUT/seed_$s.log" | sed 's/^/       /'
        fail=1
        if [ -f "$OUT/seed_$s.full.x0x" ]; then
            if python3 tests/fuzz/shrink.py "$OUT/seed_$s.full.x0x" "$OUT/seed_$s.min.x0x" "$HOST" "$OUT"; then
                echo "       minimised reproduction: $OUT/seed_$s.min.x0x"
            else
                echo "       not minimised; the full sequence is $OUT/seed_$s.full.x0x"
            fi
        fi
    fi
    i=$((i + 1))
done
if [ "$fail" -eq 0 ]; then
    echo "fuzz-ui: $SEEDS seeds x $EVENTS events: all passed"
else
    echo "fuzz-ui: FAILED"
    exit 1
fi
