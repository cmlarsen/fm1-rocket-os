#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# fm1-rocket-os T5c: the NOISE bit-identical parity test and the X0X adapter test.
# It fetches Felucca's ORIGINAL dsp.c and eng_noise.c from the `felucca` remote into build/ref/ (the
# reference) and builds and runs the test.
#   tests/host/run_noise.sh
set -eu
cd "$(dirname "$0")/../.."
CC="${CC:-cc}"
OUT=build/host
mkdir -p "$OUT" build/ref
git rev-parse --verify felucca/main >/dev/null 2>&1 || { echo "run_noise: no felucca/main (git remote add felucca https://github.com/hugelton/Felucca.git && git fetch felucca)"; exit 2; }
git show felucca/main:firmware/src/dsp.c > build/ref/dsp.c
git show felucca/main:firmware/src/eng_noise.c > build/ref/eng_noise.c
python3 tools/gen_fm6_tables.py firmware/src/dsp/fm6_tables.h >/dev/null
W="-std=c99 -O2 -ffp-contract=off -Wno-unused-function -Wno-unused-const-variable"
# shellcheck disable=SC2086
$CC $W -Ibuild/ref -Itests/host -c tests/host/noise_ref_tu.c -o "$OUT/noise_ref_tu.o"
# shellcheck disable=SC2086
$CC $W -Ifirmware/src/dsp -Itests/host -c tests/host/noise_port_tu.c -o "$OUT/noise_port_tu.o"
# shellcheck disable=SC2086
$CC $W -Ifirmware/src/dsp -Itests/host -c tests/host/noise_parity.c -o "$OUT/noise_parity.o"
$CC -std=c99 -O2 -ffp-contract=off "$OUT/noise_ref_tu.o" "$OUT/noise_port_tu.o" "$OUT/noise_parity.o" firmware/src/dsp/eng_noise.c -o "$OUT/noise_parity" -lm
"$OUT/noise_parity"
