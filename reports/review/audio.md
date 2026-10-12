# Audio lane review

Reviewed 2026-10-11. Scope: product paths in `firmware/src/app/live_bridge.c`, `engine.c`, `midi_live.c`, `main_fm1.c`, and `plat_fm1.c`; supporting clip/take/perform/ring code was read to establish caller contracts. Read CLAUDE.md, the glossary, architecture invariants, and interaction model first. No product code, tests, or documentation changed; no hardware flashed.

**9 findings: BUG 5, EDGE 3, SIMPLIFY 0, CLARITY 0, MAINTAIN 1.** Findings are ranked by severity and practical impact. The top three are dropped safety commands on saturation, unconditional monophonic note-off, and publication of take playback before its all-off request.

## Findings

### 1. EDGE / high — Safety commands can disappear when the audio command queues fill

**Locations:** `firmware/src/app/live_bridge.c:1051`, `firmware/src/app/midi_live.c:197`, `firmware/src/app/engine.c:229`, `engine.c:440`.

The bridge ignores command admission results. The clip queue has 15 usable slots; `clip_engine_transport_stop` changes the public playing flag and attempts a Stop that can be refused. Older queued Starts then turn playback back on. Separately, the 1,024-word engine queue silently drops note-offs like parameter changes. MIDI cleanup clears `mh[]` after calling a void note-off API, even if that note-off was dropped; the table then has no way to release the still-sounding note. MIDI overflow cleanup does not request `engine_all_off`.

**Failure:** a batch of 15 MIDI Starts followed by Stop reaches the main loop before an audio block; Stop is dropped and all Starts drain, leaving the engine playing. Or an already-sounding FM6/NOISE note, followed by 1,024 queued parameter/note commands and MIDI cleanup, loses its off and remains held after its tracking entry is erased.

**Evidence:** ran a throwaway bridge harness using the actual clip engine, with this key sequence:

```c
for (int i = 0; i < 15; i++) live_bridge_transport_start();
live_bridge_transport_stop(); run_isr_block(0, 256);
printf("playing=%d\n", live_bridge_transport_playing());
```

Observed `playing=1`. The engine-queue lost-note-off branch was established by inspection, not executed against DSP.

**Fix:** make Stop, all-off, mute-clear and note release non-droppable, using ordered reserve capacity or an explicit safety override consumed by the ISR. Return admission status for other commands; do not retire held-note ownership until release is admitted, and route MIDI overflow to a guaranteed panic path. Preserve Start/Stop ordering when introducing overrides.

### 2. BUG / high — Releasing an older external note silences the newer monophonic note

**Locations:** `firmware/src/app/live_bridge.c:1127`, `firmware/src/app/midi_live.c:74`; also `engine.c:555`.

`live_bridge_play_key` discards the pitch on BASS/LEAD release and always queues `engine_bass_off(part)`. The MIDI table remembers each pitch's role, but there is no check that the released pitch is the one sounding. This violates the architecture's explicit monophonic legato invariant. It also lets an external release interrupt a panel or clip note on the same part.

**Failure:** focus BASS; send Note On 60, Note On 64, Note Off 60. Note 64 should remain sounding; the bridge queues an unconditional part-wide off instead.

**Evidence:** ran the real bridge with engine calls instrumented: `play_key(BASS,60,1,100); play_key(BASS,64,1,100); play_key(BASS,60,0,0);` produced one unconditional `engine_bass_off`. No DSP run was needed to establish the command mismatch.

**Fix:** carry pitch through the note-off command and compare it with the ISR's current sounding pitch/owner before closing the 303 gate. Apply the same policy to clip, panel and external note producers, rather than trying to coordinate independent main-loop tables.

### 3. BUG / high — Take playback is published before the audio-source handoff's all-off

**Location:** `firmware/src/app/live_bridge.c:664–670` (primary line 664).

The masked section resets clip bookkeeping, publishes TAKE_PLAY, stages launch copies, and sets playing. It restores interrupts **before** calling `engine_all_off`. An audio IRQ in that gap renders the new take while old DSP voices still exist. The subsequent request is consumed at the next block start, killing notes the new take just started. Clearing `g_eng.note[].used` at reset does not release those old DSP voices.

