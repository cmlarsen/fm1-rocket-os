# TFIXA — audio and take review fixes

Implemented from `docs/engineering/tasks/TFIXA.md`, after reading the audio,
engine and verify-audio reports and the architecture invariants. No hardware was flashed.

## Findings and regression evidence

Each implementation group began with a failing regression. Tests use production
clip/take/bridge code; the engine scenario assertions inspect the actual DSP gates.

| Finding | Change | Regression and observed failure before the fix |
| --- | --- | --- |
| Audio 1 / engine 1: safety admission | The clip ring reserves two slots; transport/role Stops and mute-clear also have an ordered overflow latch. Later ordinary commands are refused until the latch drains. The audio ring reserves 256 release slots, returns admission, counts refused ordinary commands and uses an after-drain panic if releases themselves exhaust it. All-off remains a guaranteed flag. MIDI overflow requests panic. | `tfixa_checks` failed saturated Stop; a later role-Stop test failed all five releases under saturation. `tfixa_midi_stop.x0x` exercises real MIDI Start + 14 PCs + Stop in one poll. `tfixa_engine.x0x` originally reported five failures, including the FM6 release after 1,100 parameter pushes. |
| Audio 3: take start ordering | All-off is published inside the PLAY critical section, before playing becomes visible. | `tfixa_take_tests` injects an IRQ in the old unlock gap and checks the all-off publication; it failed before the move. |
| Engine 2 and engine 3 baseline staleness | Capture the playing roles' steps at the recording boundary, after advancing roles and before firing notes. Store a phase-prelude launch and restore its step when the staged copy becomes playing. The boundary rebuild also refreshes role/mute/tempo setup. | `tfixa_phase_test` failed note-stream equality for a two-bar clip launched on global step 3, with recording starting on step 16. `tfixa_phase.x0x` compares the actual engine's recorded/replayed note command streams using `eventscmp`. |
| Audio 2: mono legato | Every MIDI, clip and panel release carries its pitch; the audio owner closes a 303 gate only for that pitch. Explicit unconditional release uses pitch marker 255. Remove the panel's old last-key suppression, which conflicted with this rule. | The engine assertions failed BASS/LEAD older-note releases through both the bridge and clip dispatch. `tfixamidimono` exercises the real MIDI parser and held table. Long fuzz found the newest-first panel release conflict; `tfixa_panel.x0x` failed first, then passed with the minimized seed-51 reproduction. |
| Engine 4: Start/Continue staging | Protect both queued and not-yet-consumed launch snapshots during relaunch staging. | `tfixa_checks` failed both launch-13 followed by Restart and launch-13 followed by Continue. |
| Audio 5 / engine 5: natural take end | Keep the final interval playing until clip/audio rendering reaches the take length; publish note releases and all-off at that boundary, then clear staging and tempo ownership. | `tfixa_take_tests` failed to emit the drum launch in a 128-frame take before the fix. |
| Engine 7: rejected launches recorded | Record launch intents only after successful admission, inside the same critical section. | `tfixa_take_tests` found a recorded launch for unused clip 13 before the fix. |
| Audio 6: command/intent publication gap | Publish launch, role-stop, mute, tempo and FX intents with their commands under the same mask. Tempo/mute/stop refusals also suppress the intent. | `tfixa_intents_test` injects an IRQ before posting the tempo intent and checks its stamp against the command's actual consume block; it failed before masking publication. |
| Audio 7: silent take-intent overflow | Count intent drops and let the ISR close a recording with its full/incomplete indication and END marker on overflow. | A 40-intent burst in `tfixa_intents_test` previously left recording active with no full indication. |
| Engine 9: Stop then Start | Consume Stop's save/release transition immediately, so the subsequent Start resets the transport. | `tfixa_checks` found elapsed 101 instead of 1 before the fix. |
| Engine 11: UINT32_MAX deadline | Return deadline presence separately from the timestamp. | Near-wrap `tfixa_checks` failed to release the note at offset 255 before the fix. |
| Engine 10: wide division | Use bounded 32-bit tempo rescaling (maximum product 36,000 squared), with a compile-time sample-rate bound. Add `no-64-div` to `make check` for core and app C/headers. The conservative source guard checks casts, named values, typedef aliases, pointers, comma declarations and wide literals. | `no_64_div_test.py` initially failed on production `set_bpm`; fixtures verify division/modulo rejection and allowed 32-bit arithmetic. |
| Take compatibility | New take envelopes are v2; song envelopes remain v1. Validate phase launches, accept v1 takes, and migrate their pitchless mono releases to marker 255. | Store regressions initially failed the v2 envelope, invalid-role rejection and legacy release migration. V1 envelope/readback and phase roundtrip also pass. |

The only golden changes are intentional: the ring burst admits two fewer ordinary
commands, and the live-note take log now carries the released pitch instead of zero.
No PNG golden changed.

## Decisions and skipped work

The coordinator approved phase plus the boundary note stream, without reconstructing
remaining durations of notes that began before recording. Those notes' remaining sustain
is accepted as lost on replay; this is documented in `docs/engineering/model.md`.
The ISR baseline callback writes the take log from ISR-owned clip snapshots; it performs
no ring pushes, editable song-clip reads or waits. Main-loop take queries retain the
existing mask/snapshot contract.

