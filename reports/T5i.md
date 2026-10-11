# T5i: FM6 and NOISE as X0X engine parts

Integration lane for T5 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)), run
alongside T3. **No existing part table, stored format or sequencer changed** (the coordinator's call:
`NPARTS` stays 4, the new parts are engine-only), and the block-render hook and `dly_buf` T3 owns were
left alone. Nothing was flashed and no flash was written.

`reports/T5i-review.md` items 1–6 are fixed (item 7, the integration merge, is deliberately **not**
done: T3 merges first). The fixes are at the end, under **Review fixes**.

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

`plat_fm6_vbank()` maps the reserved sector at `0x9F000` for reading in place (device: the XIP window;
host: none). `fm6_vbank_import()` validates it: each of the 32 packed 128-byte records is round-tripped
(`fm6_unpack` → `fm6_pack`) and any field out of range, or blank/erased flash, refuses the whole bank.
On a valid bank the adapter's B1..B32 load it; otherwise a bank slot falls back to a ROM factory voice.
`eng_fm6_load_preset(n)` / `engine_fm6_preset(n)` use the SLOT numbering (0..7 ROM factory, 8 OWN,
9..40 bank). **T5i never writes flash**; the loader CRC port is T5L.

## CPU and `ENG_FM6_VOICES`

Scenario: 909 + 303 A + 303 B playing (909 kick/hats, both 303s on TB-3PO lines), an 8-note FM6 chord
held and NOISE retriggered; `X0X_PROFILE=1`, the host single-core (the 808 idle). Host instructions /
sample (perf doc's measure; a released voice's tail is included) and the stage shares:

| Combo (909 + 303 A + 303 B + …) | instr / sample | FM6 share | NOISE share | est. device load (×2 cycles) |
|---|---:|---:|---:|---:|
| no FM6/NOISE (baseline) | 1595 | 0.0 % | 0.0 % | — |
| FM6 **4** voices + NOISE | 2308 | 21.6 % | 9.5 % | ~85 % at 240 MHz |
| FM6 **6** voices + NOISE | 3021 | 28.9 % | 7.7 % | ~94 % |
| FM6 **8** voices + NOISE | 3349 | 35.3 % | 6.7 % | ~103 % |

`docs/plans/2026-10-04-performance.md` assumes about **2 device cycles per Mac instruction**; at that
factor 8 voices is ~103 % of a 240 MHz block, 6 ~94 % and 4 ~85 %. The review capped
**`ENG_FM6_VOICES` at 4**; **RC1 measures it and may raise it** (the default is a `-D` override). The
FM6 and NOISE shares above are the new PERF buckets (review fix 2): `PR_FM6`/`PR_NOISE`, timed per
block and shown on the PERF screen as a 3×3 table.

## RAM (before / after the follow-up, `make fw` on deltron)

| | `.data+.bss` (of 98,304) | POOL (of 344,064) | pool spare (floor 8,192) |
|---|---:|---:|---:|
| as first delivered (8 voices) | 96,264 | 335,704 | 8,360 |
| follow-up **before** fix 6 (4 voices, NOISE/FM6 bits in `.bss`) | 96,320 | 334,440 | 9,624 |
| follow-up **after** fix 6 (4 voices, moved to the pool) | **96,032** | **334,712** | **9,352** |

Fix 6 moved the NOISE voices (`nz_v`), the FM6 tail (`fm6_tail`) and the FM6 LFO state (`fm6_lfo`)
from `.bss` into the shared `.pool`: **`.bss` −288 B, POOL +272 B** (16 B of alignment). Pool state
now: `fm6_voice[4]`, the 4,100-byte RAM voice bank, a 1 KiB int32 block, plus those three. T3's
`dly_buf` cut frees another ~88 KB.

## Tests

- `make unit-core` — all suites pass, including T5c's FM6/NOISE parity and adapters (unchanged) and
  `unit-fm6bank`: `fm6_vbank_import` accepts a valid bank and refuses erased / blank / corrupt / short
  images, and `eng_fm6_load_preset` falls back to the ROM voices with no bank (and with an empty bank).
  Plain and ASan/UBSan.
- `make scenarios` — every scenario passes. New `t5i_parts` covers the review fixes: a note above 127
  is clamped and its note-off still releases it (no stuck voice); the guard caps FM6 at 2 held voices
  under `load 95`; `engine_all_off` releases a held FM6 chord and a NOISE note; and a transport stop
  (`tap PLAY` twice) releases them, the output decaying to silence (`peak −180 dBFS`). `migrate0102`
  now checks the rev-5 migration on the 0.10.2 fixture (below). `t5i_engines` (the FM6 chord + NOISE
  hit) and its goldens are unchanged.
