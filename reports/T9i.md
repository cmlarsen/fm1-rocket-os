# T9i: the take, recorded in LIVE

Integration lane for T9. Wires T9c's take recorder/player into X0X: the engine tap, the LIVE
view, and `song_io`'s take calls. Merges `integration` (T7b's clip editors, T11h's help cards,
`TAKE_MAX_EVENTS` 1024). Nothing was flashed.

- Files: `firmware/src/app/engine.{c,h}`, `firmware/src/app/live_bridge.{c,h}`,
  `firmware/src/app/song_io.{c,h}`, `firmware/src/app/ui.c`, `firmware/src/app/ui_live.c`,
  `firmware/src/app/rc_debug.c`, `host/x0x_host.c`, `host/build_host.sh`, `web/emu/build.sh`,
  `tools/build.py`, `core/model/undo_stack.{c,h}`, `Makefile`, `tests/run_tests.sh`,
  `tests/host/live_bridge_test.c`, `tests/host/song_io_test.c`,
  `tests/scenarios/t9i_take_*.x0x` (7), `tests/golden/t9i_take_*` (7), and the docs updated
  below.

## What T9i wires (the T9c report's "what T9i must wire")

1. **The engine tap.** `engine.c`'s `drain()` calls `live_bridge_take_feed(base, cmd)` once per
   drained command, with `base = eng_prof_frames - n` (the **block-start** sample the T9c note
   asks for; `engine_tap` stays at the block's end, so T2a's goldens are untouched). The take
   keeps only user actions: launches, mutes, punch FX, parameter moves and notes played live
   (the PLAY layer's `engine_bass_on`/`engine_note_on` go through the command queue). The clip
   engine's own notes are applied directly in `live_apply` and only reach `engine_tap` (the host
   log), **not** `take_feed`; replay regenerates them from the recorded launches. Confirmed.
2. **The bar grid.** `take_set_bar_fn` with `bridge_bar_fn`, which returns the first bar boundary
   at or after `sample` from the clip engine's transport (the same grid `clip_engine_pos` uses).
   REC while playing arms at the next bar; REC while stopped arms for the next PLAY, which calls
   `take_arm` with the identity bar at the transport's first block (bar 0).
3. **The prelude.** `bridge_prelude` writes the offset-0 state: the playing clip per role
   (`TAKE_LAUNCH now`) or a stop, the mutes, the tempo, and every parameter's current value from
   the engine's sound mirror (`engine_sound_mirror`). It runs inside `take_arm` (main loop), so
   `take_feed` stays constant time in the ISR.
4. **Launches, mutes, punch FX.** The clip engine's own events are not enough (they do not carry
   `queue`/`now`, and a live mute has no event), so the main loop posts each intent into a small
   SPSC ring (`tp_post`) and the ISR's `live_bridge_take_block` stamps it at the block boundary
   where the clip engine will process the clip ring, then `take_feed`s it. A recorded stream
   therefore replays sample-exactly. `engine_perform_engage`/`_release` call
   `live_bridge_take_fx` (a no-op unless recording).
5. **Playback.** `live_bridge_take_play` resets the clip transport to bar 0, silences everything,
   and `take_play`s; `live_bridge_take_block` runs the pump at the block's first frame, before
   the clip render, so a replayed launch is consumed in the same window. The sink
   (`take_sink_ev`) applies parameters and notes directly to the engines (`engine_take_cmd`),
   maps launches/mutes onto the clip engine, FX onto perform and tempo onto the clip engine's
   BPM. When the take runs out the transport stops and everything goes all-off.
6. **Storage.** `song_io` owns the deferred write: a finished recording (or a clear) queues it
   and it rides the normal stopped-and-quiet flush beside the song (`song_io_flush` ->
   `song_io_take_flush`), plus an in-LIVE flush once the transport is stopped so a replace/clear
   can be undone in-session. `song_t.has_take` is set. A clear is stored as an empty take, which
   supersedes the old record at boot and keeps the old one recoverable for one undo.
7. **Undo.** `undo_run` passes T6c's take scratch, backed by the in-RAM take buffer (no second
   16 KiB). Undo of a not-yet-flushed record/clear cancels the pending write and reloads the
   store's take (the previous one); a flushed op uses the durable take-undo stack. `has_take`
   follows the restored take. HOME+REC in LIVE undoes.
8. **UI.** REC arms/stops (tap) or clears (1.5 s hold, with the ring); the header shows a red REC
   chip and the elapsed bars, the main area a red frame, and a red dot marks each K1-K4 whose
   moves are recorded. EDIT opens the take card (LIVE 10: length, bars, role lanes, SEL to play/
   stop); LIVE input is ignored while it plays apart from stop.

