# T7a: STUDIO song home and clip CRUD

Integration lane for [T7a](../docs/engineering/tasks/T7a.md)
([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)), in parallel with T8 (the
songs UI, new files). **The song is the edited object from now on.** No hardware was flashed
(`make fw`/`make check` only build); `tools/check_loader.sh` confirms the package carries X0X 1.0.5's
loader.

## The model change

- **The song home is the default view.** A new `V_STUDIO` (appended to `ui.c`'s view enum, so every
  existing view number is unchanged) is where boot lands; `V_CLIPSET` is STUDIO 5. The song card,
  the role × clip grid with its cursor and the cursor clip's settings (K1–K4) live in the new
  `firmware/src/app/ui_studio.c` (included by `ui.c` like `ui_live.c`).
- **The clip engine is the only audio source in every normal view** (`live_bridge_init` sets
  `g_active = 1`), so tweaking a sound/FX/MIX/GLOBAL page never stops the clips. `live_bridge_enter`
  no longer re-inits the engine, so clips keep playing across the STUDIO ↔ LIVE switch (decision 7).
- **The pattern-hash remap is gone.** `song_io_boot` adopts slot 0 as it is; the project only seeds
  the song once, when no stored song exists. `project_save` no longer re-derives the clips, and
  `live_bridge_enter` no longer rebuilds on a hash change. `live_bridge_song_baseline(0)` marks a
  loaded song as "nothing to re-derive".
- **The song's `bpm_x10` is authoritative.** `live_bridge_sync_tempo()` copies it into `seq.bpm`, so
  X0X's sequencer follows the song; the GLOBAL tempo and the song-home tempo write the song.
