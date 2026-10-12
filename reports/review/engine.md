# Engine review

Review-only lane: `core/seq/clip_engine.[ch]`, `core/seq/take.[ch]`, `core/midi/*`, and their integration in `firmware/src/app/live_bridge.c`. Read CLAUDE.md, docs/README.md, architecture.md and interaction-model.md first. Reviewed the engine.c call ordering only to establish how the bridge runs. No product code, tests or documentation changed; no hardware flashed.

**11 findings: BUG 8, EDGE 3, SIMPLIFY 0, CLARITY 0, MAINTAIN 0. Severity: high 2, medium 8, low 1.** The top three issues are a lost transport Stop on command-ring saturation, incorrect clip phase in the take prelude, and a prelude that reflects arming rather than recording start.

## Findings, ranked

### 1. EDGE / high — a full command ring can lose Stop and restart the instrument

**Location:** `core/seq/clip_engine.c:352–355`, `:367–368`, `:779–795`.

`transport_stop()` writes `playing = 0` but ignores a failed enqueue; an earlier queued Restart/Continue/Start can subsequently write it back to 1. Thus Stop does not necessarily win, contrary to the function's ordering contract. Other void command APIs also hide refusal from callers.

**Scenario:** stopped engine; enqueue MIDI Start, then 14 soft-tempo commands (all 15 usable ring slots occupied), then MIDI Stop before an audio drain. Stop's command is rejected; the ISR consumes Start and resumes playing. Notes can continue after the user/external clock stopped the instrument.

**Repro: ran, ASan/UBSan.** Key calls: `clip_engine_restart(&e); for (int i=0;i<14;i++) clip_engine_queue_bpm_soft(&e,1200); clip_engine_transport_stop(&e); clip_engine_render(&e,1,&sink);`. Output: `full-ring Start then Stop: playing=1 (expected 0)`.

**Fix:** make transport stop lossless, using a dedicated ISR-observed stop request or reserved capacity with ordering semantics; expose enqueue success for other commands rather than silently discarding it.

### 2. BUG / high — take playback loses the phase of clips already playing

**Location:** `firmware/src/app/live_bridge.c:110–114`, `:175–195`, `:648–662`.

The prelude stores only a clip ID and `TAKE_LAUNCH_NOW`; playback resets the entire transport and starts that clip at step 0. Neither the role's position nor its phase is recorded. Starting recording on a bar does not imply that every clip is at step 0: clips can be 32/64 steps or have been launched out of phase. Once clips can similarly restart a section that was already consumed.

**Scenario:** a 64-step clip begins at transport step 0, with note 60 at clip step 0 and note 76 at step 16. Arm REC shortly after start, so recording begins at the next bar (step 16). Original recording begins with note 76; replay begins with note 60, and the rest of the clip remains offset.

**Repro: ran against the actual bridge, ASan/UBSan, using its existing test stubs.** Key sequence: `live_bridge_launch_now(12); live_bridge_transport_start(); run_isr_block(0,100); g_now=100; live_bridge_take_arm(); run_isr_block(100,88101); live_bridge_take_finish(); live_bridge_take_play(); run_isr_block(88201,128);`. Output: `record-start clip phase=16 note=76`; `replay first note=60 phase=0`.

**Fix:** capture and restore each playing role's phase at recording start, including any necessary sounding-note state, or define a recording-start policy that aligns clips and actually implement it. A clip ID alone cannot reconstruct the performance.

### 3. BUG / medium — the take prelude captures the arm-time state, then discards intervening changes

**Location:** `core/seq/take.c:149–168`, `:175–192`; `firmware/src/app/live_bridge.c:105–134`, `:554`.

`take_arm()` writes the offset-0 state immediately, although recording may start almost a whole bar later. Feeds while ARMED are dropped. Changes to clips, mutes, FX, parameters or tempo during that interval are therefore absent from both the prelude and the event log, despite being active when recording starts. Tempo changes can also make the fixed arm sample cease to be a bar boundary.

**Scenario:** arm at sample 100 for boundary 1000 while the lead is unmuted; mute it at sample 500; begin recording at 1000. Music is muted throughout the take, but replay's offset-0 mute remains off.

