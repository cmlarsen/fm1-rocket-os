#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# X0X in the browser: the host simulator compiled to WebAssembly (Emscripten), plus its page.
#   [X0X_VERSION=0.4-beta] web/emu/build.sh  ->  build/emu/{index.html, worklet.js, x0x.wasm}
# Same sources and flags as host/build_host.sh (-ffp-contract=off, like the device).
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/gen build/emu
[ -f build/gen/felucca_font.h ] || python3 tools/gen_font.py build/gen/felucca_font.h >/dev/null
[ -f build/gen/x0x_drum_samples.h ] || python3 tools/gen_drum_samples.py build/gen/x0x_drum_samples.h >/dev/null
[ -f build/gen/fm6_patches.h ] || python3 tools/gen_fm6_patches.py build/gen/fm6_patches.h >/dev/null
U="firmware/src/dsp/drum909.c firmware/src/dsp/drum808.c firmware/src/dsp/bass303.c
   firmware/src/dsp/fxbus.c firmware/src/dsp/master.c firmware/src/dsp/fm6_core.c
   firmware/src/dsp/fm6_vbank.c firmware/src/dsp/eng_fm6.c firmware/src/dsp/eng_noise.c
   firmware/src/dsp/perform.c
   firmware/src/seq/sequencer.c firmware/src/seq/tb3po.c firmware/src/seq/pattern.c
   firmware/src/seq/motion.c firmware/src/app/engine.c
   core/seq/clip_engine.c firmware/src/app/live_bridge.c firmware/src/app/midi_live.c core/midi/midi_in.c
   core/model/song_store.c core/model/undo_stack.c firmware/src/app/song_io.c firmware/src/app/song_templates.c"
# shellcheck disable=SC2086
emcc -O2 -ffp-contract=off -std=gnu99 -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -DX0X_HOST -DCLIP_ENGINE_FS=44100 -DX0X_WEB "-DX0X_VERSION=\"$(printf %s "${X0X_VERSION:-DEV}" | tr a-z A-Z)\"" -I. -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen \
    --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=33554432 -sFILESYSTEM=0 \
    -o build/emu/x0x.wasm web/emu/x0x_web.c $U
cp web/emu/index.html web/emu/worklet.js build/emu/
echo "emu: build/emu ($(wc -c < build/emu/x0x.wasm) B wasm)"