**Failure:** a sustained CHORDS note is sounding; start a take whose offset-zero prelude launches a long CHORDS note; audio interrupts at the unlock. The first take block mixes old and new voices, then the next block's all-off releases the new long note, which is not retriggered until a later clip event.

**Evidence:** source interleaving review; not run against the actual DSP. The existing host hook is deliberately at this exact gap (`live_bridge_test_hook`, lines 665–668), so the race can be tested without changing the production scheduling model. Existing bridge tests stub `engine_all_off` to an empty function and cannot detect it.

**Fix:** publish `engine_all_off` inside the same critical section, before making TAKE_PLAY/playing visible, so the first new block consumes it before applying the take. Audit other reset/reload source changes against the same ordering contract.

### 4. BUG / high — Take playback applies every event in a block before any audio, ignoring sample offsets

**Locations:** `firmware/src/app/live_bridge.c:146`, `firmware/src/app/engine.c:998`.

`take_render` supplies the event's exact window offset, but `take_sink_ev` explicitly discards it. The engine calls the entire take pump before clip/audio rendering. Notes, parameters, FX, tempo and launches at different positions in that block all apply at its beginning; the last state wins for the whole block. For notes, an on and off within one block can produce no held interval at all. Tapping the block-end stamp into the event log hides the audio discrepancy.

**Failure:** load a take with FM6 on at sample 80 and off at 160 in a 256-frame block. Expected 80 frames of held sound; both commands are applied before frame zero. An FX engage/release in that window likewise has no audible held interval.

**Evidence:** ran the real bridge/take player with `engine_take_cmd` instrumented and called only `live_bridge_take_block(0,256)`: both engine commands had already applied before any call to clip/audio render. Probe output: `2 engine cmds applied before any clip/audio render, on/off at 80/160`.

**Fix:** merge take and clip events on one ordered sample timeline; render to each offset, apply events there, then render the tail. Split perform processing at those boundaries as well, and apply tempo/launch events after advancing clip transport to their actual offset.

### 5. BUG / medium — Natural take completion removes the final partial block's clip audio

**Location:** `firmware/src/app/live_bridge.c:521` (especially line 528).

The take pump advances `play_pos` through the entire upcoming block and, if it reaches the length, clears `g_eng.playing` before the engine renders that block. Therefore clip playback is stopped at the start of the final window rather than at the take's length. Clearing staged launches also discards launches just replayed in the final window. This is distinct from offset handling: even a launch at offset zero is lost.

**Failure:** a take contains a drum launch at sample 0 and has length 128; render 256 frames. The first 128 frames should play the clip, then stop. The bridge emits no clip events at all. Longer takes lose up to one block of their ending.

**Evidence:** ran a real bridge/clip/take probe with `ev={0, TAKE_LAUNCH(0,TAKE_LAUNCH_NOW,DRUMS)}`, length 128, then take pump plus clip render for 256. Observed `emitted 0 clip events`.

**Fix:** render the remaining take interval first, apply stop/all-off exactly at the length boundary, and render the rest as a release tail. Do not destroy staged state before events in that final interval have been consumed.

### 6. BUG / medium — Commands and their take-recording intents can land in different blocks

**Locations:** `firmware/src/app/live_bridge.c:1071–1072` (primary line 1072), `1088–1089`, `1096–1097`, `1199–1200`; `firmware/src/app/engine.c:79–80`.

Launch staging/command publication finishes under the mask, but the take intent is posted after unlocking. An IRQ between those operations applies the command without recording it; the next block records the intent. Mutes, tempo and FX use the same two-publication pattern. This contradicts the comment that the intent is stamped at the boundary where the corresponding command is consumed, and can move a quantized action to another beat/bar on replay.

**Failure:** a launch-now command is published just before a step boundary, IRQ runs at the unlock, and the intent is posted afterward. Live playback launches at that boundary; replay sees the launch only in the next block and waits for a later step.

**Evidence:** ran a host preemption probe by invoking a real ISR-side bridge block from `engine_crit_exit`, before `tp_post`. At transport sample 5500, the launch generated clip events at the upcoming step (5512); its take event was stamped 5756 on the next pump. Probe output: `preempted launch emitted 3 clip events; recorded launch offset=5756 (actual command block 5500; note 5512)`.