- `make check` — firmware build, soft-double/size and symbol checks pass.
- `tools/check_loader.sh build/x0x.fwsc` — **the loader is byte-identical to X0X 1.0.5.**
- `make parity` — WASM matches native (audio, screen, lights).

## Review fixes (items 1–6)

1. **`ENG_FM6_VOICES` is 4** (was 8), the review's ceiling; RC1 may raise it.
2. **The overload guard sheds FM6 and NOISE.** While `guard_on`, `eng_fm6_limit_voices(2)` releases the
   oldest *held* voices (they fade, they are not cut) every block, and `eng_noise_set_guard(1)` makes
   the NOISE render skip its impulse density without touching the stored parameter. Added `PR_FM6` and
   `PR_NOISE` PROF buckets (`engine.h`) and timed them in `t5i_render`; the PERF screen's stage table
   is now 3×3 and `ui.c`'s stage-name array is extended (a small `ui.c` edit, noted for the T3 merge).
   Tested with the runner's `load N` (`t5i_parts`).
3. **`note` is clamped to 0..127** in `engine_note_on/off` (and defensively in the C_BON/C_BOFF drain),
   so a note above 127 can never leave a voice stuck. Tested with `note 4 200 …` then its off.
4. **`engine_all_off()`** releases the 303s (`bass303_all_off`), FM6 and NOISE (the one-shot drums need
   nothing). It sets an ISR flag, like the other main-loop→audio paths. It is called on the
   transport-stop transition inside `engine_render`, and is the panic all-notes-off (no panic handler
   exists yet). **T3's `live_bridge_exit` will call it after the merge** — noted for the coordinator.
   Tested: a held FM6 chord + a NOISE note go silent on `alloff` and on a transport stop.
5. **Sound format rev 5.** `PROJ_SND_REV` is 5; `sound_rev5()` (called for `snd_rev < 5`) gives an older
   sound's `T_MIX` 4/5 the other parts' level (the BASS strip, the existing melodic part), centred with
   no sends, and the adapter defaults for `T_RSV` (FM6's macros at their 0 offset, SLOT 0; NOISE at its
   init `{ANLG, 64, 70, 100, 127, 0, 70, 0}`). Tested on the 0.10.2 fixture: `migrate0102` now expects
   the new values and that they survive a save/reboot.
6. **RAM**: see the table above (`nz_v`, `fm6_tail`, `fm6_lfo` into the pool).
7. **Done: `integration` is merged and CHORDS/FX reach FM6/NOISE through the live path** — see
   **Item 7** below.

## Item 7: `integration` merged; CHORDS and FX in LIVE

`integration` (T3 LIVE + the follow-ups) is merged into this branch. The conflicts — `engine.c`,
`Makefile`, `host/build_host.sh`, `tools/build.py`, `web/emu/build.sh` — were resolved by keeping both
sides: `engine_init` has T3's `live_bridge_init()` **and** T5i's `ENG_NPARTS` mixer loop; the build
lists carry both the live_bridge/clip_engine units and the fm6/noise units; `engine.h` has one
`engine_all_off()` declaration; and `engine_render` keeps T3's LIVE branch (T5i's duplicate
`all_off_req` consume was dropped — T3's single `all_off_now`, now extended to release FM6 and NOISE,
does it).

**Audio.** Clip events are applied in T3's ISR-direct `live_apply`, never queued: **CHORDS →
`eng_fm6_note_on/off`** (part 4) and **FX → `eng_noise_note_on/off`** (part 5), with the note's
velocity, and the same `live_tap` event word as the 303s. `live_bridge_play_key` also plays CHORDS/FX
on the PLAY layer, and `live_bridge_exit` calls `engine_all_off()` (after the 303 releases, so their
events stay), so entering or leaving LIVE silences FM6 and NOISE.

