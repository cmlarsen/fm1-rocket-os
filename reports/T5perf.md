# T5perf: win back CPU headroom

RC1 measured full LIVE stress at **76–77 % mean** on the device with 4 FM6 voices; the overload
guard engaged at ~1 s and then stayed on (it released below 65 %, and the guarded load settled at
66–69 %). RC1's pass criteria are mean < 70 %, max < 85 %, 0 xruns, guard never engaged. This task
wins the headroom back and fixes the two tooling gaps RC1 found.

**Result (device, coordinator-measured):** see [Device numbers](#device-numbers). The host can't run
two cores, so the per-engine cost is the host half and the split is the device half.

## What changed

1. **FM6 renders on the second core** (`engine.c`). `render_sub` already hands core 1 a job
   (`core2_job`: the 808). FM6 now joins it when it sounds, into the same `part_i32` int32 block the
   core-0 mix reads (arg bit 9). Core 0 keeps the mix (`t5i_part_mix`), so the render order is one
   core's, bit for bit. It falls back cleanly: if core 1 is off or has given up, or the new
   debug switch forces it, `core2_job` runs on core 0 exactly as before. In LIVE core 1 is otherwise
   idle (the 808 does not play there — the clip engine drives the 909 role), so the FM6 render is
   nearly free on it.
2. **Guard hysteresis** (`engine.c`): the release threshold rose from 65 % to **75 %** (the engage
   stays at 92 % last / 85 % average), so the guard no longer stays on at 66–69 %.
3. **Probe retries** (`tools/rc_probe.py`): `Probe.ask` retries a timed-out command up to three
   times. Every command is idempotent (a duplicate `ED_STRESS n` restarts the same level; `ED_STATS`
   and `ED_STRESS 0` are reads / no-ops), so a retry cannot double-start a run. RC1 saw commands
   60/61 miss a reply in LIVE.
4. **Single-core stress levels** (`rc_debug.c`, `rc_debug.h`, `rc_probe.py`): `ED_STRESS 5/6` are
   levels 2/1 with the core-1 job kept on core 0, for RC1 to report the numbers a unit that cannot
   run core 1 would see. They do not stop core 1 (no ISR race); `ED_STATS`' `cpu2` reads 0 for them.
5. **Core-1 load in `ED_STATS`** (`engine.c`, `rc_debug.{c,h}`, `rc_probe.py`): a new field reports
   core 1's mean busy share of the block, self-timed on core 1 with the shared 24 MHz timer. Core 0's
   `audio_cpu_pct` only includes core-1 work it waited for, so FM6 on core 1 can be invisible in the
   load while it is still real work; this makes the split visible. `rc_probe.py` prints it and puts
   `core1_pct` in the JSON.

Assessed and left alone (already satisfied):

- **Skip work that makes no sound** is already done: idle 909/808/303s are not cleared, mixed or
  metered; `t5i_render` skips FM6/NOISE while idle; `fm6_core_render` skips an operator below
  `FM6_THRESH` on both block ends; `fm6_note_done` drops a finished note.
- **The FM6 inner loop** is already table-driven and integer, with per-block envelope/LFO/pitch
  (`fm6_note_compute`); the per-sample work is the six operator loops (`fm6_sin` lookup + interp).
  A cheaper sine would change the parity golden, so it was not worth the audio risk for this pass.

## Before / after host profile (stress 2, one core)

`X0X_PROFILE=1 build/host/x0x_host <stress2>` after `stress 2`, 6 s. The host runs one core (the
core-1 job is called synchronously), so the *work* is the same before and after — the table shows
that, and the win is where the FM6 work runs. ns/sample is wall-clock and moves run to run; the
shares and instructions/sample are the stable part.

| Stage | before ns (share) | after ns (share) | |
|---|---:|---:|---|
| 909 | 22.5 (15.5 %) | 23.5 (15.6 %) | |
| 808 | 0.1 (0.1 %) | 0.1 (0.1 %) | idle in LIVE |
| 303 A | 32.7 (22.5 %) | 33.9 (22.4 %) | |
| 303 B | 33.3 (22.9 %) | 34.4 (22.7 %) | |
| FX | 11.0 (7.6 %) | 11.2 (7.4 %) | |
| MASTER | 9.4 (6.4 %) | 9.7 (6.4 %) | |
| SEQ | 0.1 (0.1 %) | 0.1 (0.0 %) | |
| **FM6** | **32.8 (22.5 %)** | **34.4 (22.7 %)** | render moved to core 1 |
| NOISE | 3.7 (2.5 %) | 4.0 (2.6 %) | |
| **instructions / sample** | **2610** | **2612** | unchanged (same work) |

## Each change and its gain

| Change | Host | Device |
|---|---|---|
| 1. FM6 → core 1 | no host change (one core); the host's FM6 render (22.5 %) moves off core 0 | stress 2 went 76–77 % → 66.4 % mean with the guard never engaged; core 1 carries the FM6 render (`core1_pct` 5 %), which core 0's load does not show |
| 2. Guard release 65 → 75 % | `guard.x0x` updated: 70 % sustained now releases; 75–92 % holds | the guard no longer stays on at 66–69 % (stress 2 guard never; stress 5 single-core still protects) |
| 3. Probe retries | `rc_probe.py` (no host test; needs the device) | fewer missing replies in LIVE |
| 4. Single-core levels 5/6 | `rc1_probe.x0x` asserts `core2forced` and restores it | stress 5: 67.4 % mean, guard engaged once — a single-core unit is protected |
| 5. `core1_pct` in ED_STATS | — | the split is visible: core 0's load hides core-1 work it does not wait for (FM6 5 % at stress 2) |

## Device numbers

`build/debug/x0x.fwsc` (X0X_DEBUG=1), measured by the coordinator with `tools/rc_probe.py` over 60 s.

**Before (RC1, for reference):** stress 2 was 76–77 % mean, 77 max, 91 peak, 0 xruns, guard engaged at
~1 s and then stayed on (guarded steady 66–69 %).

**After (candidate 2, the accepted build):**

| Stress | Mean | Max | Peak | Xruns | Guard | cpu2 | core1 | Notes |
|---|---:|---:|---:|---:|---|---|---:|---|
| 2 (LIVE, 4 FM6) | **66.4** | **69** | 78 | 0 | **never** | on | 5 | **PASSES RC1** (mean < 70, max < 85, 0 xruns, guard never). prof 909 31.7 / 303A 20.7 / 303B 20.2 / FX 14.4 / MASTER 8.2 / SEQ 0.1 / FM6 0.7 / NOISE 4.1 |
| 1 (X0X baseline) | 65.3 | 67 | 76 | 0 | never | on | 23 | core 1's 23 % is the 808 (the sequencer path); FM6/NOISE 0 |
| 5 (core 1 forced off) | 67.4 | 80 | 92 | 0 | engaged once | off | 0 | FM6 now renders on core 0 (prof FM6 5.3 %); the guard protects a single-core unit, as it should |

`core1` is core 1's own busy share of the block (the end-of-run value here; `rc_probe.py` now reports
`core1_pct_mean`/`_max`). Stress 2's `core1` of 5 % is the FM6 render — core 0's `cpu_pct` does not
wait for it, so `FM6 0.7 %` in the prof buckets is the mix only. On the device FM6 is a smaller share
than the host profile suggests (the host over-weights its integer FM6), so the win at stress 2 is the
guard staying off with headroom (66.4 % vs the 76–77 % that tripped it) rather than a large `core1`.

