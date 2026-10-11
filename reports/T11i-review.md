# T11i review (Opus): request changes

Notes, Program Change and panic are sound. Clock following reads the wrong tempo on hardware. The reviewer's simulation is `/private/tmp/claude-502/-Users-caleb-CalebApps-fm1-rocket-os/10296272-f272-4eb6-9e65-5ca02d389a5d/scratchpad/clk2.c`. Coordinator decisions are in **bold**. First merge `integration`: TRAM restored the pool headroom, and `make unit` is fixed there.

1. **Timestamp at the source.** `main_fm1.c:348` / `midi_live.c:150-153` stamp every packet drained in one poll with the same `eng_prof_frames`. **The producer (the TIMER5 ISR, `usb.c:640` / `midi_uart.c:22`) records the sample counter for each packet in a timestamp ring next to the packet ring,** read from the same audio sample counter, interpolated within the block if X0X exposes the in-block position. Otherwise use the block start plus a sub-block estimate from the 24 MHz timer. The main loop passes those stamps to `midi_in`.
   - Port the reviewer's `clk2.c` into a unit test that drives `midi_live` with redraw stalls of 8, 12 and 20 ms. At 120 and 174 bpm, the followed tempo must stay within ±1 bpm.
   - The host runner gets a `stall MS` command, so scenarios can model main-loop stalls.
2. **Start versus Continue** (`midi_live.c:78-79`). **0xFA Start resets the clip engine to bar 1, step 0** (and relaunches what was playing from its start); 0xFB Continue resumes. Song Position Pointer may stay unsupported; document that.
3. **Back to Internal** (`midi_live.c:166`): switching Auto→Internal, or losing the clock with Internal selected, restores the song's own `bpm_x10`.
4. **Ring overflow** (`usb.c:129`, 64 packets). **Grow the ring to 256 packets in the pool,** not `.bss` (which has 1.8 KB spare). On overflow, drop note-ons first, never note-offs. If an overflow happened, run `note_release_all` for external notes at the next poll.
5. Keep everything else. Run `make unit unit-core scenarios screens-lint parity check fw fw-debug` on the merged branch and `tools/check_loader.sh` on both packages. Report the RAM numbers.

Commit, update reports/T11i.md, and send worker_done.

## Re-review: request changes (the clock fix itself is verified: ±0.5 bpm under stalls)
The reviewer's tests are `/private/tmp/claude-502/-Users-caleb-CalebApps-fm1-rocket-os/10296272-f272-4eb6-9e65-5ca02d389a5d/scratchpad/clk3.c` and `restart_test.c`. Turn them into real tests.
1. **[High] The audio ISR can lock up** (`plat_fm1.c:112-118`, `main_fm1.c:40-41`). The audio ISR writes the time base before setting `in_audio`. TIMER5 can preempt it in that window, run `usb_poll`, and spin in `plat_sample_now` (`plat_fm1.c:125`) waiting for a write that can't finish. **Make the time-base publish lock-free and wait-free:** a sequence counter, where the reader retries a bounded number of times and otherwise uses the previous snapshot, never spinning on the writer. **Also set `in_audio` before the sync.** No path in an ISR may wait on another ISR. Add a host test that interleaves the reader at every point of the writer.
2. **[High] MIDI Start while playing leaves notes on** (`clip_engine.c:586-600`). Restart releases every sounding note (note-offs at the restart sample) before resetting the playhead, and the step-0 notes then sound. Same for Continue after Stop if any notes remain.
3. **[Medium] Start/Continue aren't ring-ordered** (`clip_engine.c:305-307`). Restart and continue become ring commands, so `playing` changes only in the ISR as part of applying the command.
4. **[Low] A song loaded after the clock is lost keeps the followed tempo** (`clip_engine.c:564`). Loading a song clears `bpm_ext`, unless the clock is currently present.
5. `make fuzz-ui` now exists on `integration` (T2c). Merge `integration` again and run it.

Then run the full ladder plus `make fuzz-ui`, and `tools/check_loader.sh` on both packages. Commit, update reports/T11i.md, and send worker_done.

## Final check: the branch is approved. Now merge integration (T7b) with these fixes.
A trial merge conflicts in `Makefile`, `clip_engine.h`, `live_bridge.c`, `ui.c`, `build_host.sh` and `web/emu/build.sh`. Merge `integration` into your branch and resolve:
1. **[High]** Wrap `live_bridge_launch_quant` (Program Change) in `engine_crit_enter`/`engine_crit_exit`, like T7b's other launches. `clip_engine_launch_quant` now copies the clip into `next[]` from the main loop.
2. **[Medium]** MIDI Start/Continue relaunch `s_cur` without refreshing `play[]`, so a clip edited while stopped plays its old copy. Start and Continue go through the same main-loop staging copy as a launch, under `engine_crit`, before the ring command.
3. Merge both enums: T7b's `CLIP_CMD_LAUNCH_AT` alongside `SET_BPM_SOFT`, `RESTART` and `CONTINUE`. Merge the `unit-core` list (`unit-crit`, `unit-midilive`, `unit-sampleclk`) and `ui.c`'s `R_CLKIN` and `R_ED_*` entries.
4. **[Low]** Stop also becomes a ring command, so Start then Stop in the same poll stops.
5. **[Low]** Strengthen `sample_clock_test.c`: actually run the writer between the reader's own loads (step through every interleaving), and make the random check able to fail. Share `last` safely, or make it per-context.
Then run the full ladder on the merged branch (`make unit unit-core scenarios fuzz-ui screens-lint parity check fw fw-debug`, and `tools/check_loader.sh` on both packages). Commit, update reports/T11i.md, and send worker_done.
