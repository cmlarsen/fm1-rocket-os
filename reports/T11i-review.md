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