## Follow-ups from the coordinator

- **Clip notes are not recorded** (point 1 above): only user actions; replay regenerates clip
  notes from the launches.
- **Per-step parameter coalescing.** A parameter change whose last recorded event is the same
  parameter (same target/voice/index) in the same 16th step replaces that event's value (the last
  value wins), so a long knob sweep costs at most one event per step. A single `turn K1 8 20`
  collapses from 8 events to 1-2. Implemented in `live_bridge_take_feed`.
- **Pool.** `TAKE_MAX_EVENTS` is 1024 (integration), so the take is 8,244 B. The undo ring was cut
  from 23 to 11 entries (12,240 -> 5,856 B) and the debug build's FM6 array from 8 to 6 voices,
  on the coordinator's direction. See the numbers below.

## RAM (make fw / make fw-debug on deltron.local)

| build | pool | `.data+.bss` | spare | margin over the 8 KiB rule |
|---|---|---|---|---|
| release | 332,448 / 344,064 | 96,540 / 98,304 | **11,616** | **3,424** |
| debug (X0X_DEBUG=1) | 333,608 / 344,064 | 96,656 / 98,304 | **10,456** | 2,264 |

Both keep the 8 KiB rule. The release margin (3,424) covers T11i's MIDI rings (~3 KB); the
debug margin is 2,264, which the coordinator accepted (debug-only code will be trimmed if T11i
pushes it under). No user-facing buffer was cut.

## Tests

`make unit unit-core scenarios screens-lint parity check fw fw-debug fuzz-ui` all pass, and
`tools/check_loader.sh` is ok on both packages. The T9i scenarios (with goldens) are:

| scenario | what it pins |
|---|---|
| `t9i_take_replay` | record a session (two launches, a knob move, a latched punch FX, a role mute), stop, replay: `eventscmp` says the note/drum stream is identical relative to the first event; the take's own log is pinned |
| `t9i_take_reboot` | record, save, `reboot`, replay: still exact |
| `t9i_take_full` | feed notes past the cap: the take closes cleanly with `TAKE_MARK end`, `take_full` set, 1,024 entries |
| `t9i_take_clear` | hold REC 1.5 s: `has_take` 0, stored; HOME+REC restores it |
| `t9i_take_replace` | record take 1, record take 2, undo: take 1's log is back |
| `t9i_take_hung` | a held FM6 note (no note-off) is released when the take ends mid-note, and leaving LIVE during playback releases everything: `voices 0` |
| `t9i_take_screens` | LIVE 4 (recording), LIVE 8 (clear ring), LIVE 10 (the take card) PNGs |

REC while the transport is stopped is exercised inside `t9i_take_replay`/`_reboot` (REC arms,
then PLAY starts the take at bar 0).

## Notes

- **`engine_tap` stays at the block's end**; only the take uses the block-start stamp. The T9c
  note suggested moving the tap too, but that shifts every existing `events` golden by one block;
  the take does not need it.
- **A clear is stored as an empty take**, not a new store API: it supersedes the old record at
  boot and keeps it for one undo. `song_store_list`'s `has_take` therefore reads 1 for a cleared
  slot until the next take save; the song's own `has_take` (the authoritative flag) is 0.
- **LIVE is not in `screens_tour.x0x`.** Adding it exposes a pre-existing 1 px footer clip
  (`SCL play` at y=71 in the 82 px knob band) that `screens-lint` would flag; fixing the footer
  would churn every LIVE golden. The LIVE 4/8/10 goldens are in `t9i_take_screens` instead, and
  `make screens-lint` is clean (0 findings). Flagged for the owner.
- **Two recordings without a stop between them**: the first take is not stored yet, so an undo
  cannot bring it back (the store still holds the pre-take state). The in-LIVE flush on transport
  stop makes the normal replace+undo work; documented in `model.md`'s undo limits.

## r1: the Opus review findings

Reviewed at `bf9c038` (already merged with integration, incl. T11i). Each blocker and should-fix is
fixed and, where a host test can catch the race, proved by one that fails on the reviewed code. The
ladder (`make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug`, plus
`tools/check_loader.sh` on both packages) passes on the committed tree; both builds keep the 8 KiB
pool spare (release and debug pool **335,556 B**, spare **8,508 B**).

Host tests render synchronously and cannot catch ISR races, so the two new `unit-live` tests simulate
the preemption: they call the ISR-side function between the main loop's steps. `clip_engine.c`'s
`push_cmd` and `live_bridge.c`'s `tp_post` now trap under `X0X_HOST` when a host test sets the ISR
flag, so a main-loop ring push from the audio path aborts the test. Restoring the reviewed sink (an
`ISR`-side `live_bridge_launch_now`) makes `b1_isr_sink_test` trap; reverting `live_bridge_exit` to
the reviewed "request a stop at a bar that never comes" makes `b2_leave_test` fail.

