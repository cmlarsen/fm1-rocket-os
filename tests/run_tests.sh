#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# X0X host tests: the maths, the sequencer + TB-3PO (against schwung-tb3po), each engine
# against its reference implementation, and the whole firmware app in the simulator
# (tests/scenarios/*.x0x: UI, patterns, persistence, MIDI clock), with screenshots and
# audio in build/scenarios/. Needs the reference sources (see tests/host/run_*.sh).
#   tests/run_tests.sh            everything
#   tests/run_tests.sh --unit     only the host unit suites (what `make unit` runs)
# The reference comparisons need their checkouts next to the tree; without them they are
# skipped (a plain macOS checkout has no ../schwung-*).
set -u
cd "$(dirname "$0")/.."
UNIT_ONLY=0
for a in "$@"; do
    [ "$a" = "--unit" ] && UNIT_ONLY=1
done
CC="${CC:-cc}"
FAIL=0
OUT=build/host
mkdir -p "$OUT" build/scenarios
run() {
    name="$1"; shift
    if "$@" > "$OUT/$name.log" 2>&1; then
        echo "  ok   $name"
    else
        echo "  FAIL $name (see $OUT/$name.log)"
        tail -15 "$OUT/$name.log" | sed 's/^/       /'
        FAIL=1
    fi
}
W="-O2 -ffp-contract=off -Wall -Wextra -Wdouble-promotion -Werror"
run fastmath sh -c "$CC -O2 -ffp-contract=off -Wall -Wextra -Werror -o $OUT/fastmath_test tests/host/fastmath_test.c -lm && $OUT/fastmath_test"   # its reference is double on purpose
TB3PO_REF="${TB3PO_REF:-../schwung-tb3po/src/dsp/tb3po.c}"
seq_test() {
    if [ -f "$TB3PO_REF" ]; then
        ref="$(cd "$(dirname "$TB3PO_REF")" && pwd)/$(basename "$TB3PO_REF")"
        $CC -O2 -ffp-contract=off -Wall -Wextra "-DREF_TB3PO=\"$ref\"" -o $OUT/seq_test tests/host/seq_test.c \
            firmware/src/seq/sequencer.c firmware/src/seq/tb3po.c firmware/src/seq/pattern.c || return 1
    else
        echo "(no schwung-tb3po checkout at $TB3PO_REF: the TB-3PO comparison is skipped)"
        $CC -O2 -ffp-contract=off -Wall -Wextra -o $OUT/seq_test tests/host/seq_test.c \
            firmware/src/seq/sequencer.c firmware/src/seq/tb3po.c firmware/src/seq/pattern.c || return 1
    fi
    $OUT/seq_test
}
run sequencer seq_test
run encoder sh -c "$CC -O2 -w -Ifirmware/hal -o $OUT/encoder_test tests/host/encoder_test.c && $OUT/encoder_test"
run bass303_lite sh -c "$CC -std=c99 -O2 -ffp-contract=off -w -Ifirmware/src/dsp -Ifirmware/src -o $OUT/bass303_lite_test tests/host/bass303_lite_test.c firmware/src/dsp/bass303.c -lm && $OUT/bass303_lite_test"
run uac sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/uac_test tests/host/uac_test.c -lm && $OUT/uac_test"
run trs sh -c "$CC -O2 -w -Ifirmware/src -Ifirmware/hal -o $OUT/trs_test tests/host/trs_test.c && $OUT/trs_test"
run undo sh -c "$CC -O2 -Wall -Wextra -Wno-unused-function -o $OUT/undo_test tests/host/undo_test.c && $OUT/undo_test"
run lz sh -c "$CC $W -o $OUT/lz_test tests/host/lz_test.c && $OUT/lz_test"
# T3: the LIVE bridge's pattern -> song mapping; clip_engine.c at the device FS (44.1 kHz)
run live_bridge sh -c "$CC $W -I. -Ifirmware/src -DX0X_HOST -DCLIP_ENGINE_FS=44100 -o $OUT/live_bridge_test tests/host/live_bridge_test.c firmware/src/app/live_bridge.c core/seq/clip_engine.c && $OUT/live_bridge_test"
# T6i: song_io over a RAM song flash (its own live_bridge stubs); song_store.c is its own unit
run song_io sh -c "$CC $W -I. -DX0X_HOST -o $OUT/song_io_test tests/host/song_io_test.c firmware/src/app/song_io.c firmware/src/app/song_templates.c core/model/song_store.c core/model/undo_stack.c && $OUT/song_io_test"
run motion sh -c "$CC $W -o $OUT/motion_test tests/host/motion_test.c firmware/src/seq/motion.c && $OUT/motion_test"
run drum-pan sh -c "$CC -O2 -ffp-contract=off -w -Ifirmware/src/dsp -Ifirmware/src -Ibuild/gen -o $OUT/drum_pan_test tests/host/drum_pan_test.c firmware/src/dsp/drum909.c firmware/src/dsp/drum808.c -lm && $OUT/drum_pan_test"
run stereo sh -c "$CC -O2 -ffp-contract=off -w -Ifirmware/src/dsp -Ifirmware/src -Ibuild/gen -o $OUT/stereo_test tests/host/stereo_test.c firmware/src/dsp/master.c firmware/src/dsp/fxbus.c -lm && $OUT/stereo_test"
run master sh -c "$CC $W -Wno-double-promotion -o $OUT/master_test tests/host/master_test.c firmware/src/dsp/master.c -lm && $OUT/master_test"
# the engine reference comparisons: run only when their checkout is next to the tree
NINEW9_DIR="${NINEW9_DIR:-../schwung-9W9}"
EIGHTW8_DIR="${EIGHTW8_DIR:-../schwung-8W8}"
SCHWUNG303="${SCHWUNG303:-../schwung-303}"
for t in drum909 drum808 bass303; do
    [ -f "tests/host/run_$t.sh" ] || continue
    case $t in
    drum909) [ -f "$NINEW9_DIR/src/dsp/er99_engine.c" ] || { echo "  skip $t (set NINEW9_DIR; $NINEW9_DIR not found)"; continue; } ;;
    drum808) [ -f "$EIGHTW8_DIR/src/dsp/sc808_engine.cpp" ] || { echo "  skip $t (set EIGHTW8_DIR; $EIGHTW8_DIR not found)"; continue; } ;;
    bass303) [ -f "$SCHWUNG303/src/dsp/open303/rosic_Open303.cpp" ] || { echo "  skip $t (set SCHWUNG303; $SCHWUNG303 not found)"; continue; } ;;
    esac
    run "$t" sh "tests/host/run_$t.sh"
