# RC1p: measuring on the device without hands

RC1's job is to measure CPU headroom on the device ([build plan](../build-plan.md), RC1). Nobody is at the FM-1, so the coordinator measures it over USB. This task builds the tools for that. **You never install or flash anything.** The coordinator runs the installer.

Read first:
- `CLAUDE.md`;
- `tools/fm1_debug.py` and the debug commands in `firmware/src/app/editor.c` (PEEK/POKE/CLOCK, `X0X_DEBUG=1`);
- `firmware/src/app/plat_fm1.c` (the `audio_cpu_pct`, `audio_peak_pct` and `audio_xruns` counters, and core-1 status);
- `firmware/src/app/engine.h` (the PROF buckets, including `PR_FM6`/`PR_NOISE`);
- `firmware/src/app/live_bridge.c` and `ui_live.c` (entering LIVE and launching clips);
- `reports/T5i.md` (`ENG_FM6_VOICES` and the guard);
- `docs/engineering/architecture.md` (budgets).

**Ownership:** `firmware/src/app/editor.c` (debug commands only, inside `#if X0X_DEBUG`), a new `tools/rc_probe.py`, `docs/engineering/rc1-probe.md`, and the Makefile targets `fw-debug` and `fw-debug-check`.

## Build
1. **`make fw-debug`** builds with `X0X_DEBUG=1` on deltron into `build/debug/x0x.fwsc`. `tools/check_loader.sh` must pass on it.
2. **Debug command `ED_STRESS n`**, debug builds only:
   - n=0 stops;
   - n=1 is the X0X baseline: the sequencer plays a busy built-in pattern on 909 + 808 + 303A + 303B;
   - n=2 is LIVE: enter LIVE with a generated song, then launch a busy clip on every role, with CHORDS on FM6 playing 4-note chords at `ENG_FM6_VOICES` and FX on NOISE;
   - n=3 is n=2 plus every punch FX that is wired (skip the ones that aren't);
   - n=4 is n=2 with the FM6 voice cap temporarily raised to 6, then to 8 (a second argument selects the cap). This measures headroom; it is a debug-only override.

   The command replies with an ACK. It must work from any screen. It may only do what a user could do, plus the voice-cap override.
3. **Debug command `ED_STATS`** replies with `audio_cpu_pct`, `audio_peak_pct`, `audio_xruns`, the guard state, every PROF bucket, `.bss`/pool high-water if available, core-1 on/off and gave-up state, and the FM6 voices active. This saves resolving symbol addresses for PEEK. `ED_STATS_RESET` clears peak and xruns.
4. **`tools/rc_probe.py`** (uses `.venv` mido): `--stress N [--cap K] --seconds S`. It resets the stats, starts the stress, samples `ED_STATS` every 500 ms for S seconds, stops the stress, and prints a table plus a JSON summary: the max and mean CPU, the peak, the xrun count, guard engagements and per-bucket means. `--info` passes through to `tools/fm1_install.py --info`. It never installs anything.
5. **Host test:** the runner gets a command to call `ED_STRESS` and `ED_STATS` through the same code path. A scenario starts each stress level and checks the stats reply is well formed and that LIVE actually plays (events present).
6. Run `make unit-core scenarios check fw fw-debug`, and `tools/check_loader.sh` on both packages.

## Done
- `docs/engineering/rc1-probe.md` describes how to run it and how to read the numbers. Pass criteria for RC1: mean CPU under 70%, max under 85%, 0 xruns over 60 s at stress 2, and the guard never engaged at stress 2.
- Commit, write `reports/RC1p.md`, and send `worker_done`.
