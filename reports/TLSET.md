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
