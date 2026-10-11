#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Compare a scenario's output directory against its golden directory (T2a).
#   tests/golden.sh OUTDIR GOLDENDIR
#   GOLDEN_UPDATE=1 tests/golden.sh OUTDIR GOLDENDIR   # copy OUTDIR over the goldens
# The work is tests/golden.py: PNGs by decoded pixels, JSON by structure, *.wav by loose
# frames/peak/rms metrics (see the docstring there).
exec python3 "$(dirname "$0")/golden.py" "$@"