- **Classic patterns.** X0X's editors left the normal button paths. `GLOBAL → Classic patterns`
  (`ACT_CLASSIC`) hands the audio to X0X's sequencer and shows a CLASSIC chip in the header; the way
  back (GLOBAL → Song home, or HOME) stops that sequencer and returns the audio to the clip engine.
  See [interaction-model.md](../docs/product/interaction-model.md#classic-patterns).

## Build (T7a)

| # | what |
|---|---|
| 1 | **STUDIO 1, the song home.** Header (STUDIO chip, SONG/COPY, transport, song BPM, bar.beat, dirty dot); a song card; the 5-row role × clip grid in role colours with the empty/loaded/playing/queued states and the cursor; the K1–K4 strip. SELECT moves between roles, ALGORITHM along a row, SEL launches the cursor clip, the keys mirror the grid. |
| 2 | **STUDIO 2, the empty slot.** An empty cursor shows the "DRUM A EMPTY / play it in, or step it in" card. SEQ creates a clip (`used`, 16 steps, Loop, Bar) and opens STUDIO 5; REC shows the T7b hint. |
| 3 | **STUDIO 5, clip settings.** `V_CLIPSET`: LENGTH (1–64), MODE (Loop/Once), LAUNCH (bar/beat/end of clip) on K1–K3 and SWING on K4, with the design's mode and launch chips. Every change marks the song dirty. |
| 4 | **STUDIO 9, clear.** HOME + hold a pad, or SEL held on the cursor, for 1.5 s: a ring names the clip and clears it with a `song_io` undo entry. Releasing early cancels. |
| 5 | **STUDIO 14, copy.** Hold a loaded pad; empty same-role slots blink slowly; tap one to copy (undoable). |
| 6 | **STUDIO 10, undo/redo.** HOME+REC / HOME+PLAY; a toast names the action ("UNDO cleared LEAD B"), and "can't undo" is its own toast. The clip's name comes from `song_io_undo_clip()`. |
| 7 | **LIVE uses the same song.** Entering LIVE keeps what is playing; leaving to the song home keeps it playing. Entering/leaving from a classic view swaps the audio back. |

The song-home buttons keep the one grammar: EDIT opens the sound page for the cursor's role
(audition), FX/LFO/GLO keep their X0X pages, PLAY is the clip transport everywhere normal.

## Tests

- **Unit (`make unit-core`).** `song_io_test` gained `test_clip_edits` (create/length/mode/quant,
  clear + undo + redo, copy + undo) and `test_project_change_kept` (a changed project no longer
  rebuilds a stored song). `live_bridge_test` and the store tests still pass.
- **New scenarios with goldens** (`tests/scenarios/studio_*.x0x`, 1:1 PNG + JSON):

  | scenario | covers |
  |---|---|
  | `studio_home` | boot to the song home; SELECT/ALGORITHM move the cursor; SEL launches |
  | `studio_empty` | the empty-slot card; SEQ creates the clip and opens the settings |
  | `studio_settings` | length 16→32, mode Loop→Once, launch Bar→End |
  | `studio_clear` | early release cancels; the ring clears; undo; redo; SEL-hold clears |
  | `studio_copy` | hold a pad, tap an empty same-role slot, undo |
  | `studio_live` | a clip playing across STUDIO → LIVE → STUDIO |
  | `studio_persist` | a clip set up, autosaved, rebooted, back with its settings |

- **`make scenarios`**: all 53 pass. Existing scenarios that boot into the 909/X0X pages now take
  one helper step (`tap GLO; tap SEL`) into Classic; the LIVE scenarios seed the song from the
  patterns they build (`songseed`, a test-only runner command) before entering LIVE; scenarios that
  reboot re-enter Classic after it. `t6i_remap` is deleted with the remap it tested.
- **Goldens re-seeded** (view pixels changed): `groove`, `harness`, `live_autosave`, `live_chordfx`,
  `live_entry_stop`, `live_hold`, `live_leds`, `live_pads`, `live_play`, `live_press`, `patterns`,
  `song`, `t5i_engines`, and the seven new `studio_*` directories.
- **`make screens-lint`**: 0 findings (the tour carries STUDIO 1, 2, 5 and 9).
- `make parity`, `make check fw` and `tools/check_loader.sh build/x0x.fwsc` all pass.

### New PNGs (every one, for the reviewer)

- `tests/golden/studio_home/`: `studio_home_grid.png`, `studio_home_playing.png`.
- `tests/golden/studio_empty/`: `studio_empty.png`, `studio_clipset.png`, `studio_home_clip.png`.
- `tests/golden/studio_settings/`: `studio_settings.png`, `studio_settings_home.png`.
- `tests/golden/studio_clear/`: `studio_clear_ring.png`, `studio_undo.png`, `studio_redo.png`.
- `tests/golden/studio_copy/`: `studio_copy.png`, `studio_copy_undone.png`.
- `tests/golden/studio_live/`: `studio_live_studio.png`, `studio_live_live.png`, `studio_live_back.png`.
- `tests/golden/studio_persist/`: `studio_persist_before.png`, `studio_persist_after.png`.

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

| | value |
|---|---|
| app image `x0x.bin` | **512,736 B** |
| RAM `.data+.bss` | **95,816 B** of 98,304 (2,488 B spare) |
| POOL | **292,132 B** of 344,064 |
| package `x0x.fwsc` | 609,680 B, identity FM-1_900 |
| checks | register access hal/-only; no soft-double; no heap / 64-bit division; flash guard ok; no sample writers |

## Limits and open items

- **The STUDIO 1 mock sits in the framework's 20 px header / 138 px main / 82 px knob bands**, not
  the design's 24/140/48: the card is compact (30 px) and the grid rows 18 px so the card, the grid,
  the cursor and the K1–K4 cards all fit the 240×240 at 1:1. Recorded for the owner.
- **T7b** provides the clip editor (NOTES/DRUM, MOTION) and REC; SEQ opens the settings page until
  then, and REC shows a hint.
- **Classic entry/exit** uses the GLOBAL list; the coordinator's "HOME back" was adapted because
  HOME is load-bearing in X0X's own navigation (the tour and the prob/trackmute scenarios rely on
  it). The classic pages are otherwise byte-for-byte the X0X ones.
- The one-time project → song seed still maps CHORDS from the BASS clips and generates the FX clips
  (T5i's interim content); T8's Starter song will replace it.
