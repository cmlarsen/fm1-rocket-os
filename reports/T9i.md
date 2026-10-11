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
