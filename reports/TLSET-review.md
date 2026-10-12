# TLSET review (Opus, 2026-10-11, on ca33861)

Host build, unit-store/songio/midilive and all scenarios passed. Fixed in TLSET r1 (2330cfb, 2642880); each finding below has a scenario that failed on ca33861.

## Blocker
1. A momentary FX held in the FX layer stayed on when EDIT opened SETUP: `live_setup_enter()` reset the FX UI state without `engine_perform_release()`, and `live_setup_key()` swallowed the key release. A regression from integration. Test: `tlset_fx_edit`.

## Should-fix
2. LIVE assignments remapped STUDIO's keys (`ui_studio.c` read `live_bridge_key_clip()`). STUDIO now uses the fixed zone map. Test: `tlset_studio_keys`.
3. An assigned FX's latch leaked through the FX-layer path, which treated key index 0–5 as an FX index. A per-FX owner key now decides which release ends it. Test: `tlset_fx_owner`.
4. A stage-locked SEL hold launched the clip on release, and the LOCKED toast refired every tick. Test: `tlset_lock_sel`.
5. No test showed assigned keys recorded into a take. Tests: `tlset_take`, `tlset_take_note`. A 256-sample replay offset on live notes was a T9i issue, fixed in TPOL.
6. A key could not be set back to unassigned, and tapping an unassigned key assigned DRUMS A. Test: `tlset_assign_none`.

## Nits (all done in r1)
- The toast named the wrong pitch.
- The cheat sheet listed "EDIT tap" under the HOME combos.
- The EDIT card wording.
- LEDs for keys assigned to stops, FX and live notes.
- The channel clamp compare.
- Indentation.
- A test for the v1 migration rejecting bytes 27–38.
- Space between LIVE 9's Take row and PERFORM.

## Verified OK
- The validator rejects 39–254.
- The v1 migration round-trips.
- No flash writes in LIVE.
- The MIDI rows reach `midi_live` on the main loop.
- Clock in is the same GLOBAL value.
- The EXT chip uses `midi_live_ext()`/`midi_live_bpm_x10()`.
- No ISR changes.