**Content (interim, until T6i/T8's starter songs).** One small function, `map_chords_fx()` in
`live_bridge.c`: the **CHORDS** clips are derived from the **BASS** clips (each bass note a chord at
root+12, +19 and +24, velocity 90, the same step and length, ties as in the bass); the three **FX**
clips are generated — **A** off-beat eighths (1 bar, note 60, vel 100, len 1), **B** a 1-bar riser
(16 steps, note 60, vel 20→127, len 1), **C** a sparse 2-bar texture (steps 0/11/22 of 32, note 48,
vel 80, len 4). `tests/host/live_bridge_test.c` asserts the exact CHORDS notes and all three FX
clips.

**Scenario.** `tests/scenarios/live_chordfx.x0x` builds a 303 A line (so a CHORDS clip is derived),
enters LIVE, launches CHORDS A and FX A, and asserts the FM6 (`bon 4 … vel90`) and NOISE
(`bon 5 60 vel100`) events, `liveclip3`/`liveclip4`, and non-silent audio (peak −8.0 dBFS). Its
goldens are `tests/golden/live_chordfx/` (`live_chordfx_events.txt`, `live_chordfx.json`,
`live_chordfx.png`, `audio.json`).

**Re-seeded goldens** (the FX/CHORDS clips are now loaded, so their LIVE LEDs and pad cells change) —
the Opus reviewer should view these at 1:1:

| Golden | What changed |
|---|---|
| `tests/golden/live_chordfx/*` | **new**: the CHORDS/FX pads and the FM6/NOISE event stream |
| `tests/golden/live_leds/live_leds.{png,json}` | keys 24–26 (FX A–C) loaded: dim, not off |
| `tests/golden/live_pads/live_pads.{json}` `live_pads_{playing,stopped}.png` | key 19 (CHORDS A) and 24–26 loaded; the CHORDS/FX pad cells |
| `tests/golden/live_press/live_press.png`, `live_autosave/live_autosave.png`, `live_entry_stop/live_entry_stop.png` | the FX (and CHORDS) pad cells are loaded |

The RAM after the merge: **`.data+.bss` 96,204, POOL 261,296** (T3's `dly_buf` cut and the LIVE
`song`/clip engine in the pool free ~89 KB; pool spare 82,768).

## Files

New: `tests/host/fm6_bank_test.c`, `tests/scenarios/t5i_engines.x0x`,
`tests/scenarios/t5i_parts.x0x`, `tests/scenarios/live_chordfx.x0x`,
`tests/golden/t5i_engines/{ev.txt,audio.json}`, `tests/golden/live_chordfx/`, this report.
Changed: `firmware/src/app/engine.{c,h}` (parts, dispatch, mix strips, params, the guard, the PROF
buckets, the note clamp, `engine_all_off`, and the ISR-direct CHORDS→FM6 / FX→NOISE in `live_apply`),
`firmware/src/app/live_bridge.c` (`map_chords_fx`, the CHORDS/FX PLAY keys, the exit all-off),
`firmware/src/app/x0x.h` (`PROJ_SND_REV 5`), `firmware/src/app/project.c` (`sound_rev5`),
`firmware/src/app/ui.c` (the PERF stage names and grid), `plat.h` / `plat_fm1.c` (`plat_fm6_vbank`),
`host/x0x_host.c` (`plat_fm6_vbank`, `note`, `alloff`, `fm6voices`/`fm6held`/`nzvoices` expects, the
event tap, the profile names), `firmware/src/dsp/eng_fm6.{c,h}` (`eng_fm6_load_preset`,
`eng_fm6_limit_voices`, `eng_fm6_voices_held`, `ENG_FM6_VOICES`, the pool),
`firmware/src/dsp/eng_noise.{c,h}` (`eng_noise_set_guard`, the pool),
`firmware/src/dsp/fm6_vbank.{c,h}` (`fm6_vbank_import`), `firmware/src/dsp/fm6_tables.h` +
`tools/gen_fm6_tables.py` (`#ifndef FS`), the build lists (`tools/build.py`, `host/build_host.sh`,
`web/emu/build.sh`, `Makefile`), `docs/engineering/model.md`, `tests/scenarios/migrate0102.x0x`,
`tests/host/live_bridge_test.c`, and the re-seeded LIVE goldens above.

## For the coordinator

- **T3 is merged.** `engine.c` keeps T3's LIVE branch and calls `all_off_now()` on the transport-stop
  transition; `all_off_now()` now also releases FM6/NOISE. CHORDS/FX arrive through `live_apply`'s
  direct calls, never `engine_note_on` (which queues); the PLAY-layer keys use the queued
  `engine_note_on`. `live_bridge_exit()` calls `engine_all_off()`.
- **`plat.h`** gained `plat_fm6_vbank` (device XIP read, host none) and the `PLAT_FM6_VBANK_*` macros.
- **No stored format changed** (`NPARTS`, `sound_t`, `settings_t`, `pattern_t` all pinned as before);
  the only format-visible change is the sound revision's *content* (migrated on load).