**Fix:** publish command and recording intent atomically, including admission decisions, or record the intent when the ISR consumes the actual accepted command. Prefer the latter to avoid maintaining two independent queues that must agree.

### 7. EDGE / medium — The take-intent queue silently drops meaningful releases and launches

**Location:** `firmware/src/app/live_bridge.c:71`.

The 32-slot queue has 31 usable entries and calls every full-queue event a “stray bump.” Those entries include launches, stops, FX releases, mutes and tempo. A full queue simply drops the recording event even when the separate product command was accepted. The perform ring explicitly preserves release/reset on overflow, making a missing recorded release particularly likely under saturation. There is no take-drop counter or incomplete-recording indication for this queue.

**Failure:** during LIVE recording, enqueue 31 take intents without an audio block, then release an effect. Perform's release is guaranteed via its override flag, but the take's OFF is discarded; playback keeps the effect active until the take ends. Large MIDI Program Change batches can fill this queue in a single 256-packet poll.

**Evidence:** inspection; not independently run. The unconditional full-queue `return` and separately posted FX OFF at `engine.c:84–85` establish the paths. The passing perform saturation test checks DSP release, not the recorded take.

**Fix:** tie recording to consumed/accepted commands, or guarantee capacity for state-ending intents and mark/stop a take on recording overflow. Never silently claim a complete recording after dropping performance events.

### 8. EDGE / medium — Core-1 failure recovery can spend most of an audio deadline waiting

**Locations:** `firmware/src/app/engine.c:823`, `firmware/src/app/plat_fm1.c:173`.

The audio ISR calls the HAL's core-1 wait, which spins for up to 8 ms before holding the failed core and rendering the job again on core 0. The DMA half budget is about 11.61 ms and comprises two 256-frame engine calls. An 8 ms wait leaves about 3.61 ms for all work across the half, including fallback DSP, main-core DSP, FX and nested TIMER5 activity. Bounded failure recovery still cannot promise uninterrupted audio. This is product misuse of the inherited timeout, not a request to rewrite the HAL.

**Failure:** core 1 accepts an 808/FM6 job and stops acknowledging it. The half incurs its normal work plus the 8 ms timeout and fallback; sufficiently busy content overruns the DMA deadline on the same recovery half. The no-required-second-core policy must include this first failure, not only steady state after core 1 is disabled.

**Evidence:** source budget analysis; no on-device timing or fault injection was performed. The wait implementation is `firmware/hal/fm1_cpu1.h:179`; `main_fm1.c:14` sets 512-frame halves.

**Fix:** use a recovery timeout derived from the remaining audio deadline, or avoid offloading in configurations where failure cannot be recovered within it. Measure the first failed-job half under maximum product load and account for partially completed work before fallback.

### 9. MAINTAIN / medium — Critical sections include bulk resets and prelude construction, without a measured bound

**Locations:** `firmware/src/app/live_bridge.c:867`, `1019`, `646`.

`live_bridge_song_reload` clears the whole clip engine byte by byte under a global interrupt mask, including four clip copies per role (`play`, `next`, two take FIFO entries). Start while armed also constructs the full take prelude under the mask: it enumerates all sound parameters, adds setup events, scans the built log for tempo, and computes the bar. Take-play copies up to ten clips under one mask. These are much larger than the architecture's “few µs” snapshot section and share the mask with the 10 kHz panel scanner, whose 100 µs cadence the main-file comments explicitly protect.

**Failure/fragility scenario:** adding one more clip copy or parameter silently increases a masked interval; a Start with REC armed or a song reload can miss panel scan cadence even if normal audio CPU measurements remain clean. No current host assertion measures duration; its critical-section tests check nesting/order, not deadline cost.

**Evidence:** inspection; no pi32v2 duration measurement, so this is a maintenance/budget risk rather than a claimed measured xrun.

**Fix:** prepare immutable reset/prelude/copy data with interrupts enabled, then use a short publication section where feasible; otherwise establish and continuously measure explicit worst-case mask-time limits on the device. Document unavoidable bulk sections and keep the scanner budget separate from audio average CPU.

## Coverage and shared-state audit

