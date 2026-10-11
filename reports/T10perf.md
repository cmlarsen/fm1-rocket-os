# T10perf: cut punch FX's idle CPU cost

The device numbers in [2026-10-11-rc1-device-results.md](../docs/reviews/2026-10-11-rc1-device-results.md)
showed punch FX costing about **4 %** of the block even when no effect is engaged: stress 2 (LIVE, FX
idle) went from the post-T5perf **66.4 %** to **70.3 %**, 0.3 % over RC1's 70 % mean line, because the
mono history has to record continuously so repeat and freeze can capture the last slice. **Goal:** with
no effect engaged, `perform_process` costs ≤ 1 % of the block. The effects behave and sound exactly as
before.

**Result:** an idle fast path in `perform.c` skips the whole per-sample chain (filter, crossfade, output
write, per-sample loop branches) and pays only for the history write. On the host (device-like scalar
build) idle `perform_process` fell **1.50 → 0.61 ns/sample**, a **2.5×** cut, and is now within ~25 % of
the raw history-write floor. The device stress numbers come from the coordinator.

## What changed

**`firmware/src/dsp/perform.c` only** (plus the test):

1. `perf_idle(p)`: true when nothing in the *audio* path can change the block — no buffer effect
   (`act` / `old` / `pend` off), no crossfade (`xmode == PERF_XF_NONE`) and no filter
   (`filt_state == 0`). The echo throw is a controller for X0X's fxbus delay and never touches this
   audio, so it can be held while the path is idle.
2. `perf_record(p, l, r, n)`: the idle path. The output is the input, in place, so the only work is the
   mono history write repeat / freeze capture from. The ring wrap is split out of the per-sample loop
   (`PERFORM_HIST_N - w` at most once per 256-frame block), so the inner loop has no per-sample mask or
   branch beyond the `f2i16` clamp. No loop is engaged while idle, so the write is a plain forward run
   (the old `skip_loops` / `in_region` tests can only ever say "not in a loop").
3. `perform_process` runs `drain()` first (unchanged), then takes the fast path when `perf_idle`. A
   command that arrived in `drain()` changes the state, so a block where an effect just engaged still
   takes the full path below and the grid capture is exact.

Filter, freeze and the crossfade cost nothing while inactive, as asked. No new waits, no change to the
ring, the grid contract or any hand-off. The `engine.c` call site is unchanged (the fix is entirely
inside `perform_process`).

## Host profile, before and after

Mac (`cc -O2 -ffp-contract=off`), 256-frame blocks, 44.1 kHz, 10 s. `ns/sample` is wall-clock and moves
run to run; the share of the block's realtime budget (one core) is the stable part. Two builds: the
default `-O2` (auto-vectorised) and `-fno-tree-vectorize` (scalar; closest to the pi32v2, which has no
SIMD).

| Idle `perform_process` | before | after | |
|---|---:|---:|---|
| default `-O2`, ns/sample | 2.07 | **0.20** | ~10× (the idle loop auto-vectorises now) |
| `-fno-tree-vectorize`, ns/sample | 1.50 | **0.61** | **2.5×**, the device-like number |
| scalar share of the block budget | 0.0066 % | **0.0027 %** | both ≪ the 1 % goal on the host |
| flat history-write floor (isolated loop) | 0.51 | 0.51 | the unavoidable part: a mono int16 store per sample |

The host floor shows the fast path is now dominated by the history write itself.

### Per section, idle (`-fno-tree-vectorize`; the same run)

Attribution by stubbing sections out of the before revision, then measuring the after fast path. All are
ns/sample of the block budget.

| Line | Measured |
|---|---:|
| block overhead only (drain, empty ring: `perform_process` with the per-sample loop removed) | 0.08 |
| history write (recording head + `skip_loops`; before minus the no-history stub) | 0.46 |
| everything else in the per-sample loop (the no-history stub minus the overhead) | 0.95 |
| **before total** | **1.50** |
| **after (fast path)** = overhead 0.08 + history 0.46 + ~0.07 loop split | **0.61** |