### Blockers

- **B1 — the sink ran main-loop APIs in the audio ISR.** The sink (`take_sink_ev`) now calls
  ISR-side apply functions only: `clip_engine_take_launch/_stop/_mute/_bpm` (new, in
  `clip_engine.c`) and `engine_take_perform`/`engine_take_perform_reset` (new, wrapping
  `perform_take_fx`/`perform_take_reset`). None pushes a ring or reads `song->clip[]`. The main loop
  stages each launch's clip copy in the clip engine's own `next[role]` (`live_bridge_take_stage`),
  called at `take_play` (for the offset-0 prelude launches) and every UI frame while a take plays;
  `apply_role` swaps it in at the boundary. The take end no longer calls
  `clip_engine_transport_stop` from the ISR: it sets `g_eng.playing = 0` directly (the clip render's
  stop branch releases every role), clears what the take set, and signals the main loop with
  `g_take_end_req` (`live_bridge_take_end_pending`). Mutes are latched (`take_mute[]`) and applied at
  the render's window start, so a mute's note-offs reach the sink.
  Test: `unit-live`'s `b1_isr_sink_test` (stages, arms the traps, runs `live_bridge_take_block`,
  asserts the launch was queued ISR-side with no ring push).
- **B2 — leaving LIVE lost the recording.** `live_leave` now calls `live_bridge_take_live(0)`, which
  finalizes the take at once (`take_finish`, new — no bar wait), stops any playback and stops the
  feed. `live_bridge_take_feed` records only while `g_take_live` (entered in `live_bridge_enter`,
  left in `live_bridge_take_live(0)`, also from `live_bridge_exit`). `song_io_take_load_ram` and the
  take-undo paths refuse while a take is armed/recording/playing.
  Test: `unit-live`'s `b2_leave_test` (records, leaves via `live_bridge_exit`, asserts the take was
  finalized and kept, and that a later feed appends nothing).
- **B3 — the in-LIVE flush bypassed autosave.** `live_take_tick`'s direct `song_io_flush()` is gone.
  A finished take (or clear) stays pending and rides the normal autosave path (stopped and idle,
  never while LIVE is active). Replace and undo still work in-session: undo of a not-yet-written
  replace/clear cancels the pending write and reloads the stored take. `model.md`'s undo limits and
  take notes are updated. The t9i scenarios now leave LIVE to let the save ride autosave; their
  goldens are regenerated.

### Should-fix

- **S1 — a Program Change launch lost its quantize.** `live_bridge_launch_quant` posts a new
  `TP_LAUNCH_Q` intent; the ISR records `TAKE_LAUNCH_QUANT` with the quantize packed into the
  command's role byte, and the sink replays it with `clip_engine_take_launch(..., quant)`.
- **S2 — tempo and FX state were not recorded.** The prelude now writes the tempo (already did) and
  all six punch FX (latch/on/off from `engine_perform_active`), so a replay reconstructs the state.
  UI tempo and the followed clock (`live_bridge_set_bpm`/`_soft`) post a `TP_BPM` intent, recorded
  as a `TAKE_MARK_TEMPO`. A MIDI Start or Continue during a recording ends the take cleanly
  (`live_bridge_take_finish`) and both now honour a take armed while stopped
  (`take_arm_if_pending`). Documented in `model.md`.
- **S3 — take end/stop left the FX and mutes it set.** Both the natural end (ISR) and
  `live_bridge_take_stop_play` (main loop) now reset perform and clear the mutes; `live_leave`
  resets them after the take stop.
- **S4 — main-loop reads/writes of `g_eng` outside `engine_crit`.** `eng_reset_transport` and the
  PLAY publish + `g_eng.playing = 1` are one `engine_crit` section in `live_bridge_take_play`;
  `bridge_bar_fn` masks its transport-word reads; and `live_bridge_transport_start` makes arm+start
  one critical section.
- **S5 — classic notes recorded while leaving.** Covered by B2: `live_bridge_exit` finalizes the
  take at once (no `stop_at` wait) and the feed is LIVE-only.
- **S6 — undo order.** `song_io_undo` cancels a pending take only when no newer undo entry sits
  above it (`undo.cur <= take_pending_cur`); a newer clip edit is undone first.
- **S7 — MIDI fought take playback.** `live_bridge_launch_quant` is ignored while a take plays, and
  `live_bridge_transport_stop` stops take playback too.

### NITs

- The stored-take loader validates `hdr.nevents` against the record length and that the event
  samples are non-decreasing (`song_store.c`).