done
run storage sh -c "$CC -O2 -o $OUT/storage_test tests/storage_test.c && $OUT/storage_test"
# the update path, against the firmware package (Felucca's tests; needs ./build.sh)
if [ -f build/x0x.fwsc ]; then
    run ota-entry sh -c "$CC -o $OUT/ota_test tests/ota_test.c && $OUT/ota_test build/x0x.fwsc"
    if [ -z "${AC79_SDK:-}" ]; then
        echo "  skip update-loader (needs AC79_SDK, as the build)"
    else
    run update-loader sh -c "head -c 200000 build/x0x.bin > $OUT/old_app.bin && \
        python3 tools/fm1pkg_make.py $OUT/old_app.bin build/loader/ota.bin $OUT/old.fwsc >/dev/null && \
        $CC -o $OUT/ldr_test tests/ldr_test.c && $OUT/ldr_test $OUT/old.fwsc build/x0x.fwsc"
    fi
    run installer python3 tests/install_test.py
    run check-loader python3 tests/check_loader_test.py build/x0x.fwsc
else
    echo "  skip update-path tests (no build/x0x.fwsc: run ./build.sh)"
fi
# T11h: every gesture-table button in the interaction model has a help card
run check-help python3 tools/check_help.py
run host-build sh host/build_host.sh
if [ "$UNIT_ONLY" = 1 ]; then
    [ $FAIL -eq 0 ] && echo "unit tests passed" || echo "UNIT TESTS FAILED"
    exit $FAIL
fi
if command -v emcc >/dev/null 2>&1 && command -v node >/dev/null 2>&1; then   # the browser build, when it can be built
    run emu sh tests/parity.sh
else
    echo "  skip emu/parity (needs emcc and node)"
fi
for s in tests/scenarios/*.x0x; do
    n=$(basename "$s" .x0x)
    mkdir -p "build/scenarios/$n"
    run "scenario-$n" build/host/x0x_host "$s" "build/scenarios/$n"
done
[ $FAIL -eq 0 ] && echo "all tests passed" || echo "TESTS FAILED"
exit $FAIL
