#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# WASM-vs-native parity (TFIXU 4, tests #3): build the host simulator and the browser build, run
# ONE shared scenario (tests/parity.x0x) through both, and compare the audio of both channels at
# equal length within 1 LSB (tests/host/emu_test.mjs). The screen and the lights must be drawn too.
# Needs emcc and node on PATH.
#   tests/parity.sh
set -e
cd "$(dirname "$0")/.."
SCEN=tests/parity.x0x
command -v node >/dev/null 2>&1 || { echo "parity: node is not on PATH (install Node.js)"; exit 2; }
command -v emcc >/dev/null 2>&1 || { echo "parity: emcc is not on PATH (install the Emscripten SDK)"; exit 2; }
mkdir -p build/host
sh host/build_host.sh >/dev/null
sh web/emu/build.sh >/dev/null 2>&1
build/host/x0x_host "$SCEN" build/host >/dev/null
if node tests/host/emu_test.mjs build/emu/x0x.wasm build/host/emu_ref.wav "$SCEN"; then
    echo "parity: WASM matches native (audio, screen, lights)"
else
    echo "parity: FAILED: the WASM build does not match the native host (see the emu line above)" >&2
    exit 1
fi
