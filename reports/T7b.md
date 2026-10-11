# T7b: clip editors and recording into clips

Integration lane for [T7b](../docs/engineering/tasks/T7b.md)
([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)), after T7a. No hardware was
flashed (`make fw`/`make check` only build); `tools/check_loader.sh` confirms the package carries
X0X 1.0.5's loader. T10i and T8i ran in parallel: this lane stays out of `ui_live.c`, the master
path in `engine.c`, and the songs UI.

## What it adds

SEQ on the song home now opens the **clip editor** for the clip under the cursor (creating an empty
slot first, 16 steps / Loop / Bar), and SEQ **cycles the pages**: NOTES (or the DRUM grid) →
MOTION → SETTINGS → back to the song home. HOME leaves from any page. The editors live in the new
`firmware/src/app/ui_clip.c/.h`; `ui_studio.c` only enters and leaves them; the song edits all go
through the new `song_io` entry points.

| # | built |
|---|---|
| 1 | **SEQ cycles NOTES/DRUM → MOTION → SETTINGS → the song home**; SEQ on an empty slot creates the clip and opens NOTES or DRUM. |
| 2 | **STUDIO 4, the drum grid.** 11 lanes × the clip length; the white keys are steps 1–16 of the visible page, the black keys pick the lane (the 909 order BD SD LT MT HT RS CP CH OH CR RD); OCT± pages the steps for clips longer than 16; a white key toggles the step on the lane, HOME + a white key toggles the accent; the playhead shows while playing. |
| 3 | **STUDIO 3, notes.** A piano roll with a step cursor (ALGORITHM, or the STEP card on K1) and the STEP / NOTE / VEL / LEN cards on K1–K4. A key press adds a note at the cursor (length 1 step); holding several keys adds a chord up to `CLIP_MAX_NOTES`; HOME + a key removes that note; OCT± shifts the octave. |
| 4 | **STUDIO 15, motion.** Two lanes a clip. Turning K1–K4 while a pass records writes the value at the playhead step for the param that knob controls (FILTER / SPACE / ECHO / DRIVE); the first two distinct params claim lane 0 and lane 1, a third stays off with "2 lanes max"; the page draws each lane's curve; HOME + a knob clears its lane. The lanes play back through the clip engine's motion events. |
| 5 | **REC.** REC on an empty slot arms a pass from the next bar (starting the transport if it is stopped); the keys record into a new clip until REC is pressed again, which closes the loop at the next bar, sets the length to the recorded bar count (1, 2 or 4, rounded up) and starts the clip looping. REC on a loaded clip overdubs. Notes quantize to the 16th grid. |
| 6 | **Undo.** Each editor session and each record pass is one `song_io` undo entry (the first mutation of a session pushes the clip as it was; `song_io_edit_begin`/`_end`). |
| 7 | **ISR safety.** The clip engine keeps a per-role copy of the playing clip (`clip_t play[NROLES]`) and the audio ISR reads only that: a reload command (posted after an edit), a launch and the loop point are the double-buffer swaps at a step boundary, so the ISR never reads a half-written `clip_t`. |

The clip engine's copy is `play[NROLES]` (2,640 B in the `.pool`); the architecture RAM table is
updated. On the device the pool grows to **294,772 B of 344,064** (from 292,132), `.data+.bss` is
**95,924 of 98,304**.

## Tests

- **Scenarios with goldens** (1:1 PNG + JSON, and the engine event log where it matters):

  | scenario | covers |
  |---|---|
  | `studio_clipdrums` | a drum grid edit (lanes, steps, accent) with the edited hits played back as `drum` events |
  | `studio_clipnotes` | four notes stepped in plus a chord, and HOME + a key removing one |
  | `studio_cliplong` | a two-bar clip paged with OCT+ / OCT− |
  | `studio_clipmotion` | motion recorded on two lanes, a third refused, played back as `param` events |
  | `studio_cliprec` | REC on an empty slot for 1 bar: the loop closes at 16 steps and plays |
  | `studio_clipoverdub` | REC on a loaded clip overdubs; the length is untouched |
  | `studio_clipundorec` | HOME+REC undoes the whole record pass |
  | `studio_clipliveedit` | a hit added while the clip plays; the clip keeps looping and the edit lands at the reload |