**Candidate 1 → 2 fix.** Candidate 1's stress 5 looked identical to stress 2 (`FM6 0.0 %` in prof,
same load) — `render_sub` gated the FM6 half of the core-1 job on `!engine_core2_forced_off()`, so with
core 1 forced off the FM6 render was **skipped entirely** (silent FM6). Candidate 2 makes `fm6_on2`
depend only on whether FM6 sounds, so the forced path renders it on core 0 (stress 5 now shows
`FM6 5.3 %`), and adds the `core1_pct` field and the `t5perf_core2` host regression test.

## Verification

- `make unit-core` — all host unit suites pass (incl. the FM6/NOISE parity and adapters).
- `make scenarios` — every scenario passes; all goldens unchanged (audio parity: the FM6 render
  produces the same samples on core 0 or core 1). `guard.x0x` and `rc1_probe.x0x` updated for the new
  release threshold and the new levels.
- `make parity` — WASM matches native (audio, screen, lights).
- `make check` — firmware build, soft-double / size / symbol checks pass.
- `make fw-debug` — the debug package builds (`.data+.bss` 96,288 B of 98,304; pool 279,456 B of
  344,064).
- `tools/check_loader.sh build/x0x.fwsc` and `build/debug/x0x.fwsc` — both loaders are byte-identical
  to X0X 1.0.5.

## Files

Changed: `firmware/src/app/engine.c` (the FM6 half of the core-1 job, the forced-single-core switch,
core 1's self-timed busy counter, the guard release), `firmware/src/app/engine.h` (the switch and the
counter), `firmware/src/app/rc_debug.c` / `rc_debug.h` (levels 5/6, the `core1_pct` field, `cpu2` for
a forced run, the LIVE-exit settle), `tools/rc_probe.py` (retries, levels 5/6, `core1_pct`,
`single_core` in the JSON), `tests/scenarios/guard.x0x` (the 75 % release),
`tests/scenarios/rc1_probe.x0x` (levels 5/6, `statswords 22`), `host/x0x_host.c` (profile LIVE too,
the `core2forced` expect, the `core2off` command), `docs/engineering/rc1-probe.md` (levels, retries,
the `core1_pct` field), this report.
New: `tests/scenarios/t5perf_core2.x0x` and `tests/golden/t5perf_core2/` (the forced-off FM6
regression test).
