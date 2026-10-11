# TCLS: remove Classic patterns, shorten the delay to 0.75 s

Owner decision: [ADR-0004](../docs/decisions/0004-remove-classic-shorten-delay.md). This report says
what was removed, the pool and RAM before and after, and what moved in the goldens.

**Result:** the firmware has two modes, STUDIO and LIVE, and one thing that owns the audio (the clip
engine). X0X's pattern editors, its pattern sequencer, its song arranger, TB-3PO and the whole
`GLOBAL → Classic patterns` path are gone. The delay line is 0.75 s. The pool gained **38,944 B**
(~39 KB) and `.data+.bss` lost **20,480 B**, so both builds are far above the 8 KiB pool spare.

## What was removed

**UI (firmware/src/app/ui.c, ui_studio.c, ui_live.c, ui_help.c).**
- The `GLOBAL → Classic patterns` row, the `Back to song` row, the `CLASSIC` header chip, the
  `GLO: BACK TO SONG` footer and the `ACT_CLASSIC` action are gone, and `ui.classic` with them.
- The GLOBAL list lost every X0X pattern/song row: CLEAR PATTERN, COPY PATTERN, SONG mode,
  CLEAR SONG and INSERT/DELETE bar (the V_SONG arranger is unreachable). The `PERFORM` knob page's
  SWING knob (which edited X0X pattern swing) is gone; the clip SWING on the STUDIO clip-settings
  page is the only swing. The clip-editor `SAVE + REC: clear part` and the `SAVE + white key: paste
  pattern` gestures are gone with the patterns.
- HOME and SEQ on a surviving X0X page return to the song home; EDIT opens the cursor role's sound
  page; ARP (TB-3PO) does nothing. The reachable inherited pages are the sound pages (909/808/303,
  FM6/NOISE cards), FX, MIX and GLOBAL.
- X0X's "hold one button ~1 s" help (the Classic help) is off (`help_tick` is a no-op); the reachable
  help is the GLO + button card and the HOME cheat sheet. The cards that named Classic were rewritten.
- `SAVE` now waits for the **clip** transport to stop, not X0X's (`save_project`): with Classic gone
  `seq.playing` is always 0, so a SAVE while the clip engine played would have written flash mid-audio.
- **STOP releases every voice again.** X0X's transport stop all-off'd through `engine.c`'s
  playing→stopped edge; that edge is dead now (`seq.playing` is always 0). The STOP button (STUDIO
  and LIVE) calls `engine_all_off()` after stopping, so a note a key, MIDI or a sound page left
  sounding is released as before.
- `RUN PERF TEST` (GLOBAL → PERFORMANCE) is gone: it played three X0X patterns through the removed
  sequencer. The PERF screen keeps its live CPU/stats readout.
- The LIVE footer is uppercase (`SCL PLAY`, `KEYS PLAY BASS`, `TAP LATCH`, `HOME+FX RESET`), which
  fixes the descender clip screens-lint now sees because the tour visits LIVE.

**Audio path (engine.c, midi_live.c, live_bridge.c).**
- `engine_render` no longer advances X0X's sequencer: the clip engine is the only render path, with no
  `seq_advance` branch. The audio ISR no longer drains X0X's per-part MIDI-in (`midi_in` is gone);
  the main loop's `midi_live_poll` owns the ring in every view, with no Classic branch.
- `live_bridge_enter()` takes no pattern/bpm and never seeds from the project. The one-time project
  mapping (`live_bridge_rebuild` on the boot path) is gone; the mapping functions stay only for
  rc_debug's stress content and the mapping unit test. `live_bridge_exit` stays for rc_debug's
  stress teardown; the app never leaves the clip engine. `engine_all_off` is kept on every remaining
  handoff (LIVE enter/leave, SONGS, undo of a cleared playing clip).
- rc_debug lost stress levels 1 and 6 (the X0X-sequencer baseline); `tools/rc_probe.py` and
  `tests/scenarios/rc1_probe.x0x` were updated to match.

**Project / RAM (x0x.h, project.c, song_io.c).**
- `project_t` no longer holds `pattern_t pat[NPAT]`. `project_save`/`project_load` write and read only
  the settings and the sound; X0X's project patterns, song and motion lanes are neither stored nor
  loaded, and the X0X project region on the flash is left untouched (never erased or written). A first
  boot (no stored song) builds the **Starter** song from `song_templates.c` (owner: a new device should
  play something at once); an empty song slot uses the New-song templates as before. `project_pat(i)`
  returns one shared scratch pattern for the unreachable inherited editor code.
