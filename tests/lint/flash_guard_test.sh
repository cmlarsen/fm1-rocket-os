#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# T2b: build and run the flash-write guard test on the host (see flash_guard_test.c).
set -e
cd "$(dirname "$0")/../.."
CC="${CC:-cc}"
mkdir -p build/host
"$CC" -std=c99 -O2 -w -Ifirmware/hal -o build/host/flash_guard_test tests/lint/flash_guard_test.c
build/host/flash_guard_test
