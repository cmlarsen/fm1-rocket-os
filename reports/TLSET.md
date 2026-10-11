# TLSET: LIVE SETUP report

EDIT in LIVE opens LIVE SETUP (`firmware/src/app/ui_live_setup.c`). All rows, the EXT chip, help and docs are in. Owner answers applied: the layer toggle reads SEL in new strings; the key encoding is one byte per key.

## Rows and tests

| Row / rule | Behaviour | Test |
|---|---|---|
| EDIT opens SETUP, EDIT or HOME leaves | Take card is now the Take row | `tlset_rows.x0x`, `tlset_setup.x0x` |
| Assign keys | Hold a key, PRESETS cycles all 39 targets, release commits, toast "A4 -> CHORDS B" | `tlset_assign.x0x`, golden `tlset_live7_assign.png` |
| Key encoding | 0-26 clips, 27-31 role stops, 32-37 FX, 38 live note, 255 unassigned; model v2, no struct growth | `tests/host/song_store_test.c` (v1 fixture `tests/fixtures/tlset-v1-song.bin` round trip) |
| Live note | Plays the focused role at the key's pitch in PADS, released on layer change, leave and close | `tlset_assign.x0x` |
| Assigned FX | Tap latches, hold is momentary, release cleanup on layer change and leaving LIVE | `tlset_assign.x0x` |
| Launch default | `song.quant_default`; applies to new clips (`clip_open`, `clip_record_open`) and to PC launches (`pc_quant`) | `tlset_rows.x0x` |
| Stage lock | Saved with the song, shown in the row, and enforced on both launch-mode clear paths in STUDIO (HOME + pad, SEL hold) while the transport plays; toast LOCKED; take clear unaffected | `tlset_lock.x0x` (new in this pass), `tlset_rows.x0x` |
| External keys channel | Omni or 1-16, held notes released on change | `tlset_rows.x0x`, `midi_live` unit |
| Program Change launch | Off or On | `tlset_rows.x0x`, `midi_live` unit |
| Clock in | Same value as GLOBAL CLOCK IN (`proj.set.rsv[1]`), not a copy | `tlset_rows.x0x` |
| Take | Existing take card, T9i behaviour kept | `t9i_take_screens.x0x` |
| Dirty and autosave | Edits mark the song dirty; header dot shows it; autosave only stopped and idle | `tlset_rows.x0x`, `live_autosave` |
| EXT chip | Header chip and `midi_live_bpm_x10()` readout while following external clock | goldens `tlset_ext_pads.png`, `tlset_ext_setup.png` |
| Help | Setup card, LIVE cheat sheet EDIT row, SEL wording | golden `tlset_setup_help.png`, `check_help` |
| Docs | interaction-model.md, screens.md, MANUAL.md, model.md | review of `c340664` |

## Gap fixed in this pass

Stage lock was stored and shown but nothing enforced it. `ui_studio.c` now refuses both clear gestures while playing with the lock on and shows a LOCKED toast. `tlset_lock.x0x` failed before the change (clip cleared under lock) and passes after.

## Ladder (committed tree)

| Step | Result |
|---|---|
| `make unit unit-core scenarios screens-lint fuzz-ui check` | pass (200 x 2000 and 20 x 500 fuzz seeds) |
| `make fw`, `make fw-debug` | pass, build checks ok |
| `tools/check_loader.sh build/x0x.fwsc` | ok, X0X 1.0.5 loader |
| `tools/check_loader.sh build/debug/x0x.fwsc` | ok, X0X 1.0.5 loader |

## Pool numbers

| Build | Image | RAM .data+.bss | Pool used / total | Pool spare |
|---|---|---|---|---|
| release | 549,704 B | 72,896 / 98,304 | 301,940 / 344,064 | **42,124 B** |
| debug | 554,264 B | 73,524 / 98,304 | 301,940 / 344,064 | **42,124 B** |

Both clear the 8,192 B rule. No hardware was flashed.

## r1: Opus review findings

Fixes are in `2330cfb` and the test follow-up. Each new scenario failed on `ca33861` before the fix (the take scenarios are pins and pass on both).

| # | Finding | Fix | Test |
|---|---|---|---|
| 1 | Blocker: a momentary FX held in the FX layer stuck on when EDIT opened SETUP | `live_fx_reset_ui()` now releases every non-latched held FX key (`live_fx_release_held()`, shared with the FX-button release). Mapped and FX-layer key releases are handled before `live_setup_key()` | `tlset_fx_edit.x0x` (before: `fxactive2` stayed 1) |
| 2 | LIVE assignments remapped STUDIO's keys | STUDIO uses the fixed zone map (`st_key_clip`, key k = clip k) for key handling and LEDs; only LIVE PADS reads `key_clip` | `tlset_studio_keys.x0x` (before: the clear hit the wrong clip) |
| 3 | An assigned FX latched when an unrelated clip key with the same index was released | `fx_owner_key[6]` records the engaging key; only that key's release ends or latches the effect | `tlset_fx_owner.x0x` (before: `fxlatch0` 1) |
| 4 | A refused SEL hold under stage lock launched the clip on release, and the toast refired each tick | A refusal sets `btn_used` for SEL and the tick will not retry while it is set, so the toast shows once and the release is not a tap | `tlset_lock_sel.x0x` (before: the release launched clip 1). The scenario is its own file, not an addition to `tlset_lock`, because that scenario's cursor clip is already playing and a launch there is invisible |
| 5 | No take scenario with assigned keys | Host expect `fxactiveN` added. Stop and FX assignments recorded and replayed event-exactly; the live note is recorded and pinned separately | `tlset_take.x0x` (takedump and eventscmp, goldens), `tlset_take_note.x0x` (takedump golden) |
| 6 | Assign had no None, and tapping an unassigned key assigned DRUMS A | PRESETS cycle has 40 states, the last is None (255). Release commits only when the target changed from the start, so no toast and no dirty flag otherwise | `tlset_assign_none.x0x` (before: `songdirty` 1 after a plain tap) |

NITs done: key names in the toast and title follow the pitch played (41+k); "EDIT tap" left the HOME combo sheet (the EDIT card covers it) and the card reads "EDIT: setup / EDIT again: done"; LIVE LEDs dim-light assigned stop, FX and note keys and light a latched or held assigned FX; the channel compare happens after clamping and the indentation is fixed; LIVE 9 rows moved to an 18 px pitch so the Take row clears PERFORM; `song_migrate()` split out of the store load with a unit test that rejects v1 bytes 27-38 (`tests/host/song_store_test.c`).

Goldens changed (GOLDEN_UPDATE=1, viewed at 1:1): `help_sheet_live` (EDIT row gone), `tlset_live7_assign` (key A3), `tlset_live9_setup` and `tlset_ext_setup` (row pitch), `tlset_setup_help` (card line); new `tlset_take` and `tlset_take_note` logs. SEL releasing held notes on layer change (architecture.md) was what moved `live_hold` and `snd_hang_scl` in `c340664`.

Known, not fixed here (T9i's): a take replay starts a live note 256 samples early on the PLAY layer as well as on assigned keys (rec 384 vs replay 128 in `tlset_take_note`), so that scenario does not eventscmp.

Ladder on the committed tree: `make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` pass; `tools/check_loader.sh` ok on `build/x0x.fwsc` and `build/debug/x0x.fwsc`. Pool spare is 42,124 B on both builds (301,940 / 344,064 used); images 549,800 B release and 554,344 B debug. No hardware was flashed.