- FM6 is unchanged. The fuzzer found "3 voices after the release tail" on seed 27; it is **not** a
  hung note and not a TCLS regression: every one of the run's 103 FM6 note-ons has its note-off, the
  voices are released (`fm6held 0`), and they ring out slowly — the **GLASS BELL** factory patch
  (F2) releases over several seconds, which the engine's own test already allows (it renders up to
  ~24 s for the voices to end, `tests/host/fm6_engine.c`). The same minimal sequence (load F2 on the
  CHORDS sound card, play and release a note) fails identically on the base commit `615c93d`
  (`firmware/src/dsp/eng_fm6.c` and `fm6_core.c` are byte-identical to it), so it predates TCLS;
  TCLS only changed the fuzz's state trajectory so seed 27 reaches it. The fuzz's fixed 3 s tail was
  too short, so it now waits for the voices to end, bounded at ~25 s (a truly hung note never ends
  and still fails); the new `tests/scenarios/fm6_release.x0x` pins the exact sequence.

## Pool and RAM before and after

`make fw` and `make fw-debug` on deltron, committed tree.

| Build | | POOL used | POOL spare | `.data+.bss` |
|---|---|---:|---:|---:|
| `make fw` before | | 333,692 | 10,372 | 96,228 |
| `make fw` after | | **294,748** | **49,316** | **75,748** |
| `make fw-debug` before | | 335,484 | 8,580 | 96,344 |
| `make fw-debug` after | | **296,540** | **47,524** | **75,848** |

The 38,944 B of pool is the 0.75 s delay line (**22,050 B**) plus the 16,896 B the 32-pattern bank
took in the pool through the X0X diff-undo shadow (`undo_shadow` is sized from `project_t`, and the
patterns sat inside it). `.data+.bss` lost the pattern bank itself (and alignment). The architecture
RAM plan was updated with these numbers.

**The delay clamp.** `DLY_LEN` is 33,075 (0.75 s at 44.1 kHz). `fxbus` already halves then clamps
every synced division to `(dlen - 256)` samples, so no read and no write can pass the line. The new
`tests/host/delay_clamp_test.c` pins the line and the clamp edge: the longest synced time that fits
is **666.67 ms at 60 bpm** (the 2/3-beat and 4/3-beat divisions) and **562.5 ms at 80 bpm** (the
1/2. and 1/4. divisions). Note ADR-0004 says a quarter note still fits at 80 bpm: a quarter is
exactly 750 ms = `DLY_LEN/SR`, but the 256-sample safety margin (a full-length read, and the TAPE
wow/flutter excursion, must stay off the write head) puts the usable maximum at 744.2 ms, so the
quarter halves to 375 ms (still on the beat). The margin is kept.

## Goldens

Regenerated with `GOLDEN_UPDATE=1` and viewed at 1:1. The changes are the expected ones:

- **`studio.classic` is gone** from every `dump` JSON (studio, songs, live, midi scenarios).
- **The boot song is the Starter template** (LIVE scenarios, `harness`, `help_cards`): the LEDs and
  JSON show the Starter's clips on keys 0-3/7-9/12-14/19-20/24 instead of the empty project mapping;
  the LIVE pads/play/LED shots show those clips.
- **A New Empty song** for the STUDIO clip-editor scenarios (`newsong 0`): `songs.current` is 0
  instead of -1, and the grid shots are empty before the edit.
- **The `CLASSIC` header chip and `GLO: BACK TO SONG` footer** are gone from the STUDIO/part shots;
  the `EDIT` button is no longer lit in the STUDIO dumps.
- **The uppercase LIVE footer** changes the LIVE shots (`live_fx*`, `live_hold`, `live_pads`,
  `live_play`, `live_press`, `live_leds`, `midi_*`, `snd_hang*`, `studio_live`).
- **`harness`**: the events log now carries the `param` event, the transport dump is the STUDIO song
  home (view 10, no classic), and the shot is the STUDIO header.
- **Removed** (scenario and golden): `patterns`, `plock`, `prob`, `probcond`, `probreset`, `tb3po`,
  `step303`, `steps64`, `song`, `songrep`, `trackmute`, `undo`, `migrate`, `migrate0102`,
  `studio_classic_exit`, `groove`, `jam`, `showcase`, `tour`, `getting_started`, `help`, `keysound`,
  `perf`, `live_entry_stop` (it tested the X0X→LIVE hand-over, which is gone).
- `screens_tour.x0x` now tours only the surviving views (STUDIO, the sound/FX/MIX/GLOBAL pages,
  PERF and LIVE); `screens-lint` reports 0 findings.

## Verification

Every target the task asks for, on the Mac (`deltron.local` for the firmware builds); logs under
`build/ladder/`.