- **Unit (`make unit-core`).** `clip_engine_test` gained `edit_while_playing` (edit the song's clip
  mid-play, `reload`, and the new hit fires at the next step). `song_io_test` gained
  `test_clip_editor_ops` (drum set/accent, note add/chord/dedupe/remove, motion lanes with the
  two-lane cap, and one undo entry per session).
- **`make scenarios`**: all 73 pass.
- **`make screens-lint`**: 0 findings; the tour now carries STUDIO 3, 4 and 15.
- **`make parity`**, **`make check fw`** and `tools/check_loader.sh` all pass.

### New PNGs (every one, for the reviewer)

- `tests/golden/studio_clipdrums/`: `studio_clipdrums_grid.png`, `studio_clipdrums_playing.png`.
- `tests/golden/studio_clipnotes/`: `studio_clipnotes.png`.
- `tests/golden/studio_cliplong/`: `studio_cliplong.png`.
- `tests/golden/studio_clipmotion/`: `studio_clipmotion.png`.
- `tests/golden/studio_cliprec/`: `studio_cliprec.png`.
- `tests/golden/studio_clipoverdub/`: `studio_clipoverdub.png`.
- `tests/golden/studio_clipundorec/`: `studio_clipundorec.png`.
- `tests/golden/studio_clipliveedit/`: `studio_clipliveedit.png`.

### Re-seeded goldens and why

- **`studio_empty`, `studio_settings`, `studio_card`, `studio_persist`** JSON `transport.sample`:
  SEQ now opens the editor, so these scenarios take two more SEQ taps to reach the settings page.
  The PNGs are unchanged.
- **`clip_engine`**: the new `edit_while_playing` case golden only; the existing cases are byte-identical.

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

| | value |
|---|---|
| app image `x0x.bin` | **523,456 B** |
| RAM `.data+.bss` | **95,924 B** of 98,304 |
| POOL | **294,772 B** of 344,064 |
| package `x0x.fwsc` | 609,680 B, identity FM-1_900 |
| checks | register access hal/-only; no soft-double; no heap / 64-bit division; flash guard ok; no sample writers |

## Limits and open items

- **Layout.** The editors use the framework's 20 px header / 138 px main / 82 px knob bands. The
  drum grid is 11 rows of 12 px and the notes roll 12 rows of 9 px; the motion page is two 48 px
  lane cards. The noise (a drum or notes clip's page beyond its length) stays blank.
- **Drum recording is step entry only.** REC on a DRUMS clip arms and closes like a melodic pass but
  records no hits and leaves the length alone; the drum grid's step and accent toggles are the way
  to build it. Playing hits in as a drum take is left for later.
- **Motion params.** The motion page's four knobs are the LIVE performance params (FILTER, SPACE,
  ECHO, DRIVE); a clip has two lanes, the model's cap.
- **Edits while playing** reach the audio at the next `ui_frame`: the main loop writes the playing
  copy (masked), so the change is heard almost immediately; a staged launch takes it at its boundary.