- **Bridge:** `g_song` is main-loop model state; song metadata/tempo also has ISR readers through the clip engine. `g_eng` transport positions, cur/que/play/next and take FIFO state are ISR-consumed; launches, clip edits, restart/continue, take staging and reload use engine_crit for copy publication. Main-loop queries of single fields are observational; multi-field positions/prelude reads are not an atomic snapshot. `g_active`, `g_eng_ready`, hash and `g_stop_q` are main-loop configuration/UI bookkeeping (active is also read in ISR). `g_take` is shared: ISR feed/tick/render, main arm/play/stop/load/close; state-before-data barriers and masked close protect normal log mutation. `g_take_live` is main written/ISR read, `g_take_block_end` ISR-only, arm/reload/bar/stage-index bookkeeping main-only; `g_take_end_req` is a volatile ISR-to-main notification. `tp[]` is main-to-ISR SPSC with publication barrier; the findings above concern cross-queue ordering and overflow. Immutable take-log scans occur with interrupts enabled; state/cursor is rechecked under the mask before staging. No ISR-side song clip reads or main-ring pushes were found in the reviewed take sink.
- **Engine:** DSP instances, mix targets/glides/buffers, probability RNG, core-job outputs and render-local bookkeeping are audio-owned; core 1 completes before mixing their outputs. `cq[]` is main-to-audio SPSC, indices volatile and producer publication ordered. `all_off_req` and motion release/enable are main-to-ISR flags. Sound mirror pointer and echo-base/apply bookkeeping are main-loop state; perform DSP/history/latches are ISR-owned, with a separate SPSC ring and non-droppable release/reset flags. The app still directly updates perform echo base fields from main; no measured failure for that ancillary snapshot was established. Guard enable/core-off switches are main written/audio read, load/guard state audio written with UI readers. Profiler, peak/hit/scope and active-voice reads are asynchronous telemetry rather than a consistent transaction. Handoff ordering and queue saturation are reported above.
- **MIDI glue:** parser, configuration, held table, held count, external indicator and sent-tempo state are main-loop only. Note ownership follows the role after focus changes. Packet validation and channel/cable filtering belong to the parser. Arrival stamps preserve tempo under stalled draining in the existing tests; clock lost leaves the last followed tempo, and Internal restores the song tempo. No unbounded wait was found here. Pitch-only ownership under Omni does not distinguish same pitches on different channels; that policy needs specification if overlapping multi-channel keyboards are intended.
- **Platform/main:** MIDI input producer is serialized TIMER5 USB/TRS polling; main consumes paired packet/stamp slots before publishing its read index. MIDI output uses the existing producer/consumer publication barrier. Overflow/CPU-peak read-and-clear notifications can race but are telemetry or followed by future overflow notification; no concrete hung-note finding is assigned solely to their flags. Sample clock writer is audio, readers TIMER5/main; bounded seqlock retries/fallback never wait. `master_q12` is aligned volatile main-to-audio; battery/LED composition state is main-only, with individual LED bytes consumed by the scanner. Audio metrics/profiling are ISR-written main-read. `usb_due`/timer accumulation are timer-owned; audio buffer is ISR-produced/DMA-consumed. `x0x_dbg.in_audio` gates nested USB/TRS polling. Boot/safe-mode state is initialized before normal operation. Platform flash entry points retain region/alignment bounds; save eligibility resides above this lane, so this report does not certify every storage/OTA/upload caller.

## Verification and limits

Ran `make unit-live unit-midilive` and `make unit-fx unit-sampleclk`; native and ASan/UBSan variants passed. These cover bridge mapping/staging consistency and preemption hooks, MIDI followed-tempo jitter/stalls, punch FX DSP/latch/release/saturation, and sample-clock publication interleavings. The five throwaway reproduction checks used `/tmp/audio_review_bridge.c` derived from the existing host bridge harness, linked to the unchanged production bridge/clip/take code; instrumented engine stubs establish commands and scheduling, not analog audio quality. The key sequences and results are quoted above; no throwaway file is committed.

Clean within those checks: clip-copy publication under the normal launch/edit masks, take staging's cursor recheck, wait-free sample-clock reads, sample arithmetic avoiding 64-bit division/double in reviewed product hot paths, steady-state single-core fallback, and perform's own non-droppable reset/release logic. No new out-of-bounds access was established in the scoped paths. Device CPU, global-mask duration and core-failure behavior remain unmeasured; no firmware build or hardware flash was performed.