| Command | Result |
|---|---|
| `make unit` | pass (incl. the new `delay_clamp` suite) |
| `make unit-core` | pass |
| `make scenarios` | pass |
| `make screens-lint` | pass: 0 clipped, 0 overlap, 0 tiny |
| `make fuzz-ui` | pass: 200 seeds × 2000 events |
| `make check` | pass (forbidden symbols, flash guard, help cards, size, short fuzz) |
| `make fw` | pass: pool 294,748 B of 344,064 |
| `make fw-debug` | pass: pool 296,540 B of 344,064 |
| `tools/check_loader.sh` (both packages) | ok: the loader is X0X 1.0.5's |

Hardware was never flashed.

## Files

Changed: `firmware/src/app/{ui.c,ui_live.c,ui_studio.c,ui_help.c,ui_help.h,engine.c,midi_live.c,
midi_live.h,live_bridge.c,live_bridge.h,song_io.c,song_io.h,project.c,x0x.h,plat.h,rc_debug.c,
main_fm1.c}`, `core/seq/clip_engine.h`,
`host/x0x_host.c`, `tests/host/{delay_clamp_test.c,song_io_test.c,live_bridge_test.c}`,
`tests/run_tests.sh`, `tests/parity.sh`, `tools/rc_probe.py`, `Makefile`, the scenarios and goldens
above, the new `tests/scenarios/fm6_release.x0x`, `docs/{MANUAL.md,manual.html,open-questions.md,
product/interaction-model.md,product/screens.md,engineering/architecture.md,
engineering/rc1-probe.md,decisions/0004-remove-classic-shorten-delay.md}`.

Not changed: `firmware/src/dsp/` (FM6 and the DSP are byte-identical to the base commit).

## r1: the Opus review findings

The r1 pass fixes the three findings from the review of the TCLS tree, plus the nits it named. No
behaviour the TCLS report promised changes; the ladder was re-run on the committed tree.

### 1. No X0X pattern gesture survives on FX / MIX / GLOBAL

`key_event` returned early for STUDIO, the clip editors and the sound pages only, so the inherited
pattern gestures still ran on FX, MIX and GLOBAL:

- SAVE + a white key pasted the pattern (`ACT_COPY` on the dead scratch pattern, `mark_dirty`);
- HOME + a white key chained / cued it (`seq_chain` / `seq_cue`, and the inherited header drew a
  `P1 >N` chip);
- MIX + HOME + a black key muted a dead X0X part / drum track (`seq.mute` / `seq.vmute`).

Those gestures are gone from `key_event`. MIX keeps its own black-key part / track pick (stereo.x0x
still passes), and FX, MIX and GLOBAL return before the step keys. The fuzz then found the same
kind of gesture on the encoders and buttons, so those went too: the PRESETS encoder no longer
chains / cues a pattern on any surviving page, SAVE + OCT no longer picks the copy bank
(`COPY TO P17-32`), and OCT no longer writes a step (LFO + OCT wrote a tie, ENV + OCT a rest) on the
sound pages. The inherited header's X0X pattern / cue / chain chip is dropped
(`draw_header`); the pages kept from X0X are setup / sound pages and no longer claim to play a
pattern. `ui.help` (the Classic 1 s help) keeps its field only for the host's `expect help 0`.

The UI fuzz now snapshots the shared scratch pattern, `proj.arr` (X0X's song and motion lanes),
`seq.ppat`, `seq.pcue`, the chain, `seq.song_on` and `seq.pat`, and fails the moment an event
changes any of them. The 200 × 2000 run found the PRESETS-encoder and the OCT step-edit paths the
review's key-only repro missed; all are fixed. A scenario per gesture pins it (each fails on the
pre-r1 tree, passes here):

- `tests/scenarios/fx_save_paste.x0x` — FX + SAVE + white key; `expect dirty 0` and no pattern
  state moves.
- `tests/scenarios/fx_home_chain.x0x` — FX + HOME + white key; `expect cue 0`, and HOME returns.
- `tests/scenarios/mix_home_mute.x0x` — MIX + HOME + black key (PARTS and a drum page);
  `expect mute 0` / `vmute 0`.
- `tests/scenarios/glo_pattern_gestures.x0x` — GLOBAL: paste, chain and the Classic keyboard all do
  nothing (`expect voices 0`).
- `tests/scenarios/sound_oct_no_edit.x0x` — a 303's sound page with REC on: LFO + OCT / ENV + OCT
  leave the scratch step a rest and the project clean (`expect gate0.0 0`, `expect dirty 0`).

`legato_cut.x0x` tested the Classic keyboard (REC + keys on GLOBAL), which is exactly the path that
is gone; it is removed. `legato_live.x0x` keeps the mono-part rule on LIVE's PLAY layer.

### 2. SAVE no longer sticks on "SAVES WHEN STOPPED"

