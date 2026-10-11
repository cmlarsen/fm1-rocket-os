# T4c review (Opus): merge after these fixes

Coordinator decisions are in **bold**.

1. **Clip kind comes from the role, not from the payload bytes** (`clip_engine.c:88-99`, `:207`). **A clip of role ROLE_DRUMS uses `d.drum`. Every other role, FX included, uses `d.notes[0..nnotes)`.** Remove the guess. Honour `used` (**an unused clip cannot be launched; launching it is ignored and emits nothing**) and `nnotes` (clamp it to `CLIP_MAX_NOTES`). Add goldens: a drum clip with lane-0 hits on steps 0 and 24, and a melodic clip shortened below its notes.
2. **A tempo change sent through the ring is reverted on the next block** (`:470-471` vs `:395`). Make one source of truth: the ring's tempo command sets the song's tempo, or the engine stops re-reading `song.bpm_x10` after the change. Add a ring-tempo golden that renders several blocks.
3. **A big tempo jump fires several steps on one sample** (`:42-48`). Keep the current step's phase when the tempo changes. Each step fires once, at its correct sample. Golden: 60 bpm, then 300 bpm mid-step.
4. **A role stop must use the clip's quantize** (`:326`). Q_BAR stops at the next bar, Q_BEAT at the next beat, Q_END at the clip end. Fix the `stop_role` golden.
5. **Cancelling a queued stop must emit `CLIPEV_CLIP_CANCELLED`** (`:402-404`).
6. **Ring publish order** (`:294-298`): add a compiler barrier (`__asm__ volatile("" ::: "memory")`) before the head store and after the head load on the consumer side, and make sure the indexes are `volatile`.
7. Minor fixes:
   - HOME+tap on the already-queued clip launches it now (does not cancel).
   - Make `clip_engine_set_bpm` internal, or document it as ISR-context only. The UI uses the ring.
   - Make the ring push return `int` (0 = ok, -1 = full).
   - `queued_clip` reads a consistent snapshot.
   - Rename `clip_engine_stop_role` to `clip_engine_stop` as the spec says.
8. Add tests for things that were untested: sample counter wrap (start the counter near `0xFFFFFFFF - 48000`), 1-frame render windows over a full bar, and tempo through the ring.

Then run `make unit-clip` and the sanitizer build, recompile for the device on deltron, commit, update reports/T4c.md, and send worker_done.
