#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# WASM-vs-native parity (T2a): build the host simulator and the browser build, run the same
# script through both, and compare the audio sample for sample (tests/host/emu_test.mjs). The
# screen and the lights must be drawn too. Needs emcc and node on PATH.
#   tests/parity.sh
set -e
cd "$(dirname "$0")/.."
command -v node >/dev/null 2>&1 || { echo "parity: node is not on PATH (install Node.js)"; exit 2; }
command -v emcc >/dev/null 2>&1 || { echo "parity: emcc is not on PATH (install the Emscripten SDK)"; exit 2; }
mkdir -p build/host
sh host/build_host.sh >/dev/null
printf 'wav emu_ref.wav\nwait 500\ntapkey b0\ntapkey w0\ntapkey w8\ntap PLAY\nwait 2960\nwavstop\n' > build/host/emu_ref.x0x
sh web/emu/build.sh >/dev/null 2>&1
build/host/x0x_host build/host/emu_ref.x0x build/host >/dev/null
node tests/host/emu_test.mjs build/emu/x0x.wasm build/host/emu_ref.wav
echo "parity: WASM matches native (audio, screen, lights)"
