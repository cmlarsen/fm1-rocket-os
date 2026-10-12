# TFIXU — UI and tooling fixes from the code review

Implemented 2026-10-11 on `cmlarsen/tfixu-ui`. Sources read first:
[reports/review/ui.md](review/ui.md), [reports/review/tests.md](review/tests.md),
[reports/review/verify-ui-tests.md](review/verify-ui-tests.md),
[docs/product/interaction-model.md](../docs/product/interaction-model.md), CLAUDE.md.

Every behaviour change has a failing test/repro written first (the repros were run
against a `git archive HEAD` checkout in `/tmp` before the fix), and the goldens
that changed were regenerated only with `GOLDEN_UPDATE=1` and viewed at 1:1. **No
hardware was flashed; no device was opened.** Commits: `3955cf3` (tooling),
`9673eef` (UI), `4eaddb1` (LIVE PLAY scale).

## Status by scope item

### 1. The installer enforces the approved loader (tests #1) — done
`tools/loader_check.py` is the one implementation of "select the loader the way
`ota_ufw` does" (the type-100 UFW entry, failing closed on any structural
problem). `tools/check_loader.sh` is now a thin wrapper over it, with unchanged
arguments, output and exit codes (0/1/2). `tools/fm1_install.py`'s `load_package()`
reads `tools/x0x_loader.sha256`, computes the type-100 loader's sha256 and refuses
any mismatch **before any MIDI port is opened**. `--force` is now the stock-rescue
path only: it prints a warning and skips only the sha comparison (a structurally
valid FM-1 package is still required, so it can never serve arbitrary bytes).

- Test: `tests/install_test.py` builds genuine UFW packages (via
  `tools/fm1pkg_make.py`, like `tests/check_loader_test.py`) and adds six gate
  checks: an approved loader is accepted; a same-length tamper that keeps the
  marker is refused with no MIDI sent; a structurally broken package is refused
  even with `--force`; a missing sha file fails closed; `--force` warns and
  installs. Repro before the fix: 4 gate checks FAIL.
- `make check` now runs `sh tools/check_loader.sh build` (guaranteed by `check: fw`).
- `tools/check_loader.sh build/x0x.fwsc` and the negative suite pass on the built
  package (see the ladder below).

### 2. Help ownership (ui #2, #3) — done
`ui.c`/`ui_live.c`: while GLO is held, or a card is up, a button belongs to help.
`live_play_tick`, `live_rec_tick` and `live_fx_button` skip a claimed button's
edge/timer and consume its release, so:

- GLO + REC neither arms, stops nor clears the take (the clear timer is also
  cancelled if the card claims a press that already started).
- GLO + PLAY starts no transport and does not enter LIVE, short or long press.
- GLO + FX opens no layer (`fxlayer` stays 0, including while held).

- Test: `tests/scenarios/help_ownership.x0x`. Repro before the fix: FAIL
  `liveplaying 0`, `mode 0` (PLAY), and the take section then cascades.

### 3. Clear state (ui #1, #7) — done
- `set_view()` calls a new `studio_cancel_pending()`, so a pending clear or tap
  is dropped on any view change and on a mode change (entering/leaving LIVE). A
  HOME clear now requires HOME **and** the pad to stay held for the whole hold.
- A SEL clear sets `ui.btn_used |= 1<<B_SEL` as soon as the 250 ms hold crosses,
  so an early release no longer falls through to the ordinary SEL tap that queued
  a launch.

- Tests: `tests/scenarios/studio_clear_sel_abort.x0x` (repro: `liveque0` was 0),
  `tests/scenarios/studio_clear_view_exit.x0x` (repro: `clipused0` was 0 for both
  the page-exit and the LIVE mode-change case). Existing `studio_clear.x0x` and
  `studio_seltap.x0x` still pass; `studio_seltap`'s golden changed (see below).

### 4. Test harness (tests #2, #3, #4, #5, #8) — done
- **#2 missing expected WAV** — `tests/golden.py` now compares the expected WAV
  filename set from `audio.json` against the directory first and reports every
  missing expected file, before the loose metric check. Repro: `audio.json`
  expecting `a.wav` with an empty outdir printed "golden ok"; now exit 1.
- **#3 parity** — native and WASM run one shared `tests/parity.x0x`.
  `tests/host/emu_test.mjs` parses the same `.x0x` subset with the host's exact
  timings and compares **both** stereo channels at equal length within 1 LSB
  (was left-channel only, truncated to the shorter length, `1e-4` ≈ 3.3 LSB).
  Repro: WASM was silent (`peak 0.000, max diff 4.00e-1`); now `peak 0.400,
  max diff 3.05e-5 (1.0 LSB), length delta 173`.
- **#4 fresh-checkout unit** — `tests/run_tests.sh` generates
  `build/gen/x0x_drum_samples.h` (and the sibling `x0x_drum_tables.h`) before
  drum-pan/stereo/delay_clamp compile with `-Ibuild/gen`. Repro: `rm -rf build/gen
  && make unit` failed with `'x0x_drum_tables.h' file not found`; now passes.
