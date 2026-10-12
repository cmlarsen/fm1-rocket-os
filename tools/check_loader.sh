#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# fm1-rocket-os: the loader guard. Exit 0 only if the loader in a package matches the one
# recorded in tools/x0x_loader.sha256 (X0X 1.0.5's, the only loader the unattended-flashing
# rule in docs/engineering/build-plan.md permits).
#
#   tools/check_loader.sh build/x0x.fwsc     # one installable package
#   tools/check_loader.sh build               # every *.fwsc in the directory (all must pass)
#
# The selection, structure checks and exit codes live in tools/loader_check.py, the same module
# tools/fm1_install.py uses, so the installer enforces exactly what this guard checks. The loader
# is the UFW entry of type 100 (firmware/src/ota.c, ota_ufw), never a file merely named "ota.bin".
# A package with no type-100 entry, more than one, a bad header or entry-list CRC, a wrong chip
# tag, a bad entry count or a truncated entry fails closed (exit 2); a well-formed package whose
# loader differs exits 1. Stripping the BREAK part must not change the loader: run this on the
# stripped build too.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
ARG=${1:-}

if [ -z "$ARG" ]; then
    echo "usage: tools/check_loader.sh <package.fwsc | directory>" >&2
    exit 2
fi

exec python3 "$HERE/loader_check.py" "$ROOT/tools/x0x_loader.sha256" "$ARG"
