# RC1p: measuring on the device without hands

Builds the tools for RC1's CPU-headroom measurement: a `X0X_DEBUG=1` firmware, three debug editor
commands (`ED_STRESS`, `ED_STATS`, `ED_STATS_RESET`), and `tools/rc_probe.py`, which drives them
over USB and summarises the numbers. **Nothing was installed or flashed.** The measurement itself
is the coordinator's: flash `build/debug/x0x.fwsc` with `tools/fm1_install.py` and run the probe.
See [rc1-probe.md](../docs/engineering/rc1-probe.md) for the full how-to.

## What was built

- **`make fw-debug` / `make fw-debug-check`** (Makefile). `fw-debug` pushes, builds with
  `X0X_DEBUG=1` on deltron, and brings back `build/debug/x0x.fwsc` (+ `x0x.bin`, `x0x.elf`,
  `x0x.dis`). `fw-debug-check` runs the forbidden-symbol check on it and `tools/check_loader.sh`.
- **`firmware/src/app/rc_debug.{c,h}`** (new). The debug-only stress and stats, include-style so
  the device (`editor.c`, inside `#if X0X_DEBUG`) and the host runner (`host/x0x_host.c`) run the
  **same code**. It never touches the project or flash: the busy content is its own generated song
  in `rc_pat` (the `.pool` on the device), so a run cannot dirty the project or autosave.
- **`ED_STRESS n [cap]`** (editor cmd 60). Level 1 is the X0X baseline (909 + 808 + 303 A + 303 B,
  the sequencer's own path); 2 is LIVE with a busy clip on every role and a **held 4-note FM6
  chord** on CHORDS plus NOISE FX; 3 would add every wired punch FX (T10i has wired none yet, so it
  equals 2); 4 raises the FM6 cap to `cap` (default 6, up to 8) with the guard off. It replies with
  an ACK and works from any screen; 0 stops and restores the sequencer and the view.
- **`ED_STATS` / `ED_STATS_RESET`** (61/62). `ED_STATS` replies with `audio_cpu_pct`,
  `audio_peak_pct`, `audio_xruns`, the guard state and count, core-1 on/gave-up, FM6 voices active,
  every PROF bucket, `.bss`/`.pool` sizes and the IRQ-stack high-water (21 fields). Reset clears the
  peak and the xruns.
- **`tools/rc_probe.py`** (new). `--stress N [--cap K] --seconds S`: resets the stats, starts the
  stress, samples `ED_STATS` every 500 ms, stops the stress, prints a table and a JSON summary
  (mean/max CPU, peak, xruns, guard engagements, per-bucket shares, and the RC1 pass checks at
  stress 2). `--info` passes through to `fm1_install.py --info`. It never installs.
- **Host test** (item 5). `host/x0x_host.c` gains `stress` / `stats` / `statsreset` and the
  `stresslevel` / `stressactive` / `statswords` expects, calling the same `rc_*` functions;
  `tests/scenarios/rc1_probe.x0x` starts every level, checks the stats reply is well formed
  (`statswords 21`) and that LIVE plays (`liveclip0`, `fm6voices 4`, non-silent audio).

`tools/build.py` compiles a `X0X_DEBUG=1` build with `-DENG_FM6_VOICES=8` (release keeps 4), so the
cap can be raised without an `eng_fm6.c` change. Level 2 holds exactly **4** notes so it matches a
release build; the 6/8 caps are a device-only measurement.

## Results

- **`make unit-core`** — all suites pass (clip, live, engines, fm6bank, store, take, midi, fx, name).
- **`make scenarios`** — all scenarios pass, including the new `rc1_probe`; every golden matches.
  The new scenario's stress 2 holds 4 FM6 voices, the guard stays off (`guard 0`, `guardcount 0`),
  and the output peaks at −7.9 dBFS.
- **`make check`** (release `make fw`) — forbidden symbols, flash guard, soft-double and size checks
  pass. RAM is unchanged from T5i: `.data+.bss` 96,204 of 98,304, pool 261,296 of 344,064.
- **`make fw-debug-check`** — the debug build compiles, `check_symbols` finds no forbidden symbols,
  and `tools/check_loader.sh` passes. Debug RAM: `.data+.bss` 96,284, pool 279,456 of 344,064 (the
  +18 KiB is the stress song; release is untouched).
- **`tools/check_loader.sh`** on **both** packages — both loaders are byte-identical to X0X 1.0.5
  (`ca93577f…`).
- **`make parity`** — WASM matches native (audio, screen, lights). **`make screens`** — the tour
  renders.
- **On the attached FM-1** (port `Felucca`, `FM-1_900 [running]`): `rc_probe.py --info` returns the
  identity, and a bare `ED_STATS` gets **no reply**, i.e. the currently flashed (release) build does
  not expose the debug commands — the gating works. It must be replaced with `build/debug/x0x.fwsc`
  before measuring.

## For the coordinator

1. Flash `build/debug/x0x.fwsc` (loader is X0X 1.0.5's, so the unattended rule allows it).
2. `.venv/bin/python tools/rc_probe.py --stress 2 --seconds 60 --json build/rc1-stress2.json`
   and record the JSON in [open-questions.md](../docs/open-questions.md) with the RC1 result.
3. If it fails, `--stress 4 --cap 6` and `--cap 8` show the headroom; the FM6 share says how much
   the cap must come down.

Deviations worth knowing: stress 3 equals stress 2 until T10i wires punch FX; CHORDS is a held
chord on the FM6 PLAY layer rather than a launched clip (the LIVE mapper derives only 3-note chords,
and the point is a controlled voice count); the host build's FM6 array is the release's 4, so the
scenario covers `--cap 4` and the 6/8 caps are device-only.

## Files

New: `firmware/src/app/rc_debug.{c,h}`, `tools/rc_probe.py`, `tests/scenarios/rc1_probe.x0x`,
`docs/engineering/rc1-probe.md`, this report. Changed: `firmware/src/app/editor.c` (the three
commands, inside `#if X0X_DEBUG`), `host/x0x_host.c` (the runner commands and expects),
`tools/build.py` (`-DENG_FM6_VOICES=8` for `X0X_DEBUG=1`), `Makefile` (`fw-debug`,
`fw-debug-check`). No existing part table, stored format or golden changed; no flash was written.
