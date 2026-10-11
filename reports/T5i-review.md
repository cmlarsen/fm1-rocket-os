# T5i review (Opus): merge after these fixes

The build is safe to install, because FM6 and NOISE can't be reached on the device yet. Coordinator decisions are in **bold**.

1. **`ENG_FM6_VOICES` defaults to 4.** The estimate for 8 voices plus NOISE is about 103% of the block at a 2:1 device cycle ratio, 6 voices about 94%, and 4 voices about 85%. RC1 measures it and may raise the default.
2. **The overload guard must shed FM6** (`engine.c:83`). While `guard_on` is set, cap the active FM6 voices at 2 by releasing the oldest, and skip NOISE density. Add PROF buckets for FM6 and NOISE (`engine.h:47`) so the PERF counters show their cost. Test it with the runner's `load N` command.
3. **Clamp `note` to 0..127 in `engine_note_on/off`** (`engine.c:326`), so a note above 127 can never leave a voice stuck. Test it.
4. **Add an engine-level all-notes-off.** `engine_all_off()` releases the 303s, the 909/808 voices where that applies, FM6 (`eng_fm6_all_off`) and NOISE (`eng_noise_all_off`). Call it on transport stop and panic. T3's `live_bridge_exit` will call it after the merge; note that in the report. Test: stop with FM6 chords held, and the output decays to silence.
5. **Migrate the sound format.** Bump `PROJ_SND_REV` (`x0x.h:20`). When a project with an older revision loads, set T_MIX 4/5 and the T_RSV FM6/NOISE parameters to sane defaults: the level of the other parts, the pan centred, macros at 0 offset, and NOISE at its init values. Test it with an old-revision project fixture.
6. **RAM:** move the NOISE voices and the FM6 tail and LFO state into the pool, because `.bss` has only about 2 KB spare. Report the before and after numbers.
7. **Don't merge integration yet.** T3 merges first; then I'll ask you to merge integration and resolve the conflicts. When you do, keep both sides of `engine_init`, and CHORDS/FX use `engine_note_on`.

Then run `make unit-core scenarios check parity fw` and `tools/check_loader.sh build/x0x.fwsc`, commit, update reports/T5i.md, and send worker_done.

## Item 7, now: merge integration (T3 has merged)
- Merge `integration` into your branch.
  - In `engine_init`, keep both sides: `live_bridge_init()` and the `ENG_NPARTS` loop.
  - T3 replaced the ISR path. Clip events are applied directly to the voices at their sample offsets (`live_apply` in `engine.c`), never through `cq_put`. Add FM6 and NOISE to that direct path the same way (direct `eng_fm6`/`eng_noise` calls in the ISR), not through `engine_note_on`, because `engine_note_on` queues.
- In `live_bridge.c`, map **CHORDS → FM6** (part 4) and **FX → NOISE** (part 5). Note events with velocity become note on/off on those parts. T3's `all_off_now` must also call your `engine_all_off` path, so leaving or entering LIVE silences FM6 and NOISE.
- Add a LIVE scenario that launches a CHORDS clip and an FX clip and asserts the FM6/NOISE events and non-silent audio. Re-seed only what changes.
- Run `make unit-core scenarios screens-lint parity check fw` and `tools/check_loader.sh build/x0x.fwsc`. Commit, update reports/T5i.md, and send worker_done.
