# RC1 device results (2026-10-11, unattended)

Debug build `build/rc/rocket-rc1-debug-*.fwsc` (loader identical to X0X 1.0.5), installed over USB, then measured with `tools/rc_probe.py`. Core 1 was on (`cpu2: true`, no gave-up). Related: [rc1-probe](../engineering/rc1-probe.md) · [RAM plan](../engineering/architecture.md)

| Stress | What plays | Mean CPU | Max | Peak | Xruns | Guard |
|---|---|---|---|---|---|---|
| 1 | X0X busy pattern: 909 + 303A + 303B | 64.6% | 67% | 76% | 0 | never |
| 2 (first 1 s) | LIVE, every role, FM6 4-note chords every beat, NOISE 8ths | 76–77% | 77% | 91% | 0 | engaged at about 1 s |
| 2 (after guard) | the same, with FM6 shed to 2 voices | 66–69% | 69% | 86–91% | 0 | stays on (it releases below 65%) |

Stress 1 profile shares: 909 33%, 303A 22%, 303B 21%, FX 15%, MASTER 8.5%, SEQ 1%.

## Verdict
**RC1 fails its pass criteria at stress 2.** The criteria are a mean under 70%, a max under 85%, and the guard never engaged. No audio dropped out, and the guard did its job. Stress 2 is denser than real use: every role at full density plus a chord on every beat.

## Findings and actions
1. **CPU headroom is tight.** X0X's own baseline is already about 65% with core 1 on. A unit that falls back to a single core will be worse. This needs investigating; see task T5perf.
2. **The guard is sticky:** it turns on at an average of 85% and off below 65%, and stress 2 settles at 66–69%, so it stays on.
3. **The probe misses replies:** commands 60 and 61 occasionally get no reply in LIVE (once at 21.7 s, once at the final stop). `rc_probe.py` needs retries.
4. **Not measured yet:** single-core (core 1 off), punch FX (not wired), stress 4 (6 or 8 FM6 voices).

## Owner checklist (RC1 on hardware)
- [ ] Play LIVE by hand with CHORDS: does it sound right at 4 voices, and is a guard-shed to 2 voices noticeable?
- [ ] Decide whether a voice count below 4 is acceptable if T5perf can't win back headroom.

## Re-measured after T5perf candidate 2 (FM6 on core 1, guard releases at 75%)

| Stress | Core 1 | Mean | Max | Peak | Xruns | Guard |
|---|---|---|---|---|---|---|
| 2 (full LIVE, 4 FM6 voices) | on | 66.4% | 69% | 78% | 0 | never: **passes RC1** |
| 5 (stress 2 with core 1 forced off) | off | 67.4% | 80% | 92% | 0 | engaged once |
| 1 (X0X baseline) | on | 65.3% | 67% | 76% | 0 | never |

Candidate 1 had a bug: FM6 was silently skipped when core 1 was off. The coordinator caught it because FM6 showed 0% in the profile on the single-core run. A host test now proves the FM6 audio is the same with core 1 on or off.

**Verdict:** units with a healthy core 1 pass RC1. Single-core units stay dropout-free, with the guard shedding FM6 to 2 voices at full stress. That meets the "never require core 1" rule.

## After T10i (punch FX) + TRAM, debug build `df71895`

| Stress | Mean | Max | Peak | Xruns | Guard | Core 1 mean / max |
|---|---|---|---|---|---|---|
| 3 (LIVE + all six punch FX cycling) | 74.8% | 77% | 86% | 0 | never | 9.3% / 16% |
| 2 (LIVE, FX idle) | 70.3% | 73% | 82% | 0 | never | 9.3% / 16% |

Punch FX costs about 4% even when idle, because the history has to record continuously for repeat and freeze capture. Stress 2's mean is 0.3% over the 70% line. Follow-up T10perf: bypass the filter, echo and freeze work when idle, and make the history write cheaper. Core 1 has spare capacity (9% mean), so moving `perform`'s history write or the FX bus there is an option.
