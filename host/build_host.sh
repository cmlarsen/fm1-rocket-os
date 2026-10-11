#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the X0X host simulator (the whole app on this machine) into build/host/x0x_host.
# Same sources as the firmware, -ffp-contract=off like the device (no fused multiply-add).
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
FEAT=""
if [ "$(uname -s)" = Linux ]; then FEAT="-D_GNU_SOURCE"; fi   # clock_gettime, M_PI under -std=c99 (glibc)
mkdir -p build/gen build/host
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
$CC -O2 -ffp-contract=off -std=c99 $FEAT -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -DX0X_HOST -DCLIP_ENGINE_FS=44100 -I. ${X0X_HOST_DEFS:-} -Ifirmware/src -Ifirmware/src/dsp -Ibuild/gen -o ${X0X_HOST_OUT:-build/host/x0x_host} host/x0x_host.c $U -lm
echo "host: build/host/x0x_host"
