# T5i: FM6 and NOISE as X0X engine parts

Integration lane for T5 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)), run
alongside T3. **No existing part table, stored format or sequencer changed** (the coordinator's call:
`NPARTS` stays 4, the new parts are engine-only), and the block-render hook and `dly_buf` T3 owns were
left alone. Nothing was flashed and no flash was written.

## What was built

Two **engine-only parts** after X0X's four: `PART_FM6 = 4` (polyphonic) and `PART_NOISE = 5`, with
`ENG_NPARTS = 6` used inside `engine.c` for the mixer strips, the dispatch and the render. `NPARTS`
stays 4, so the sequencer (`NTRACKS = 4`), the UI, the project format and the host runner are
untouched. `engine_nvoices/nparams/param/voice_name` know `T_RSV` (FM6 = voice 0, NOISE = voice 1) and
`T_MIX` voices 4/5, so defaults, `engine_apply_sound` and undo all carry them.

- **Notes** go through the existing `C_BON` / `C_BOFF` with `a = 4 | 5`, `b = note`; on `C_BON` the `c`
  field is **velocity 0..127** (`a < 4` keeps X0X's `accent | slide`). `engine_note_on/off` queue them.
- **Parameters** go through `engine_set` on `T_RSV` (voice 0 = FM6, voice 1 = NOISE, index 0..7). The
  full table is in [model.md](../docs/engineering/model.md#command-words).
- **Mix strips**: `T_MIX` voices 4/5 (Level, Rev send, Dly send, Pan), like X0X's parts. `render_sub`
  renders FM6 then NOISE on **core 0** (never core 1), mixed through the same glide/pan/send path.
  Idle engines are skipped, so a project without FM6/NOISE stays bit-identical (all X0X scenario
  goldens still pass).

### FM6 presets: the 0x9F000 bank, validated, with a ROM fallback

`plat_fm6_vbank()` (new) maps the reserved sector at `0x9F000` for reading in place (device: the XIP
window; host: none). `fm6_vbank_import()` validates it: each of the 32 packed 128-byte records is
round-tripped (`fm6_unpack` → `fm6_pack`) and any field out of range, or blank/erased flash, refuses
the whole bank. On a valid bank the adapter's B1..B32 load it; otherwise a bank slot falls back to a
ROM factory voice. `eng_fm6_load_preset(n)` / `engine_fm6_preset(n)` use the SLOT numbering (0..7 ROM
factory, 8 OWN, 9..40 bank). **T5i never writes flash**; the loader CRC port is T5L.

## CPU (host estimate) and `ENG_FM6_VOICES`

Scenario: 909 + 303 A + 303 B playing (909 kick/hats, both 303s on TB-3PO lines), an 8-note FM6 chord
held and NOISE retriggered; `X0X_PROFILE=1`, the host single-core (the 808 idle). Host instructions /
sample (stable, the perf doc's measure) and the render's share of real time:

| Combo (909 + 303 A + 303 B + …) | instr / sample (mean) | worst block | render % of real time |
|---|---:|---:|---:|
| no FM6/NOISE (baseline) | 1594 | 1752 | 0.3 % |
| FM6 **4** voices + NOISE | 2301 | 2686 | 0.4 % |
| FM6 **6** voices + NOISE | 2549 | 3015 | 0.5 % |
| FM6 **8** voices + NOISE | 2804 | 3342 | 0.5 % |

So one FM6 voice is about 126 host instructions / sample and NOISE about 200. Against the FM-1's
budget (240 MHz / 44.1 kHz = **5442 cycles / sample**), 8 voices is **51.5 %** — the largest count
under 60 % of the block. `ENG_FM6_VOICES` is set to **8**.

**The caution:** `docs/plans/2026-10-04-performance.md` assumes about **2 device cycles per Mac
instruction** (the Mac vectorises). At that factor, 8 voices is ~103 % of a 240 MHz block and 6 is
~94 %; **RC1 must measure the device.** `ENG_FM6_VOICES` is a `-D` override, so 6 (Felucca's
`FM6_POLY`) or 4 is a one-line fallback if RC1 shows overload.

## RAM (before / after, `make fw` on deltron)

| | `.data+.bss` (of 98,304) | POOL (of 344,064) | pool spare (floor 8,192) |
|---|---:|---:|---:|
| before | 95,048 | 328,052 | 16,012 |
| after | **96,264** | **335,704** | **8,360** |

New pool state: the FM6 voice array `fm6_voice[8]` (in `eng_fm6.c`, the RAM plan), the 4,100-byte RAM
voice bank, and a 1 KiB int32 block the adapters render into. New `.bss`: the FM6 core's bus, the
NOISE state and the mix strips. The ramo plan's `dly_buf` cut (T3) frees another ~88 KB of pool, so
the headroom above is the standalone figure.

## Tests

- `make unit-core` — all suites pass, including T5c's FM6/NOISE parity and adapters (unchanged) and the
  new `unit-fm6bank`: `fm6_vbank_import` accepts a valid bank and refuses erased / blank / corrupt /
  short images, and `eng_fm6_load_preset` falls back to the ROM voices with no bank (and with an empty
  bank), loading the bank voice when it is there. Plain and ASan/UBSan.
- `make scenarios` — every scenario passes, including the new `t5i_engines` (a 4-note FM6 chord and a
  NOISE hit through the command path). New goldens: `tests/golden/t5i_engines/ev.txt` (the command
  stream) and `audio.json` (`t5i.wav`: 110,848 frames, peak 0.40 = **-8.0 dBFS**, rms 0.061 — not
  silent, no clipping). **No PNG** changed or was added.
- `make check` — firmware build, soft-double/size and symbol checks pass.
- `tools/check_loader.sh build/x0x.fwsc` — **the loader is byte-identical to X0X 1.0.5.**
- `make parity` — WASM matches native (audio, screen, lights), the new sources in the WASM build.

## Files

New: `tests/host/fm6_bank_test.c`, `tests/scenarios/t5i_engines.x0x`,
`tests/golden/t5i_engines/{ev.txt,audio.json}`, this report.
Changed: `firmware/src/app/engine.{c,h}` (parts, dispatch, mix strips, params, NFC), `plat.h` /
`plat_fm1.c` (`plat_fm6_vbank`), `host/x0x_host.c` (`plat_fm6_vbank`, the `note` command, the event
tap), `firmware/src/dsp/fm6_vbank.{c,h}` (`fm6_vbank_import`), `firmware/src/dsp/eng_fm6.{c,h}`
(`eng_fm6_load_preset`, `ENG_FM6_VOICES`, the voice array in the pool), `firmware/src/dsp/fm6_tables.h`
+ `tools/gen_fm6_tables.py` (`#ifndef FS`, so `engine.c` can include both), the build lists
(`tools/build.py`, `host/build_host.sh`, `web/emu/build.sh`, `Makefile`), `docs/engineering/model.md`.

## For the coordinator (merge)

- **T3.** Shared file is `engine.c`. T5i touches the parts, `drain`, `apply_param`, `render_sub`,
  `engine_render`'s meter tail and `engine_init`; T3's block-render hook and the `dly_buf` cut are
  untouched. The clip engine's CHORDS/FX events should call `engine_note_on(PART_FM6, …)` /
  `engine_note_off` (or `engine_set(T_RSV, …)`), not `engine_bass_on`.
- **`plat.h`** gained `plat_fm6_vbank` (device XIP read, host none) and the `PLAT_FM6_VBANK_*` macros.
- **No stored format changed** (`NPARTS`, `sound_t`, `settings_t`, `pattern_t` all pinned as before).