- The parameter-coalescing path never writes past `stop_at`.
- An arm-then-cancel (the transport was playing but the bar never came) restores the song's in-RAM
  take: `live_bridge_take_stop` sets `g_take_reload_req`, polled by `live_take_tick`.
- SEL stops take playback as the spec says (`live_handle_button`).

### RAM

| build | pool | spare |
|---|---|---|
| release | 335,556 / 344,064 | **8,508** |
| debug (`X0X_DEBUG=1`) | 335,556 / 344,064 | **8,508** |

The debug pool is freed with debug-only levers: `tools/build.py` drops the debug `ENG_FM6_VOICES`
from 6 to 4 and `rc_debug.c`'s `rc_pat` moves from `.pool` to `.bss` (both measurement-only; no
user-facing buffer was cut). `TAKE_MAX_EVENTS`, the perform history and the delay line are
unchanged.

## r2: the Opus re-review findings on fdbfa34

B1's r1 fix (the take sink never pushes a ring or reads `song->clip[]`) holds and is kept. r2 adds
the hard invariant, fixes the four remaining race/ordering holes, and turns "replay silently
diverges" into "a launch is dropped and counted".

### The hard invariant

`cur[role]` and `play[role]` always describe the same clip. `clip_engine_take_launch()` queues a
launch only when its staged copy is in `next[role]` (`next_set[role] && next_clip[role] == clip`);
otherwise it drops the launch, leaves `cur`/`play` untouched, and bumps `take_drop`.
`apply_role()` enforces the same at the boundary: it swaps `next -> play` and sets `cur` together, or
drops. The counter is visible in the take report (`takedump`'s header, and a `expect takedrops`
query) and in `rc_debug`'s ED_STATS (`RC_ST_TAKEDROP`, `tools/rc_probe.py` updated).

### Blockers

- **1 — the `take_play` race.** `live_bridge_take_play` now resets the transport, starts the take,
  holds the take's tempo and stages the offset-0 prelude launches **inside one `engine_crit` section,
  before `playing = 1`**. The main loop can no longer publish PLAY before the staged copy exists, so
  the first ISR block fires the prelude launches against valid copies. (fdbfa34 staged after the
  critical section.) The false "the ISR never sees PLAY before … a stale staged copy" comment is
  rewritten. Test: `unit-live`'s `r2_b1_play_race_test` (the hook fires the ISR at the play point).
- **2 — the recorded tempo never replayed.** `clip_engine_take_bpm()` now sets a take-owned hold
  (`bpm_take_on`); the render's song-tempo check must not clobber the take while it is on, and an
  edit to `song->bpm_x10` mid-replay is ignored. `clip_engine_take_bpm_hold()` arms the hold at
  `take_play` and `clip_engine_take_bpm_clear()` restores the song's tempo at the take's end and at
  `live_bridge_take_stop_play` (both ISR-safe: no ring). Test: `unit-live`'s `r2_b2_tempo_test`.

### Should-fix

- **3 — one `next[]` slot per role.** `live_bridge_take_stage` now stages a recorded launch only once
  its event is within `TAKE_STAGE_MARGIN` (`CLIP_ENGINE_FS / 32`, ~two UI frames) of the replay
  playhead, instead of staging the first launch after `play_i` however far off (fdbfa34). A later
  launch for the same role therefore no longer overwrites the copy of a queued launch that has not
  applied yet, and a launch that is genuinely missed is dropped by the invariant rather than
  corrupting `play[]`. The recorded launch now also carries its quantize in the event
  (`live_bridge_launch` posts `TP_LAUNCH_Q`), so `clip_engine_take_launch`'s `quant == 0xFF` path no
  longer reads `next[role].quant` from a since-overwritten copy. Test:
  `unit-live`'s `r2_s3_next_stage_test` (A queued at the bar, then NOW-B; B must play from B's copy).
- **4 — `take_finish` from the main loop was unmasked.** `live_bridge_take_finish` (and so
  `live_bridge_take_live(0)`, `live_bridge_transport_restart`/`_continue`) now runs `take_finish`
  inside `engine_crit`, so an ISR `take_feed` cannot land between `ended = 1` and `state = IDLE` and
  clobber the END mark or `n`. Test: `unit-live`'s `r2_s4_finish_race_test` (the hook runs the ISR
  at that exact window inside `take_end`).
- **5 — S6 failed with a full undo ring.** The pending take is now ordered by a monotonic position
  stamp `undo_stack_serial()` = `base_ser + cur` (bumped even when the full ring drops the oldest
  entry, where `cur` alone saturates), not by `undo.cur`. A newer clip edit is undone first; the take
  goes when nothing newer is above it. Test: `song_io_test`'s `test_take_undo_order_full_ring`.
