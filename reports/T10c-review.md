# T10c review (Opus): merge after these fixes

Coordinator decisions are in **bold**. The RAM plan is in `docs/engineering/architecture.md` on integration ("RAM plan (v1)"); merge integration into your branch to get it.

1. **Memory.** `perform_t` must fit the RAM plan.
   - **Remove the echo delay line from `perform.c`.** Echo throw becomes a small controller: `perform_echo_throw(p, on)` produces target values for X0X's fxbus delay (send level, feedback, tempo-synced 1/8 time, held while on, and release restoring the previous values so the tail decays naturally) through a callback or an output struct. T10i applies them with `engine_set` on `T_FX` / `T_MIX`. Read `firmware/src/dsp/fxbus.*` and `engine.h` to choose the parameters, and test the controller's outputs.
   - **Keep the 32 KiB mono int16 history.**
   - Report the new `sizeof(perform_t)`; the target is about 33 KB or less.
2. **Freeze seam click** (`perform.c:117-124`). After the first pass, wrap to `loop_pos = X` so the crossfade region is not played twice. Test: the largest sample-to-sample step at the seam on a 0.5-amplitude 440 Hz sine stays within 2× the step of the input itself.
3. **Filter engage and re-engage clicks** (`engage_filter:195-197`). Don't zero the state. Start the sweep from the filter's current cutoff and amount. Re-engaging mid-dive continues from the current amount. Ramp Q in too. Same click test for engage, re-engage and release.
4. **Tape stop after a loop reads stale audio** (lines 415-417). Keep writing live input into the history while a loop effect plays (write into a region the loop isn't reading, or keep a separate write head), so tape and repeat after a loop start from current audio aligned to their own grid. If that's impossible within 32 KiB, fall back to the live signal with a crossfade, and document it. Test both.
5. **A dropped RELEASE leaves an effect stuck** (`ring_put:266`).
   - Never drop release or reset. Coalesce tempo commands to the latest value.
   - If the ring is full, release and reset override: they set a flag the ISR checks.
   - Test: 20 tempo commands and then a release, and the effect is off.
6. `perform_reset` must not zero 16 KiB in the audio callback. With the echo line gone, check that reset is constant-time, or spread clearing over blocks.
7. Removing the old buffer effect (`release_fx` lines 208-211) must crossfade during a replacement; test it.
8. **Stronger goldens and tests.** Use a click-free test input (no 0.6 clicks) for the step and seam tests, so the thresholds mean something. Assert the largest step on every engage, release and replace boundary.
9. Add `-ffp-contract=off` to `SAN_CFLAGS` for this target.

Then run `make unit-fx` (plain and sanitizer) and the device compile, commit, update reports/T10c.md with the sizes, and send worker_done.
