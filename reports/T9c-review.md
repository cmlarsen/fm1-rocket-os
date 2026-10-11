# T9c review (Opus): merge after these fixes

Coordinator decisions are in **bold**.

1. **Drop events stamped before `start`** (`take.c:151`, `:169-172`). In `take_feed`, if `(int32_t)(sample - start) < 0`, ignore the event. **Drop it, don't clamp it.** The prelude already sets the state at offset 0. T9i stamps events with the block-start sample, so this case will happen. Test: arm at 1000 and feed at 990; the event must be ignored, and the log stays sorted.
2. **Make the prelude ISR-safe** (`:132-138`). **Publish the state last; don't depend on the caller masking IRQs.** Build the log and the prelude while `state == IDLE` (the ISR ignores feeds in IDLE). Then write `start`, put a compiler barrier (`__asm__ volatile("" ::: "memory")`), and set `state = ARMED/REC`. Only the ISR side may do `n++` once the take is armed. Test: a feed while the prelude is being built must not interleave, and no prelude event may be lost.
3. **Same pattern for `take_play` and `take_stop`.**
   - `take_play`: reset `play_i` and `play_pos`, put a barrier, and only then set `state = PLAY`.
   - `take_stop` from the main loop only sets a `stop_at` request (barrier, then flag). Only the ISR's `take_tick` runs `take_end`, so the END mark is written exactly once. Add a guard flag anyway.
   - Test: stop requested from the main loop while feeds continue gives exactly one END.
4. Minor items:
   - Document in reports/T9c.md that T9i must call `take_tick` every block.
   - Remove the unused `play_start`.
   - Make `dense_window` test something `full_buffer` doesn't, for example events at every sample of a 1-frame window sequence.

Then run `make unit-take` (plain and sanitizer), recompile on deltron, commit, update reports/T9c.md, and send worker_done.
