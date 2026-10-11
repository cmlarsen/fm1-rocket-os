# T10perf: cut punch FX's idle CPU cost

The device numbers in `docs/reviews/2026-10-11-rc1-device-results.md` show punch FX costing about 4% even when no effect is engaged: stress 2 mean went from 66.4% to 70.3%. **Goal:** with no effect engaged, `perform_process` costs ≤ 1% of the block on the host profile. The effects behave and sound exactly as before.

Read first:
- `CLAUDE.md`;
- the invariants in `docs/engineering/architecture.md` (interrupts never wait; the RAM plan);
- `firmware/src/dsp/perform.c/.h`;
- `reports/T10c.md` and `T10i.md`;
- `firmware/src/app/engine.c` (where `perform_process` runs, and the core-1 job: core 1 is about 9% busy);
- `docs/engineering/rc1-probe.md`.

**Ownership:** `firmware/src/dsp/perform.c/.h`, the `perform` call site in `engine.c`, `tests/host/perform_test.c` and its goldens.

## Do
1. Profile `perform_process` per section with nothing engaged: the history write, filter, echo controller, crossfades and ring drain.
2. **Idle path:** skip everything except what's needed to capture the moment an effect engages. The history write must continue, so repeat and freeze can grab the last slice. Make it as cheap as possible: block copy, mono downmix once per block, no per-sample branches. Filter, freeze and the crossfade state cost nothing while inactive. Any cutoff or Q smoothing settles instantly when idle.
3. If the history write is still expensive, consider recording it at half rate (22.05 kHz) for repeat and freeze only. **Ask the coordinator first**, since it changes the sound. Moving it to core 1 is also an option, with core-0 fallback and no new waits (see "interrupts never wait"). Report what each option costs.
4. `make unit-fx` goldens must stay identical, or be re-seeded with a written per-file reason, with no audible behaviour change.
5. Build `make fw-debug`. Ask the coordinator to measure stress 2 and 3 on the device through your `ask` command.

## Done
- Run `make unit unit-core unit-fx scenarios parity fuzz-ui check fw fw-debug` and `tools/check_loader.sh` on both packages.
- `reports/T10perf.md` has a before-and-after profile.
- `worker_done`.