**Repro: ran, ASan/UBSan.** `take_set_bar_fn(&t,bar,0); take_set_prelude(&t,pre,0); take_arm(&t,100); take_feed(&t,500,TAKE_MUTE(2,1)); take_tick(&t,1000); take_finish(&t,1100);` with `bar()` returning 1000 and `pre()` writing mute off. Output: `events=3 mute_prelude=0` (start, stale mute, end; no mute-on).

**Fix:** publish a consistent baseline when recording actually starts; alternatively fold pre-start state changes into an arm snapshot. Keep bar alignment tied to transport boundaries when tempo changes while armed.

### 4. BUG / medium — Start/Continue staging overwrites a not-yet-consumed launch

**Location:** `core/seq/clip_engine.c:328–347`, `:391–396`, `:699–709`.

`relaunch_stage()` skips roles with `que_set`, but that flag is set only when the ISR consumes the command ring. A main-loop launch has already written `next[]` without setting `que_set`. Start/Continue in the same poll overwrites that copy with the current or saved clip. The queued launch then lacks its matching snapshot and is dropped.

**Scenario:** clip 12 plays; enqueue HOME launch of clip 13, then MIDI Start before the next audio render. Or stop/render, launch 13, then Continue in one poll. In both cases the newly requested clip disappears and clip 12 remains.

**Repro: ran, ASan/UBSan.** `clip_engine_launch_now(&e,13); clip_engine_restart(&e); clip_engine_render(&e,100,&sink);` gives `cur=12 next=12 drops=1`. Stopped launch followed by Continue likewise gives `cur=12 drops=1`.

**Fix:** protect staging belonging to pending ring commands as well as consumed queues; use command-owned snapshot identities or perform ordered relaunch staging when transport commands are consumed, without reading song clips in the ISR.

### 5. BUG / medium — natural take end silences the whole final audio window early

**Location:** `firmware/src/app/live_bridge.c:520–531`.

After advancing the take through a window, the bridge sets `g_eng.playing=0` and requests all-off before the clip engine renders that same window. The final window should still play up to the take's end sample. Instead, it is stopped at its beginning, up to one audio block early. This can suppress a last-window trigger entirely.

**Scenario:** a take is exactly 128 samples long and its prelude launches a melodic clip. The first take pump immediately observes `play_pos == length_samples`, clears staging and stops transport; clip render produces no initial note at all. Longer takes lose their final window in the same way.

**Repro: ran against the bridge, ASan/UBSan.** Load launch/tempo at sample 0 and END at 128; `live_bridge_take_loaded(3,128,1200); live_bridge_take_play(); run_isr_block(0,128);`. Output: `one-block take events=0 playing=0`.

**Fix:** render to the actual end offset before releasing voices/stopping transport; split the last window when the take ends inside it.

### 6. BUG / medium — the bridge discards replay event offsets

**Location:** `firmware/src/app/live_bridge.c:142–163`, `:520`; `core/seq/take.c:247–263`.

The standalone player passes an exact window offset, but `take_sink_ev()` explicitly ignores it. All replayed engine commands and clip controls are applied before clip rendering for the whole window. An event after a step boundary can quantize against the previous step and launch one beat/bar too early; multiple events within a window collapse onto its start. Non-block-aligned recording starts make ordinary block-stamped recorded actions non-block-aligned on replay.

**Scenario:** replay a launch at take offset 100 in its first 128-frame block. It is applied before transport initialization and becomes a step-0 launch at offset 0, before its recorded timestamp.

**Repro: ran against the bridge, ASan/UBSan.** Load `{100, TAKE_LAUNCH(12,TAKE_LAUNCH_NOW,2)}` and END at 10000; play and pump/render 128 frames. Output: `event100 replay note offset=0 count=1`.

**Fix:** interleave take events and clip/audio rendering at their offsets. Maintain transport position at each event before calculating its quantize target. Keep event-log tap formatting separate from actual audio scheduling.

### 7. BUG / medium — rejected live launches are still recorded and become real replay launches