- **Audio 4 / engine 6: arbitrary intra-block take offsets.** Deferred: a full ordered
  take/clip/DSP/perform timeline is a render-path redesign. The verification report
  downgraded the recorded-device effect to a block shift. Phase/boundary tests pass,
  but arbitrary nonzero-offset take events still apply at the block start.
- **Engine 8: three dense same-role replay launches.** Deferred: the two-copy staging
  FIFO still drops/counts a third launch without a producer pass. Fixing this requires
  a different snapshot/admission design; existing drop-recovery coverage is retained.
- **Engine 3: regridding an armed take after tempo changes.** The baseline is refreshed
  at the reached boundary, but the arm's target sample remains fixed; dynamic regridding
  needs a separate transport/recording policy. Pending FX state in the arm interval
  also retains the existing block-based perform semantics.
- **Audio 8: first failed core-1 job deadline.** Deferred: deadline-derived HAL recovery
  requires device fault injection and timing, outside this small host-testable fix.
- **Audio 9: bulk critical-section duration.** Deferred: reset/prelude/copy publication
  requires a preparation/publication redesign or device measurements. No claim is made
  that host tests establish the panel scanner's worst-case mask time.
- **Perform engage refusal versus recording.** The perform ring's existing void engage
  admission remains; its guaranteed release/reset behavior is unchanged. General
  command-consumption-based recording would address this with the timeline redesign.

## Verification

Code is committed through `06388aa`; final committed-tree host units, scenarios and
screen lint passed. The final long fuzz and firmware/loader ladder is in progress;
this section will be replaced with its measured results before completion is reported.

## r1 — Opus review follow-ups

The Opus r1 review's repros (`repro_r1/`) are folded into the committed tests and the
directory is removed. The branch merged `integration` (only `tools/orca/health.py`).

| Finding | Change | Test that failed before the fix |
| --- | --- | --- |
| **B1** MIDI Stop→Start silences a role after a cancelled launch | In `consume_cmds` the second-press cancel clears the matching staged copy (`next_set[role]` when `next_clip[role] == target`), so `relaunch_stage()` no longer skips the role. The write is ISR-side; the main loop's staging that could set it runs under `engine_crit`. | `clip_engine_test.c` `tfixa_checks`: launch 12, launch 13, launch 13 again (cancel), Stop, Start → `cur[LEAD] == 12`, `take_drop == 0`. Before: `cur=255 drop=1`. |
| **S1** the ISR baseline wiped offset-0 events | `take_refresh_setup` recaptures only the dynamic state **in place** (`n = 1`, rewrite `[1, state_n)`); the parameter mirror and any events already appended at offset 0 survive. | `live_bridge_test.c` `tfixa_stop_at_start_test`: a role Stop drained in the block whose base is the take start is recorded (`lead_stop_after_phase()`). Before: not recorded. |
| **S2** the ISR baseline ran the ~300-event parameter prelude | The prelude is split: `bridge_prelude` (clip/phase per role, mutes, tempo, FX) runs at arm and again at the boundary; `bridge_prelude_params` (the sound mirror) runs once at arm on the main loop. `take_set_prelude_params` registers it. | `live_bridge_test.c` `tfixa_isr_prelude_test`: counts `take_put()` calls in the start block; must be `<= 2*NROLES + 8`. Before: the mirror was included. |
| **N1** non-atomic safety-latch RMW | `push_cmd` runs the latch read-modify-write inside an installable interrupt mask (`clip_engine_set_crit`, installed by `live_bridge_init` with `engine_crit_enter/exit`; a null hook is a no-op for the standalone host units). | `live_bridge_test.c` `tfixa_latch_crit_test`: a counting hook proves a push takes the mask. |
| **N2** the 64-bit lint missed the fast/least/`__` types | `WIDE` in `tools/check_no_64_div.py` also matches `u?int_fast64_t`, `u?int_least64_t` and `__u?int64_t`. | `tests/lint/no_64_div_test.py` folds the reviewer's probes (`uint_fast64_t`, `uint_least64_t`, `__uint64_t`, the `unsigned long long` cast, the struct member) as positives and the multiline-return probe as a negative. |
| **N3** `take_arm_if_pending()` read `g_eng` outside `engine_crit` | `live_bridge_transport_restart/continue` call it inside the same `engine_crit` section as the transport command (it already is for `transport_start`). | Structural (its `g_eng` reads in `bridge_bar_fn`/`take_arm` are now masked on every call path); no dedicated test. |

### Measured ladder (committed tree)

`make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` all pass;
`tools/check_loader.sh` passes on `build/x0x.fwsc` and `build/debug/x0x.fwsc`. Release
and debug both report pool spare 42,088 B (`301,976 B of 344,064`) and no forbidden
symbols. No golden file changed: the arm prelude order (`state` then `params`) is
unchanged, and the boundary refresh now keeps the arm-time parameter mirror (identical
values in the scenarios) instead of rewriting it in the ISR. No hardware was flashed.
