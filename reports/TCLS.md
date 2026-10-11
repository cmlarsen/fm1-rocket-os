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
- A patch change in FM6 (`eng_fm6_load_slot` / `eng_fm6_set_patch`) now retires every live voice. The
  fuzzer found a hung FM6 voice: a released voice's `fm6_note_done` reads the *current* patch, so a
  preset swap mid-release left it sounding forever. Browsing presets cuts the auditioning note, as a
  new patch does anyway.

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
main_fm1.c}`, `firmware/src/dsp/eng_fm6.c`, `core/seq/clip_engine.h`,
`host/x0x_host.c`, `tests/host/{delay_clamp_test.c,song_io_test.c,live_bridge_test.c}`,
`tests/run_tests.sh`, `tests/parity.sh`, `tools/rc_probe.py`, `Makefile`, the scenarios and goldens
above, `docs/{MANUAL.md,manual.html,open-questions.md,product/interaction-model.md,
product/screens.md,engineering/architecture.md,decisions/0004-remove-classic-shorten-delay.md}`.