**Location:** `firmware/src/app/live_bridge.c:1083–1089`, `:1068–1072`; `core/seq/clip_engine.c:389–392`, `:400–409`, `:501–560`.

The bridge posts take intent regardless of whether the clip engine accepted the command. An unused clip or a full command ring can reject a live launch, while the take ring accepts and records it. Replay staging checks the index but not `used`, and the take launch path does not repeat live validation. A previously nonexistent action then replaces the playing role on replay.

**Scenario:** with PC launch enabled, send Program Change 13 when that slot is empty. Live launch is rejected. Record enough time to reach the quantize point; replay launches empty clip 13, potentially silencing a previously playing role. Ring saturation similarly records actions that never happened.

**Repro: ran against the bridge, ASan/UBSan.** Arm/start recording; `live_bridge_launch_quant(13,Q_BEAT)` with `song.clip[13].used=0`; finish a 30000-sample take and replay through its beat boundary. Output: `unused live PC cur=255`; `unused replay PC cur=13`.

**Fix:** return acceptance from clip commands and record only accepted actions (ideally at the actual consume point). Validate staged take targets consistently, while preserving the ISR prohibition on reading editable song clips.

### 8. EDGE / medium — two staged copies cannot replay three dense same-role launches

**Location:** `core/seq/clip_engine.c:523–528`, `:633–646`; `firmware/src/app/live_bridge.c:653–660`, `:695–732`.

The main loop stages only two launches per role. Once the ISR has consumed/superseded those, a third event arriving before the next main-loop staging pass has no copy and is dropped. Live's 15-slot command ring accepts this density, so an otherwise valid recorded take does not reproduce the performance. MIDI Program Change bursts are an easy source; this is also dependent on main-loop timing. The existing bridge `r5_drop_recovery_test` deliberately expects one drop for launches at samples 200, 300 and 400, so current tests codify recovery rather than fidelity.

**Scenario:** three different same-role launches occur within one UI frame. Recording logs all three; replay accepts the first two and drops the third, leaving the wrong clip queued/playing.

**Repro: ran, ASan/UBSan.** Stage events 10/11 for clips 12/13, then call `clip_engine_take_launch()` for clips 12/13/14 at event indices 10/11/12 without a producer pass. Output: `accepted=110 drops=1`.

**Fix:** provide enough staging for the supported ingress burst, preload compact immutable clip references/snapshots for the pass, or enforce an explicit recording admission limit that guarantees replay. Counting drops keeps memory safe but does not preserve the take.

### 9. BUG / medium — Stop followed by panel Start in one drain does not restart transport

**Location:** `core/seq/clip_engine.c:792–799`, `:934–999`.

Transport state transitions are deferred until after all commands drain. Stop then Start leaves `playing=1` and `was_playing=1`, so the engine never visits its stop path and never resets to bar 1. The intermediate Stop also releases no notes. The public Start contract says starting from stopped resets transport.

**Scenario:** playing at sample 100, MIDI Stop arrives just before the user presses PLAY; both are consumed in one audio block. Expected new transport at sample 0; actual old transport continues at sample 100, with the old clips and voices.

**Repro: ran, ASan/UBSan.** `clip_engine_transport_stop(&e); clip_engine_start(&e); clip_engine_render(&e,1,&sink);` gives `elapsed=101 cur=12`, rather than fresh elapsed 1.

**Fix:** apply Stop's save/release transition when consuming Stop, or retain explicit ordered transition state so a following Start knows a stop occurred. Preserve Continue's distinct resume behavior.

### 10. BUG / medium — tempo changes perform forbidden 64-bit division in the audio path

**Location:** `core/seq/clip_engine.c:67`.

`(uint64_t)phase * new_l / e->step_L` is a runtime 64-bit division inside `set_bpm`, invoked from the ISR for tempo edits, clock follow and take tempo marks. This contradicts the hard CPU/build constraint and the header's assertion that every division is 32-bit. Bounds already imposed here make the wider arithmetic unnecessary: at default 48 kHz the maximum step length is 36000, so `phase * new_l <= 1,296,000,000`, fitting uint32_t (44.1 kHz is smaller).