- **6 — race tests did not model preemption.** A host-only hook runs the ISR at injected points
  inside the main-loop take functions: `live_bridge_test_hook` (at the `take_play` PLAY point and
  between `take_stage` roles) and `take_test_hook` (inside `take_end`). The handler models a real
  IRQ: it does nothing while `plat_crit_active()` (which is why the fixes hold), else runs one audio
  block's ISR half or a single ISR feed. `unit-live` now links `host/plat_crit_host.c` so the
  critical section is the nest-safe host model. `live_bridge_test_consistent()` is the invariant
  probe (cur/play byte-for-byte).

### NITs

- `ui_live.c`'s take-card role parse masks the role with `0xFF` (a Program Change launch carries its
  quantize in bits 8-9, so `quant > 0` no longer hid the lane).
- MIDI Start/Continue during take playback now stop the take playback cleanly first
  (`live_bridge_take_stop_play`) before the transport restart/continue, so there is no desync.
- `rc_debug.c`'s header comment says the debug FM6 cap is 4 (it was cut from 6 in r1), matching
  `tools/build.py`.

### Proof the race tests fail on fdbfa34

The three race tests are written against the fix. To show they catch the reviewed behaviour, the
fix hunks were temporarily reverted to fdbfa34's semantics (staging after the `engine_crit` in
`take_play`; no invariant in `take_launch`; unconditional `cur` in `apply_role`; `take_bpm` setting
only `bpm_src`; `take_finish` unmasked; the "que owns" staging), the tests rebuilt, and reverted
with `git checkout`. `make unit-live` then reports:

```
FAIL tests/host/live_bridge_test.c:201 live_bridge_test_consistent()   # r2_b1
FAIL tests/host/live_bridge_test.c:229 live_bridge_bpm_x10() == 1400u  # r2_b2
FAIL tests/host/live_bridge_test.c:231 live_bridge_bpm_x10() == 1400u  # r2_b2
FAIL tests/host/live_bridge_test.c:270 live_bridge_test_consistent()   # r2_s3
FAIL tests/host/live_bridge_test.c:295 live_bridge_take_nevents() == n0 + 1  # r2_s4
```