- **#5 artifacts** — `host/x0x_host.c`'s `png_write`/`dump_json`/`wav_open`/
  `wav_close` now record an open/write/`ferror`/`close` failure that `main()`
  folds into its exit status, and the `scenarios` target (and `run_tests.sh`)
  clears each scenario output dir first so a stale file cannot satisfy a golden.
  Repro: `shot missing/…; dump missing/…; wav missing/…` exited 0; now exit 1.
- **#8 GOLDEN_UPDATE** — `tests/host/clip_engine_test.c` and `tests/host/take_test.c`
  accept only the exact string `"1"`, matching `tests/golden.py`. Repro:
  `GOLDEN_UPDATE=0` rewrote a golden copy; now it is checked and fails.

### 5. The UI report's other medium findings — done
- **ui #4 FX keys use physical keys — done.** `live_handle_key` and `live_leds`
  translate the first six lower/white positions to punch FX and the first five
  upper/black positions to role mutes via `WHITE_KEY`/`BLACK_KEY`. Tests:
  `live_fx.x0x`, `live_fx_audio.x0x`, `live_fx_mute.x0x`, `readme_live.x0x`,
  `tlset_fx_edit.x0x` and `t9i_take_replay.x0x` now name keys by position (`w*`/
  `b*`); repro before the fix: `fxmute1`, `key 17`, `fxactive2`, the tape-stop
  silence and the mute all failed.
- **ui #5 HOME + FX resets mutes — done.** The handler now calls
  `live_bridge_mute_all(0)` alongside `engine_perform_reset()`, and the reset
  press is consumed until FX is released (so it neither opens the layer nor
  repeats). Test: `live_fx_reset.x0x`.
- **ui #6 FX layer releases PLAY notes — done.** `note_release_all()` runs when
  the layer opens and when a double tap latches it. Test:
  `live_fx_note_release.x0x` (repro: `heldnotes` stayed 1).
- **ui #8 tempo gesture on sound pages — done.** HOME + SELECT on the sound/FX/
  MIX pages routes through a shared `song_tempo_nudge()` (song_io + bridge), not
  the dead `seq.bpm`; STUDIO's HOME + PRESETS uses the same path. Test:
  `tempo_sound_pages.x0x` (repro: `bpm` stayed 125).
- **ui #9 LIVE PLAY uses the song's key and scale — done (owner decision, option a).**
  The PLAY keys are now a scale-degree keyboard:
  `note = 12*(3+octv) + song_key + scale_step(k)`, where `scale_step` walks the
  song's scale (major/minor/dorian/penta/chromatic) across octaves as *k* rises
  and the tonic is C2 for C major. The mapped Live notes in PADS share the
  mapping, and the PLAY card shows the key and scale. Tests:
  `tests/scenarios/live_play_scale.x0x` plus a new host `expect playnoteK.O`
  (`live_play_note_for`); the existing PLAY-layer event goldens are the
  failing-first evidence (e.g. `live_hold_events` note 41 → 45). Recorded in the
  interaction model, `open-questions.md` and the README.
- **ui #10 recordings over four bars — done.** `cl_finish`/`cl_rec_close` clamps
  a pass longer than two bars to the four-bar cap instead of falling through to
  two. Test: `clip_over4bars.x0x` (repro: `cliplen7` was 32).
- **ui #13 wide names on screen — done.** The NAME field picks a smaller face
  (AF_L → AF_M → AF_S) when the full name would overflow `NAME_FIELD_W`, keeping
  a maximum-length name on screen. Also fixes **ui #17**: the counter is generated
  from `NAME_MAX`, so it reads "OF 11", not "OF 12". Test:
  `tests/host/ui_name_test.c`'s `test_wide_field` (AF_L overflows, the chosen
  face fits) plus a `wide.png` render. Repro before the fix: the test did not
  exist (compile error) and nine Ws already reached x=260.
- **ui #14 LIGHTS Off vs LIVE LEDs — done.** `live_leds()` runs for `V_LIVE`
  outside the `proj.set.keyled` guard; only the key/dim decoration is gated, so
  PLAY and SEL still show with LIGHTS Off. Test: `live_lights_off.x0x` (repro:
  `button PLAY`/`button SEL` were 0).

### 6. Remove unreachable Classic UI code from `ui.c` (ui #16) — partial
Removed, provably unreachable: the second `case B_ARP:` in `button_tap` (an
unconditional `if (b == B_ARP) return;` precedes it); the `case B_HOME:` and
`case B_SEQ:` blocks (both are handled earlier for every reachable view); and the
Classic clipboard/help ticks `copy_tick()` (its body requires `btn == B_SAVE`
while its only caller runs when `!(btn & B_SAVE)`) and the empty `help_tick()`,
with their call sites. The full scenario suite and unit suites still pass.