- The one-time project → song seed still maps CHORDS from the BASS clips and generates the FX clips
  (T5i's interim content); T8's Starter song will replace it.

## Follow-up: review fixes (reports/T7b-review.md)

Opus's review asked for changes; items 1–4 are fixed, each with a failing-then-passing test. Item 5
(the T10i merge) is the coordinator's.

### 1. The main loop owns the clip snapshot

The ISR used to copy `song->clip[]` into `play[]` at a launch and a loop, so a mid-write `clip_t`
could be read. Now **the audio ISR never reads `song->clip[]`**:

- `clip_engine_t` has `play[NROLES]` (the role's playing copy) and `next[NROLES]` /
  `next_set`/`next_clip` (a launch target the main loop stages before posting the command).
- Every edit calls `song_io`'s `synced()` → `live_bridge_clip_changed()` →
  `clip_engine_clip_changed()`, which copies the song's clip into `play[]` (when it is the role's
  playing clip) and `next[]` (when a launch of it is staged) **with the audio IRQs masked**
  (`engine_crit_enter` → `plat_crit_enter`, X0X's own `irq_save`/`irq_restore`).
- A launch stages the target's copy in the main loop (masked) before posting; the ISR swaps
  `next → play` at the step boundary (`apply_role`). The loop point and `consume_cmds` no longer
  copy from the song (the old `CLIP_CMD_RELOAD` is gone).
- Tests: `clip_engine_test` keeps `edit_while_playing` and gains **`edit_every_step`** (toggle every
  step of the playing clip in turn, each with its own snapshot).

Consequence, recorded: with one staged copy per role, a *burst* of launches for one role collapses
to the last (only the newest has a copy). `ring_many_launches` is re-seeded for that (63 → 50
events). The UI posts one launch per key press, so this only shows under the synthetic burst.

### 2. Recording

- **Pre-roll keys audition only** (`clip_handle_key`): before `rec.start` a key sounds but does not
  step-enter.
- **Overdub follows the clip's own playhead** (`cl_rec_step` uses `live_bridge_clip_step`), not
  `(global-start) % len`.
- **No gap after close** (`cl_rec_commit`): the length is set and the clip is `launch_now`'d at the
  boundary.
- **Motion in a new-clip pass** uses `cl_rec_step()` (the pass step), not the note cursor.
- **Real note lengths**: note-on to note-off, in steps, min 1; a note still held when the loop
  closes ends at the loop end (`cl_rec_note_on`/`_off`, `cl_rec_commit`).
- **Quantize rounds to the nearest 16th** (`cl_rec_quant`, `step_phase`); a note in the last
  half-step wraps.
- **Drum REC** (`cl_rec_drum`): on a DRUMS clip the white keys 1–11 trigger and record lanes 1–11,
  HOME accents the hit. `screens.md` STUDIO 4 says so.
- **Undo of REC on an empty slot removes the clip** (`song_io_clip_create_undoable`): the creation is
  in the pass's undo entry, so undo leaves the slot empty and stops the role.
- Tests: `studio_cliprec` (pre-roll, real length, no gap), `studio_clipoverdub` (playhead overdub),
  `studio_clipmotion` (motion at the pass step), `studio_clipdrumrec` (finger drumming),
  `studio_clipundorec` (used 0 + stopped), and `song_io_test`'s `test_clip_create_undo`.

### 3. The notes roll

`cl.base` is the roll's bottom row; the keys play `cl.base + (k-7)`, so the visible range follows the
octave the keys play; `cl_base_for_clip` scrolls to the clip's note range on open; OCT± moves the
window and the keys together. The page also draws the design's step strip, the note-length bars (a
note's width is its length) and "bar N of M". `studio_clipnotes` shows all of it.

### 4. Visuals

- `studio_clipdrums_playing.png` shows the **drum grid with the playhead** (the scenario reopens the
  grid while the clip plays), not the song home.
- The drum grid is **14 px rows** (`cl_draw_drums`), 9 lanes visible with the grid scrolled to the
  selected lane (11 lanes no longer fit 138 px at 14 px).
- The motion page draws the design's **page tabs** ("1 NOTES / 2 MOTION / 3 SETTINGS").

### Re-seeded goldens and why

- `studio_clipdrums`, `studio_clipliveedit`, `studio_cliplong`, `studio_clipmotion`,
  `studio_clipnotes`, `studio_clipoverdub`, `studio_cliprec`, `studio_clipundorec`: the recorder,
  the notes roll and the 14 px drum grid changed the screens and the event streams.
- `studio_clipdrumrec` (new): a goldens directory with the drum-REC proof.
- `tests/golden/clip_engine/`: `edit_every_step` (new) and `edit_while_playing` / `ring_many_launches`
  (the command renamed, the burst collapse).
- The T7a scenarios (`studio_clear`, `studio_copy`, `studio_seltap`) keep their goldens: SEQ on an
  empty slot still creates the clip outside the undo entry, so only REC-on-empty is undoable.

### Device build (after the fixes)

| | value |
|---|---|
| app image `x0x.bin` | **525,456 B** |
| RAM `.data+.bss` | **96,068 B** of 98,304 |
| POOL | **297,424 B** of 344,064 (`play[5]` + `next[5]` = 5,280 B) |
| package `x0x.fwsc` | 609,680 B, identity FM-1_900 |

`make unit-core scenarios screens-lint parity check fw` and `tools/check_loader.sh` all pass.

## Item 5: the integration merge (T10i, TRAM, T8i, T11-era fixes)

Merged `integration` (T10i punch FX + role mutes, TRAM's shared song scratch, T8i's SONGS, the
T11-era fixes) into the branch. Conflicts and how they were resolved:

- **`clip_engine.c/.h`** — kept T10i's `CLIP_CMD_MUTE` / `CLIP_CMD_MUTE_CLEAR` (their own enum
  values, ordered through the ring after any pending mute) *and* the T7b staged model
  (`play[]`/`next[]` + `clip_engine_clip_changed`). The old `CLIP_CMD_RELOAD` is gone (checked: no
  reference remains). The ISR mute path only sets `muted[]` and releases the role's notes; it never
  touches `song->clip[]` — `step_events` reads `play[role]` and returns early when muted.
- **`engine.c`** — kept the T7b `engine_crit_enter/exit` (wrapping `plat_crit_*`) alongside T10i's
  punch-FX `engine_perform_*` block.
- **`live_bridge.c/.h`** — kept T7b's `live_bridge_clip_changed` / position queries and T10i's
  `live_bridge_grid` (it reads the same transport: `elapsed`/`step_*`/`global_step`) and the role
  mutes. `live_bridge_song_reload` re-inits under the crit mask and clears the song's stored mutes.
- **`ui.c`** — the view enum keeps both T8i's `V_SONGS` and T7b's `V_CLIPEDIT`/`V_CLIPMOTION`
  (`V_SOUND`, `V_CLIPEDIT`=13, `V_CLIPMOTION`=14, `V_SONGS`=15); the SONGS block and the
  `ui_clip.h` include both stay. `input()`/`ui_frame()`/`leds()` return to the SONGS view before the
  editor paths, so they do not overlap.
- **`song_io.*`** — T8i's songs ops and TRAM's `song_store_scratch_acquire/release` are kept; the
  T7b undoable clip create copies a `clip_t` and never takes the scratch, so it cannot collide with a
  songs operation holding it.
- **`song_io_test.c`** — kept the T7b stubs plus T8i's `live_bridge_transport_playing`.

### Re-seeded goldens and why (combination only)

- `songs_list`, `songs_edit`, `songs_delete`, `songs_redo`: the SONGS view's number in the JSON dump
  is 15 now (T7b's editor views sit before it), nothing else.
- `studio_clipdrums`, `studio_clipliveedit`, `studio_cliplong`, `studio_clipmotion`,
  `studio_clipnotes`, `studio_clipoverdub`, `studio_cliprec`, `studio_clipundorec`,
  `studio_clipdrumrec`: T8i's `dump_json` adds the `songs` block to every dump; these goldens predate
  the merge. PNGs are unchanged.

### Device build (merged, on deltron.local)

| | `make fw` | `make fw-debug` (X0X_DEBUG=1) |
|---|---|---|
| app image `x0x.bin` | **548,672 B** | **551,840 B** |
| RAM `.data+.bss` | **96,404 B** of 98,304 | **96,504 B** of 98,304 |
| POOL | **330,488 B** of 344,064 | **332,280 B** of 344,064 |
| POOL spare | **13,576 B** (≥ 8,192) | **11,784 B** (≥ 8,192) |
| package `x0x.fwsc` | 609,680 B, identity FM-1_900 | 609,680 B (debug) |

`make unit unit-core scenarios screens-lint parity check fw fw-debug` and `tools/check_loader.sh` on
both packages all pass; `make fuzz-ui` does not exist yet (T2c).

## Re-review fixes (merged branch)

Opus's re-review of the merged branch asked for five fixes; each has a test.

1. **A recorded clip starts exactly on the close boundary.** Added `CLIP_CMD_LAUNCH_AT` (the command
   carries the target global step) and `clip_engine_launch_at` / `live_bridge_launch_at`. The second
   REC now finalizes the take (length + held-note ends) and **arms the launch at `rec.close`**, so
   the ISR's `do_step` applies it on exactly that step instead of `launch_now`'s next step. Test:
   `studio_cliprec_at` — the transport starts at REC, REC closes at the next bar (`rec.close = 32`, a
   2-bar take), the clip starts at step 32 (`liveclip1 == 7`, `liveprog1 == 0`) and at step 48 its
   progress is 128 (`16/32`).
2. **Staging and the ring agree.** `launch_cmd` now **pushes first**: if the ring is full the launch
   is refused (`-1`) and nothing is staged, so a valid launch already queued keeps its copy; an
   invalid clip touches no staging. `consume_cmds` no longer gates a launch on `next_set` (it queues
   every launch and swaps at the boundary), and `apply_role` no longer clears `next_set` on a stop
   or a cancel, so a valid launch queued behind keeps its copy. Test: `ring_many_launches` is
   **identical to the pre-T7b golden again** (63 events, ending with the last valid launch — clip 12
   — playing): the earlier collapse was an artifact of the `next_set` gate.
3. **A nest-safe IRQ mask.** `irq_save` / `irq_restore` in `fm1_flash.h` now count nested sections
   (the pi32v2 status does not expose the interrupt-enable bit): the first save disables, the last
   restore re-enables, and an inner restore never re-enables while an outer section holds. The host's
   `plat_crit_enter/exit` (moved to `host/plat_crit_host.c`) model the same. Test: `make unit-crit`
   (`tests/host/crit_test.c`) — nested enter/exit restores the previous state.
4. **Undo stages, it does not re-init.** `song_io`'s `undo_run` now calls `live_bridge_clip_changed`
   for a clip entry (and stops the role when the restored clip is unused); only boot and a song open
   / load (stopped) call `live_bridge_song_reload`. Tests: `song_io_test`'s
   `test_undo_stages_not_reloads` (the change fires, the reload does not) and `studio_clipundoplay`
   (undo while a clip plays: `playing == 1`, `liveclip1 == 7`, the note gone).
5. **The drum grid scroll indicator.** The grid draws a thin scrollbar (a thumb for the visible
   window) and up/down chevrons when more lanes exist; the lane labels shift right to make room.
   `studio_clipdrums` shows it.

### Re-seeded goldens and why

- `studio_clipdrums`, `studio_clipdrumrec`, `studio_clipliveedit`, `studio_cliplong`: the drum grid's
  scroll indicator (item 5).
- `studio_cliprec`, `studio_clipoverdub`, `studio_clipmotion`, `studio_clipundorec`: the record now
  launches exactly on the close boundary (item 1); `studio_clipundorec` also waits a bar for the
  role's stop (undo no longer re-inits).
- `studio_cliprec_at`, `studio_clipundoplay` (new): the item-1 and item-4 proofs.
- `tests/golden/clip_engine/ring_many_launches.txt`: restored to the pre-T7b 63-event stream (item 2).

### Device build (re-review, on deltron.local)

| | `make fw` | `make fw-debug` (X0X_DEBUG=1) |
|---|---|---|
| app image `x0x.bin` | **549,008 B** | **552,176 B** |
| RAM `.data+.bss` | **96,404 B** of 98,304 | **96,504 B** of 98,304 |
| POOL | **330,588 B** of 344,064 | **332,380 B** of 344,064 |
| POOL spare | **13,476 B** (≥ 8,192) | **11,684 B** (≥ 8,192) |

`make unit unit-core scenarios screens-lint parity check fw fw-debug` and `tools/check_loader.sh` on
both packages all pass.