**Scenario:** a running transport receives a tempo change; the target compiler must lower a 64-bit divide or call a runtime helper in the ISR instead of the promised 32-bit arithmetic.

**Proof:** source inspection plus host optimized assembly, ran: `cc -std=c99 -O2 -I. -S core/seq/clip_engine.c`. Apple arm64 assembly includes `udiv x11, x13, x11` / `udiv x12, x14, x12` for the widened rescaling, alongside normal `udiv w...` divisions. This establishes the operation width, not a measured pi32v2 latency; no firmware build/hardware run was performed.

**Fix:** use bounded uint32_t multiplication/division here, with a compile-time bound for supported CLIP_ENGINE_FS if configurability must remain.

### 11. EDGE / low — UINT32_MAX is both a valid note end and the no-note sentinel

**Location:** `core/seq/clip_engine.c:33`, `:174–180`, `:1004–1018`.

`note_next()` returns UINT32_MAX for an empty pool and for a real note whose scheduled end is exactly UINT32_MAX. Render excludes that value from deadline processing. Such a note is not released at its deadline; it waits until another note deadline or a clip step/loop/retrigger eventually releases it. Wraparound itself is otherwise handled with signed deltas.

**Scenario:** after long-running transport/tempo changes, a note ends at sample 0xFFFFFFFF and no other note deadline precedes the next step. It sustains across wrap instead of ending on time.

**Repro: ran, ASan/UBSan, using explicit near-wrap engine state.** `e.elapsed=e.step_sample=0xFFFFFF00; e.note[0].used=1; e.note[0].end=0xFFFFFFFF; clip_engine_render(&e,512,&sink);` gives `off_count=0 still_used=1`, despite its end being offset 255 in the window.

**Fix:** return a separate presence flag and deadline, or derive presence from pool occupancy; do not reserve an attainable timestamp.

## Checked and found clean

- Nominal Beat/Bar/End quantize arithmetic, queue cancellation, role replacement, Loop/Once completion, shortened clips, swing, mute releases and bounded note-pool admission passed the existing clip tests. Clip step/note counts are clamped before array access.
- The audio clip path reads `play[]`/`next[]` and take FIFO snapshots rather than `song->clip[]`; bridge launch/edit/staging copies use `engine_crit`. Take replay uses ISR-side APIs rather than pushing onto main-loop rings. The reported staging problems are logical admission/order failures, not an observed torn-copy race.
- MIDI parser: channel message lengths (including one-byte Cx/Dx), running status, System Common cancellation, stray data/status recovery, velocity-zero Note Off, channel/cable routing, and realtime bytes inside messages/SysEx. Status bytes abort malformed SysEx and recover; the comment saying all non-realtime bytes are dropped until F7 is stricter than the implemented recovery, but no actionable parser bug found. Existing 20 MIDI tests pass.
- MIDI clock: integer tempo estimator, isolated dropped-tick handling, resync and wrap subtraction inspected; existing tests pass. The sample-clock seqlock reader has bounded retries and a previous-snapshot fallback; its interleaving/fuzz tests pass, with no interrupt wait found.
- Take core's ordinary arm/stop/full-buffer/end-marker paths, offset-based recording across counter wrap and standalone sample-offset playback pass its tests. The bridge fidelity failures above are not covered by those standalone guarantees.
- No heap allocation, double arithmetic or wall-clock reads found in these core modules. No flash-writing operation exists in this lane; save/power-loss storage correctness is outside this review.

## Validation and limits

Ran the existing clip_engine, take, midi_in and sample_clock host suites at `-O2 -Wall -Wextra -Werror`; all passed. Ran all four again with ASan/UBSan, plus the existing live_bridge host suite with ASan/UBSan; all passed. Ran additional throwaway `/tmp` core and actual-bridge harnesses with ASan/UBSan; their observed failures are quoted above. The bridge harness reused the existing test's engine/critical-section stubs, so it proves scheduling/state behavior rather than DSP acoustics. No repro/test file is committed, no goldens were updated, and no device timing or firmware build is claimed.