`save_project` and the autosave gated on `seq.playing` / `seq.req_stop`. With Classic gone
`seq.playing` is always 0, and `seq.req_stop` was set by the OTA path's `seq_stop` and cleared only
by `engine_all_off` (the audio ISR's `all_off_now`), so a SAVE after an OTA cancel could stick
forever. Both gates now use the clip transport and the voices only:

```c
if (live_bridge_transport_playing() || engine_voices_active()) { ... }
```

The OTA path (`main_fm1.c`) stops the clip transport and releases the voices instead of calling
`seq_stop`, and flushes the dirty song unconditionally before the OTA work (the
`if (!live_bridge_active())` guard was dead: the clip engine is active from power-on). The host
does not run `main_fm1.c` (no OTA session in the simulator), so `tests/scenarios/save_after_stop.x0x`
reproduces the failure at the seam it can reach: the host's `stop` command sets the same
`seq.req_stop` (it deliberately does not all-off), and the scenario then asserts SAVE saves
(`expect store_writes >0`). It fails on the pre-r1 gate and passes here.

### 3. FACTORY RESET wording

The GLO question and the confirmation said `SOUND, PATTERNS, SONG` and `FACTORY SOUND + PATTERNS`,
but `project_defaults` resets the settings and the sound only — not the X0X patterns (gone) and not
the clip song. They now say `SOUNDS + SETTINGS` and `FACTORY SOUND + SETTINGS`. No test golden
carried the wording (the `dump` JSON has no message text); the generated `docs/manual.html` was
regenerated from `docs/MANUAL.md`, whose FACTORY RESET row now reads "the factory sounds and
settings. The current song is kept".

### NITs

- **The fuzz tail is tighter.** It now waits 3 s after the stop and fails if any non-FM6 voice is
  still sounding or any remaining FM6 voice is still held (not in its release stage); FM6's GLASS
  BELL may then ring out, released, up to the same ~25 s bound, and the final `voices 0` /
  `heldnotes 0` check stays. A reproduction writes `stop / wait 3000 / expect nfm6voices 0 /
  expect fm6held 0 / wait 22000 / expect voices 0 / expect heldnotes 0`.
- **Dead code removed.** `g_song_ready` (live_bridge.c; never read), `help_lines` and the dead
  `draw_help` path (ui.c; `ui.help` was never set, so the stale THE PATTERN / TB-3PO / copy cards
  never drew), and the whole PERF TEST (`perf_test_start/abort/tick`, the `perf` test fields,
  `perf_bench`, `perf_snd`, `ACT_PERF_TEST`, its GLOBAL row and the host's `expect perftest` /
  `perfres`). `rc_debug` does not touch it, so it was truly unreachable. The PERF screen keeps its
  live CPU / stats readout.

### Pool and RAM

`make fw` and `make fw-debug` on deltron, committed tree.

| Build | | POOL used | POOL spare | `.data+.bss` |
|---|---|---:|---:|---:|
| `make fw` (TCLS) | | 294,748 | 49,316 | 75,748 |
| `make fw` r1 | | **294,748** | **49,316** | **72,676** |
| `make fw-debug` (TCLS) | | 296,540 | 47,524 | 75,848 |
| `make fw-debug` r1 | | **296,540** | **47,524** | **72,776** |

The pool is unchanged; `.data+.bss` lost **3,072 B** (the PERF TEST's three bench `pattern_t`s and
its `sound_t`, plus alignment).

### Verification

Every target the task asks for, on the committed tree. Logs under `build/ladder/`.

| Command | Result |
|---|---|
| `make unit` | pass |
| `make unit-core` | pass |
| `make scenarios` | pass (incl. the 6 new scenarios; `legato_cut` removed) |
| `make screens-lint` | pass: 0 findings |
| `make fuzz-ui` | pass: 200 seeds × 2000 events |
| `make check` | pass (forbidden symbols, flash guard, help cards, size, short fuzz) |
| `make fw` | pass: pool 294,748 B of 344,064; `.data+.bss` 72,676 B |
| `make fw-debug` | pass: pool 296,540 B of 344,064; `.data+.bss` 72,776 B |
| `tools/check_loader.sh` (both packages) | ok: the loader is X0X 1.0.5's |

Hardware was never flashed.

### Files

Changed: `firmware/src/app/{ui.c,ui_help.c,main_fm1.c,live_bridge.c}`, `host/x0x_host.c`,
`docs/{MANUAL.md,manual.html}`, `reports/TCLS.md`, and the new
`tests/scenarios/{fx_save_paste,fx_home_chain,mix_home_mute,glo_pattern_gestures,sound_oct_no_edit,save_after_stop}.x0x`.
Removed: `tests/scenarios/legato_cut.x0x`.
