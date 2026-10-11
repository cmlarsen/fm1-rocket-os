#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# fm1-rocket-os T5c: the FM6 bit-identical parity test and the X0X adapter test.
# It fetches Felucca's ORIGINAL fm6_core.c and tables from the `felucca` remote into build/ref/ (the
# reference), generates the port's tables and factory patches, and builds and runs both tests.
#   tests/host/run_fm6.sh
set -eu
cd "$(dirname "$0")/../.."
CC="${CC:-cc}"
OUT=build/host
mkdir -p "$OUT" build/ref build/gen
git rev-parse --verify felucca/main >/dev/null 2>&1 || { echo "run_fm6: no felucca/main (git remote add felucca https://github.com/hugelton/Felucca.git && git fetch felucca)"; exit 2; }
git show felucca/main:firmware/src/fm6_core.c > build/ref/fm6_core.c
git show felucca/main:tools/gen_tables.py > build/ref/gen_tables.py
python3 build/ref/gen_tables.py build/ref/felucca_tables.h >/dev/null
python3 tools/gen_fm6_tables.py firmware/src/dsp/fm6_tables.h >/dev/null
python3 tools/gen_fm6_patches.py build/gen/fm6_patches.h >/dev/null
W="-std=c99 -O2 -ffp-contract=off -Wno-unused-function -Wno-unused-const-variable"
CORE="firmware/src/dsp/fm6_core.c firmware/src/dsp/fm6_vbank.c"
# shellcheck disable=SC2086
$CC $W -Ibuild/ref -Itests/host -c tests/host/fm6_ref_tu.c -o "$OUT/fm6_ref_tu.o"
# shellcheck disable=SC2086
$CC $W -Ifirmware/src/dsp -Itests/host -c tests/host/fm6_port_tu.c -o "$OUT/fm6_port_tu.o"
# shellcheck disable=SC2086
$CC $W -Ibuild/gen -Ifirmware/src/dsp -Itests/host -c tests/host/fm6_parity.c -o "$OUT/fm6_parity.o"
# shellcheck disable=SC2086
$CC $W -Ibuild/gen -Ifirmware/src/dsp -Itests/host -c tests/host/fm6_engine.c -o "$OUT/fm6_engine.o"
# shellcheck disable=SC2086
$CC -std=c99 -O2 -ffp-contract=off -Ibuild/gen -Ifirmware/src/dsp "$OUT/fm6_ref_tu.o" "$OUT/fm6_port_tu.o" "$OUT/fm6_parity.o" $CORE -o "$OUT/fm6_parity" -lm
# shellcheck disable=SC2086
$CC -std=c99 -O2 -ffp-contract=off -Ibuild/gen -Ifirmware/src/dsp "$OUT/fm6_ref_tu.o" "$OUT/fm6_engine.o" $CORE firmware/src/dsp/eng_fm6.c -o "$OUT/fm6_engine" -lm
"$OUT/fm6_parity"
"$OUT/fm6_engine"
