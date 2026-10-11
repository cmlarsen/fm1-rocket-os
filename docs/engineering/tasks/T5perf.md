# T5perf: win back CPU headroom

The [RC1 results](../../reviews/2026-10-11-rc1-device-results.md) show full LIVE stress at about 77% CPU on the device with 4 FM6 voices. The overload guard trips and holds FM6 at 2 voices. The X0X baseline is already about 65% with core 1 on.

**Goal:** at stress 2 with 4 FM6 voices, mean CPU under 70%, max under 85%, and the guard never engaged, measured by the coordinator on the device. The firmware must still work correctly when core 1 is unavailable.

You can't run the device. You measure on the host with X0X's profiler and the runner's PERF/`stats` commands, and the coordinator installs your debug build and runs `tools/rc_probe.py`. Ask the coordinator through your `ask` command when you have a candidate worth measuring. They'll reply with the device numbers.

Read first:
- `CLAUDE.md`;
- `docs/reviews/2026-10-11-rc1-device-results.md` and `docs/engineering/rc1-probe.md`;
- `firmware/src/app/engine.c` (the render path, PROF buckets, the guard, the core-1 split with `plat_cpu2_run`);
- `firmware/src/dsp/eng_fm6.c` and `fm6_core.c`;
- `firmware/src/dsp/fxbus.c` and `master.c`;
- `docs/plans/*perf*` (X0X's own performance notes);
- `firmware/src/app/rc_debug.c`.

**Ownership:** `firmware/src/app/engine.c` (render, scheduling, guard), `firmware/src/dsp/*` performance changes that keep audio identical or document the difference, `firmware/src/app/rc_debug.c`, `tools/rc_probe.py`, and tests.

## Candidates, in order of expected value
1. **Use core 1 for FM6 when it's available.** X0X already renders 909/808 on core 1 when it's healthy. Render FM6 voices there too, or split the work so the cores balance. Fall back cleanly to core 0 when core 1 is off or has given up. Never *require* core 1.
2. **Skip work that makes no sound:** idle voices, silent FM6 operators (envelope at zero), FX sends at zero, NOISE with no voices. Check X0X does this for its own parts.
3. **FM6 inner loop:** the cost per voice per block. Look for per-sample work that can move to per-block (LFO, pitch), and table lookups instead of computation. Felucca's code is integer; keep it bit-identical to the T5c parity test if you can. If you can't, update the parity golden and explain why.
4. **Guard hysteresis:** release at an average of 75%, not 65%, so the guard doesn't stay on at 66–69%. Keep it engaging before dropouts.
5. **Probe reliability:** `rc_probe.py` retries a missing reply up to 3 times, using a sequence number or idempotent commands, and `ED_STRESS 0` is idempotent.
6. **Measure single-core:** add a debug-only stress option that forces core 1 off for the run, so RC1 can report the single-core numbers.

## Rules
Audio output may change only where it's documented: scenario goldens and engine parity must still pass, or be re-seeded with a clear reason per file. No heap, no `double`. Run `make unit-core scenarios parity check fw fw-debug` and `tools/check_loader.sh` on both packages.

## Done
- `reports/T5perf.md` has a before/after host profile table, each change and its gain, and the device numbers the coordinator gave you.
- Commit, then send `worker_done`.