(The S5 test similarly fails when `song_io_undo`'s condition is put back to `undo.cur <=
take_pending_cur`: `FAIL tests/host/song_io_test.c:467 song_io_take_pending()`.) With the fix in
place all pass, and the t9i scenarios' `eventscmp` is still exact (the recorded launch encoding
changed from `TAKE_LAUNCH_QUEUE` to `TAKE_LAUNCH_QUANT` with the same quantize, so the replayed
sample stream is unchanged; only `takedump`'s header gains `drops 0`).

### RAM

| build | pool | spare | margin over 8 KiB |
|---|---|---|---|
| release | 335,564 / 344,064 | **8,500** | 308 |
| debug (`X0X_DEBUG=1`) | 335,564 / 344,064 | **8,500** | 308 |

r2 adds only bookkeeping: `clip_engine_t` grows by `bpm_take_on` + `take_drop` and
`undo_stack_t` by `base_ser` (8 B total). No FIFO/copy was added — the lookahead staging reuses the
existing one `next[role]` slot — so no user-facing buffer was touched. Both builds keep the 8 KiB
rule.

### Ladder

`make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` all pass on the committed
tree, and `tools/check_loader.sh` is ok on both packages (release and debug). Nothing was flashed.

## r5: recover after a dropped launch and preserve the deferred redo tail

Started at `515bc3a`, already merged with integration including TCLS. Implementation commits:
`c754208` (event-indexed FIFO, deferred undo, tempo restoration) and `5f1ef14` (unmasked initial
scan and scan-work test). The reviewer's untracked `repro_r5/lbt.c` and `repro_r5/ust.c` cases
were folded into the existing host suites as assertions, then `repro_r5/` was deleted.

### Fixes and proof

- **A dropped launch no longer poisons the role.** Each FIFO slot carries its take event index
  (`ts_event`), preserved whenever slots shift. The stager starts at the greater of its cursor
  and `play_i`, and rechecks `play_i` under `engine_crit` before publishing. The ISR discards
  obsolete candidate copies before matching both event index and clip. A previously queued
  launch retains its own copy until application or supersession. Missing copies still drop and
  increment the counter; the ISR never reads `song->clip[]` or pushes a main-loop ring.
  `unit-live`'s `r5_drop_recovery_test` replays NOW launches of drums clips 0/1/2 at samples
  200/300/400, then clips 0/1 at 50,000/100,000 with 4,352-sample UI frames. It requires exactly
  **one drop**, checks both later playing clips, and checks `cur/play` consistency every block.
  Against the original `515bc3a` sources it reports **three drops** and fails the later clip-0
  and drop-count assertions; with the fix it passes, including ASan/UBSan.
- **Deferred take flush at `pos == cur` keeps redo.** Only `undo_stack_push_take_at` supplies
  `keep_tail`; ordinary edit pushes still discard a redo branch. A deferred insertion at or
  behind the cursor shifts the cursor with the applied entries. `unit-store`'s new
  `flush at cursor` case ends a take after E0, creates and undoes E1, then flushes the take entry.
  It requires `redo_type == UNDO_CLEAR_CLIP` (not `UNDO_NONE`) and successfully redoes E1.
  Both redo assertions fail against `515bc3a`; the fixed suite and sanitizer suite pass, as do
  the existing ordinary-edit branch and r4 behind-cursor redo tests.
- **Scans run outside `engine_crit` and cache exhaustion.** The shared immutable-log scanner
  finds initial launch indices before the atomic reset/PLAY/copy publish. During playback only
  the state recheck and slot write mask interrupts. An exhausted role stores `n` as its staging
  cursor. `unit-live`'s `r5_scan_test` observes every scanned event, requires interrupts enabled
  during the initial scan, and requires zero extra scan work over ten later staging calls for a
  take with no launches. It passes normally and with sanitizers. With the same observational
  hook added to the baseline scan (no behavior change), the baseline fails the interrupt-mask
  checks and the exhaustion assertion.
- **FIFO synchronization comment corrected.** `clip_engine.h` now states that snapshot copies
  and staging writes are guarded by the caller's short `engine_crit` sections and the ISR never
  waits. The scan-mask test and existing ISR/preemption tests remain passing.
- **Tempo restoration accounts for edits made during the take.** With no pre-take external
  clock, the song's current `bpm_x10` wins at end/stop. With a followed clock, the saved clock
  returns even if the song was edited before the next render; clearing the hold also updates
  `song_bpm` so that next render cannot mistake the take-time edit for a fresh edit and remove
  the restored clock. Documented in `model.md`. `unit-live`'s `r5_tempo_restore_test` checks both
  source cases immediately after stopping and after another render; the clock case's later
  render fails on `515bc3a`. The existing r4 take-owned-tempo test still passes.

### RAM

| build | pool | spare | margin over 8,192 B | `.data+.bss` |
|---|---|---|---|---|
| release | 301,940 / 344,064 | **42,124** | **33,932** | 72,864 / 98,304 |
| debug (`X0X_DEBUG=1`) | 301,940 / 344,064 | **42,124** | **33,932** | 73,476 / 98,304 |

The ten 16-bit FIFO event indices add **20 B** of pool over r4. Both builds exceed the required
8,192 B spare; no buffer was shrunk.

### Ladder

On the committed implementation tree (`5f1ef14`),
`make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` exits successfully.
All scenarios pass, screen lint reports **0 findings** across 1,351 frames / 2,750 canvases,
the full UI fuzz passes **200 seeds × 2,000 events**, and check's short fuzz passes
20 seeds × 500 events. Both `tools/check_loader.sh build/x0x.fwsc` and
`tools/check_loader.sh build/debug/x0x.fwsc` pass: the packages retain the X0X 1.0.5 loader.
Nothing was flashed. This report is the only change after that ladder.


## r3 (the third Opus review)

The reviewer's repro harnesses `rv3/late.c` and `rv3/u.c` are now committed host tests that fail on
`2f41b83` and pass here: `unit-live`'s `r3_s1_late_press_test` / `r3_s2_stall_test` (with the
`r3_stage_hook_test` and `r3_ext_clock_test` alongside), `unit-store`'s undo-stack branch test, and
`unit-songio`'s `test_take_undo_order_flush`. Nothing was flashed.

- Files: `core/seq/clip_engine.{c,h}`, `firmware/src/app/live_bridge.c`, `core/seq/take.c`,
  `firmware/src/app/song_io.c`, `core/model/undo_stack.{c,h}`, `docs/engineering/model.md`,
  `tests/host/live_bridge_test.c`, `tests/host/undo_stack_test.c`, `tests/host/song_io_test.c`.

### Should-fix

- **1 — the late press dropped the earlier queued launch.** A (bar) is queued, C (bar) pressed 11 ms
  after A's boundary; the replay staged C up to 31 ms early and *stole* `next[role]`, so A was
  applied against C's bytes and dropped (`take_drop`). `live_bridge_take_stage` now never overwrites
  `next[r]` while a *different* launch is queued but not applied, unless the event is due before the
  queued launch's boundary (it will be replayed first and supersede it). `clip_engine_step_sample`
  gives that boundary sample from the transport under the caller's mask. The wrong `TAKE_STAGE_MARGIN`
  comment is rewritten.
- **2 — a stalled UI dropped a launch.** The r2 ~28 ms lookahead window is smaller than a 77 ms
  full redraw, so the main loop skipped straight over it. `live_bridge_take_stage` now stages a role's
  next un-replayed launch **as soon as the role has no pending queued launch**, however far ahead;
  the margin is used only for the overwrite case above. To make the one `next[]` slot free once a
  queued launch no longer needs it, `clip_engine_take_launch` commits the staged copy to `play[]`
  when nothing is playing for the role (`que_ready`; the bytes are silent until `cur[role]` is set),
  and `apply_role` starts the queued launch from there when `next[]` has moved on. This keeps the r2
  invariant `cur[role] == song->clip[cur]` byte-for-byte. `r3_s1_late_press_test` (A heard and zero
  drops in every 60 Hz phase) and `r3_s2_stall_test` (zero drops at 40 ms and 100 ms frames, all
  phases) pin both; a sweep over all 705/1764/4410 phase offsets is clean.

### NITs

- **Injection hooks.** The `take_play` hook ran inside the masked section (a no-op); it is moved to
  just after `engine_crit_exit`. The `take_end` hook likewise moved out, to just after
  `live_bridge_take_finish`'s critical section, where an IRQ can really run (the closed take ignores
  the later feed). The `take_stage` hook (already outside the per-role mask) is now armed by
  `r3_stage_hook_test`, which proves it fires between roles.
- **Tempo clear mask.** `live_bridge_take_stop_play`'s `clip_engine_take_bpm_clear` (pure arithmetic
  over the ISR-read transport words) runs inside `engine_crit`.
- **External clock vs the take.** An external clock the user is following now wins over the take's
  recorded tempo (`clip_engine_take_bpm` leaves the running clock tempo alone when `bpm_ext` is set),
  and `clip_engine_take_bpm_clear` restores that followed clock instead of zeroing `bpm_ext`. Pinned
  by `r3_ext_clock_test`; documented in `model.md`.
- **Take undo order.** The durable take entry is pushed at the position it had when the take ended
  (`undo_stack_push_take_at` with `take_pending_ser`), not at the deferred flush, so a clip edit made
  after the take is undone first. Test: `unit-songio`'s `test_take_undo_order_flush`.
- **Pre-existing T6c bug (redo tail).** `undo_push` never discarded the entries above `cur`, so edit
  v0->v1, undo, edit v0->v2, undo, undo restored the discarded v1. `undo_push_at` now truncates the
  redo tail before every push. Test: `unit-store`'s branch test (the `u.c` harness).

### Proof the r3 tests fail on 2f41b83

`tests/host/live_bridge_test.c` compiled against `2f41b83`'s `live_bridge.c`/`clip_engine.c`/`take.c`
reports 16 failures (the take_stage hook never fires, the clock is overridden by the take, A is
dropped and C dropped in every phase); `tests/host/undo_stack_test.c` against `2f41b83`'s
`undo_stack.c` fails *the discarded redo is not undone again*; `tests/host/song_io_test.c` against
`2f41b83`'s `song_io.c` fails three checks in `test_take_undo_order_flush`.

### RAM

| build | pool | spare | margin over 8 KiB |
|---|---|---|---|
| release | 335,572 / 344,064 | **8,492** | 300 |
| debug (`X0X_DEBUG=1`) | 335,572 / 344,064 | **8,492** | 300 |

r3 adds one byte per role of engine state (`que_ready[NROLES]`, 8 B aligned) and no buffer. Both
builds keep the 8 KiB rule.

### Ladder

`make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` all pass on the committed
tree, and `tools/check_loader.sh` is ok on both packages (release and debug). Nothing was flashed.


## r4: the two-deep take staging FIFO, the take-owned tempo, and the undo-flush NIT

Merges TCLS (`6fefb2a`: Classic removed, the X0X sequencer and its ISR MIDI-in deleted, the delay
shrunk to 0.75 s, ~39 KB of pool freed) into this branch first (`git merge --no-ff integration`,
`d652219`), then fixes the r1-r3 staging bug structurally. The merge kept T9i's code on every
conflict: `engine.c`'s `apply_raw`/`engine_take_cmd` and the take pump stay, X0X's `midi_in`/`SINK`
go; `ui_live.c` keeps the take state and drops the `classic`/`back_classic` branches;
`rc1_probe.{x0x,md}` drop the stress-1 and stress-6 levels. The T9i take goldens were re-seeded for
the TCLS boot song (`76ca9bc`: the STARTER clips and the Classic-free UI change the take scenarios'
`leds`/song-name output; every scenario assertion still passes). Nothing was flashed.

- Files: `core/seq/clip_engine.{c,h}`, `firmware/src/app/live_bridge.c`, `core/model/undo_stack.c`,
  `tests/host/live_bridge_test.c`, `tests/host/undo_stack_test.c`, `docs/engineering/model.md`
  (plus the merge's `engine.c`, `ui_live.c`, `rc1_probe.*` and the goldens).

### The structural fix: two staging copies per role

r1-r3 shared one `next[role]` staging slot: the main loop could only stage a launch's copy after the
previous one applied, so a launch that followed a queued one within a single UI frame had no copy
staged in time and was dropped (the reviewer's playing-role repro). r4 gives each role a **2-deep
staging FIFO** (`clip_t ts[role][2]`, `ts_clip`, `ts_n`, `ts_queued`) owned by the clip engine:

- The main loop (`live_bridge_take_stage`) fills the free slots with the next un-staged launch
  events in take order, as far ahead as the two slots allow. It only ever writes an empty slot.
- The ISR (`clip_engine_take_launch`) takes the copy from the front (slot 0) when no launch is
  queued for the role, or from slot 1 when a queued launch is still waiting (that event overrides
  it). A copy that is not staged drops the launch and counts it (`take_drop`).
- `apply_role` pops slot 0 into `play[]` at the boundary, then slot 1 becomes slot 0. The invariant
  `cur[role] == the clip in play[role]` holds byte-for-byte (`live_bridge_test_consistent`).
- `clip_engine_take_stop` discards a superseded queued copy; a relay launch consumes both staged
  copies; `clip_engine_take_stage_clear` drops every copy at a take end/stop and at the reset.

The one-slot `que_ready` / overwrite-guard / pre-commit special cases are gone; the FIFO subsumes
them. Live (non-take) user launches keep their existing `next[]` path (`launch_cmd`/`apply_role`).

### Tempo during take playback (reverses r3)

The take's recorded tempo now wins. `clip_engine_take_bpm`/`_hold` save the pre-take `bpm_ext`
(`bpm_ext_saved`) and set the recorded tempo unconditionally; `consume_cmds` ignores both
`CLIP_CMD_SET_BPM` (the song/UI edit) and `CLIP_CMD_SET_BPM_SOFT` (the external clock) for the
transport while `bpm_take_on` is set. At the end/stop `clip_engine_take_bpm_clear` restores the
saved source (the followed clock, or the song's tempo). `model.md`'s "replays at the clock's tempo"
text is replaced.

### NIT: the deferred take flush kept the redo tail

`undo_push_at` truncated the redo tail (`u->n = u->cur`) on every push. When the deferred take
flush inserted the take entry *behind* `cur` (a clip edit made after the take, then undone), the
truncation silently dropped that edit's redo. The truncation now runs only for a top push
(`pos >= cur`); an insert behind shifts `cur` up with the entries above it, so the redo tail
survives.

### Tests and the proof

`unit-live`'s `r4_playing_role_test` is the reviewer's repro: `r3_late_run` with a prelude
`TAKE_LAUNCH_NOW` of clip 1 on drums, then A (clip 0, bar) and C (clip 2) `D` samples after A's
boundary, swept over every phase at 16/40/100 ms UI frames for `D` = 500 and 3,000. The existing
silent-role tests (`r3_s1_late_press_test`, `r3_s2_stall_test`) still pass; all report zero drops
and a consistent invariant. `r4_tempo_test` pins the take-owned tempo (a clock change and a UI edit
mid-replay are ignored; the pre-take clock returns at the stop), and `r4_relay_test` pins the relay
cancellation through the FIFO. `unit-store`'s new flush test pins the redo tail.

Compiled against `e3e54f8`'s `live_bridge.c`/`clip_engine.c`/`take.c` (with the new test file), the
playing-role repro fails in **every** phase — 28 `sawA` and 28 `drops == 0` check failures across
the 28 runs — and `r4_tempo_test` fails its four checks (the old code let the clock win).
`unit-store`'s flush test against `e3e54f8`'s `undo_stack.c` fails *the redo tail is kept* and *the
tail redoes*.

### RAM

| build | pool | spare | margin over 8 KiB |
|---|---|---|---|
| release | 301,920 / 344,064 | **42,144** | **33,952** |
| debug (`X0X_DEBUG=1`) | 301,920 / 344,064 | **42,144** | **33,952** |

The FIFO adds two `clip_t` per role (10 × 528 B = 5,280 B, plus 15 B of indices): 296,628 → 301,920 B
used. TCLS's ~39 KB freed keeps both builds far above the 8 KiB rule. No user-facing buffer was cut.

### Ladder

`make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` all pass on the committed
tree, and `tools/check_loader.sh` is ok on both packages (release and debug). Nothing was flashed.