The rest of the finding is deferred: the surviving unreachable view machinery
(`V_HOME`, `V_SONG`, `V_SEQ`, `V_GEN`, their draw functions and the pattern
clear/copy/generator/p-lock helpers) is a deep, cross-cutting removal, and the
review's own fix proposes separating the reachable page adaptation into
include-style modules or explicit APIs — a refactor/redesign, which TFIXU says to
skip. The enum values are kept so the host's `view` numbers and the goldens do
not shift; a follow-up task can delete the views and their helpers together.

## Deferred / not done, and why

- **ui #11** (notes started during record-close keep one-step lengths), **ui #12**
  (SONGS help fabricates a HOME release), **ui #15** (HOME cheat sheet claims
  sound-page knob lanes the handler cannot clear), **ui #18** (no common gesture
  contract) — not in TFIXU's scope lists. #12 and #18 are EDGE/MAINTAIN fixes that
  the shared-contract work (also proposed by ui #16) would absorb; #15 conflicts
  with interaction-model.md's own "HOME + touch a knob clears that lane on the
  sound pages" text, so hiding the row needs an owner decision first.
- **tests #6** (fuzz shrinker tail), **#7** (web harness), **#9** (help-coverage
  checker), **#10** (`check` is not the aggregate gate), **#11** (Orca health),
  **#12** (empty layout evidence) — outside TFIXU scope item 4 (tests 2, 3, 4, 5, 8).
  Note the TFIXU Done-when ladder itself is the comprehensive gate tests #10 asks
  for; it now includes the loader check.

## Golden changes (regenerated with `GOLDEN_UPDATE=1`, viewed at 1:1, in `9673eef`)

- `tests/golden/live_fx_audio/live_fx_audio.json` — the role-mute LED moved from
  the old chromatic raw key to its physical upper key (`b1`).
- `tests/golden/studio_seltap/studio_seltap.json` — an abandoned SEL hold no
  longer queues a launch (`roles[0].queued` is `-1`, was `0`).
- `tests/golden/songs_edit/songs_rename.png` — the NAME counter now reads
  "RENAME SONG 7 OF 11" (was `OF 12`). The PNG was inspected at 1:1: the name,
  cursor and hints are unchanged; only the counter text differs.
- `tests/golden/live_play/live_play.png`, `live_hold/live_hold.png` — the PLAY
  card now shows "octave 0 - A minor" (viewed at 1:1).
- `tests/golden/live_play/live_play_events.txt`,
  `live_hold/live_hold_events.txt`, `snd_hang_sel/snd_hang_sel_events.txt`,
  `studio_hang/studio_hang_events.txt`,
  `tlset_take_note/{rec_notes,play_notes,take_rec}.txt` — PLAY/Live-note events
  are now in the song's key and scale (e.g. 41 → 45, 63 → 83).

New scenarios (`help_ownership`, `studio_clear_sel_abort`,
`studio_clear_view_exit`, `live_fx_reset`, `live_fx_note_release`,
`live_lights_off`, `tempo_sound_pages`, `clip_over4bars`, `live_play_scale`)
carry explicit assertions and were left without golden directories.

## Ladder

`make unit unit-core scenarios screens-lint fuzz-ui check parity fw fw-debug`
was run on the committed tree; `tools/check_loader.sh` was then run on both
packages. Results:

Run at commit **70b769f** (`make unit unit-core scenarios screens-lint fuzz-ui
check parity fw fw-debug`, captured in `/tmp/tfxu_ladder2.log`). It exited
`LADDER_RC=0`; every goal passed:

| Goal | Result |
|---|---|
| `unit` | passed (incl. the fresh-checkout drum-header fix) |
| `unit-core` | passed (optimized + ASan/UBSan suites) |
| `scenarios` | all passed (121 scenarios; goldens compared) |
| `screens-lint` | 1585 frames, 3224 canvases, 0 findings |
| `fuzz-ui` | 200 seeds × 2000 events: all passed |
| `check` | passed, including `check: the package carries the approved X0X 1.0.5 loader` on `build/` |
| `parity` | WASM matches native (audio, screen, lights) |
| `fw` | built on deltron.local; `build/x0x.fwsc`, `x0x.bin`, `loader/ota.bin` brought back |
| `fw-debug` | built (X0X_DEBUG=1); `build/debug/x0x.fwsc` brought back |

Extra, after the ladder:

- `sh tools/check_loader.sh build/x0x.fwsc` → ok; `sh tools/check_loader.sh
  build/debug/x0x.fwsc` → ok (`ca93577fddc47710…`).
- `python3 tests/check_loader_test.py build/x0x.fwsc` → passed (the decoy,
  duplicate and broken-CRC negatives still behave).
- `python3 tests/install_test.py` → passed (28 checks, incl. the six gate checks).

The explicit loader checks on both packages are the TFIXU Done-when's last item.
No hardware was connected or flashed.