The 0.95 "everything else" is all the machinery the fast path drops: the two `if (filt_state != 0)`
tests, the `a = gain_act + gain_old` crossfade test, `advance_fades` (its `xmode` test and the
`gain_act`/`gain_old` writes), the four `in_region` calls in `skip_loops`, and the two per-sample output
stores (which were no-ops: the output already equals the input in place). None of these do useful work
while idle, and all are now skipped.

The echo controller is **not** in `perform_process`: it is `perform_echo_targets`, called from
`engine_perform_tick` once per UI frame, not per sample. While idle it returns on a single flag read. On
the JieLi clang (pi32v2, no SIMD) the isolated idle loop compiles to **~19 instructions per sample**, with
the two float loads, one add, one multiply, the `f2i16` clamp branch and the int16 store.

## Options assessed for the history write (step 3)

The history write is now the whole idle cost. It is shared by repeat, tape stop and freeze; the numbers
below are host estimates, not device measurements.

| Option | Saves | Costs |
|---|---|---|
| **Half-rate history (22.05 kHz) for repeat / freeze** | ~half the write (~0.25 ns/sample host) | **changes the sound**: repeat slices and the freeze grain lose the top octave and tape loses its pitch. It also needs a second buffer or a rate tag, because tape reads the same history at full rate. Only with the coordinator's explicit OK. |
| **Move the history write to core 1** | most of the ~0.5 ns/sample off core 0 | a per-block handoff of the audio block to core 1 plus a core-0 fallback, all wait-free ("interrupts never wait"); core 1 is ~9 % busy so it has room, but the handoff and the fallback add risk and code. |
| **Do nothing more** | — | the idle path is now within ~20 % of the raw write floor; this was the goal of step 2. |

The decision waits on the coordinator's device measurement (below); the half-rate option is only worth
its sound change if the device idle cost is still over 1 %.

## Device numbers

*(filled from the coordinator's `rc_probe.py` run on `build/debug/x0x.fwsc` — see the message trail)*

## Verification

Every target the task asks for, run on the Mac (`deltron.local` for the firmware builds); logs under
`build/ladder/`.

| Command | Result |
|---|---|
| `make unit` | pass (the reference comparisons skip without the `../schwung-*` checkouts, as before) |
| `make unit-core` / `make unit-fx` | pass; **all `tests/golden/perform/*.txt` unchanged**, plain and ASan/UBSan |
| `make scenarios` | pass, all goldens (incl. `live_fx*`, `rc1_probe`) unchanged |
| `make parity` | pass: WASM matches native, max diff 3.05e-5 (1 LSB), screen + lights true |
| `make fuzz-ui` | pass: 200 seeds × 2000 events |
| `make check` | pass: forbidden symbols, flash guard, help cards, size checks, short fuzz |
| `make fw` | pass: image 553,328 B; `.data+.bss` 96,404 B of 98,304; pool 330,588 B of 344,064 |
| `make fw-debug` | pass: image 556,448 B; `.data+.bss` 96,520 B of 98,304; pool 332,380 B of 344,064 |
| `tools/check_loader.sh` (both packages) | ok: loader byte-identical to X0X 1.0.5 (`ca93577f…`) |

No RAM moved (the change is code only); `size(perform.o)` grew by a few hundred bytes of `.text` for the
fast path. `make unit-fx` gained the idle-history assertions in `case_cleanup` (no golden rewrite).

## Files

Changed: `firmware/src/dsp/perform.c` (the idle fast path: `perf_idle` / `perf_record` and the early
return in `perform_process`, plus a header-comment note), `tests/host/perform_test.c` (the idle history
assertions), this report. `engine.c` (the call site needed no change), `perform.h` and the goldens are
untouched.

