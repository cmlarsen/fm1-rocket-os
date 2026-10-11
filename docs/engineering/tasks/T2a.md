# T2a: test harness

Part of T2 in [build-plan.md](../build-plan.md). T2b (fonts, lint, `make check`) runs at the same time in another worktree; stay inside your ownership paths so the two merge cleanly.

Read first: `CLAUDE.md`, `docs/engineering/agentic-dev.md`, the worker rules in `docs/engineering/build-plan.md`, the command list at the top of `host/x0x_host.c`, `tests/run_tests.sh`, and the top-level `Makefile` (from T1).

**Ownership:** `host/`, `tests/` (except `tests/scenarios/screens_*.x0x` and `tests/lint/`, which belong to T2b), and the `unit`, `scenarios`, `screens`, `parity` targets in `Makefile`. Do not touch `firmware/src/gfx*`, fonts or the `check` target.

## Goal
Give agents a fast, deterministic way to prove behaviour: scripted scenarios with goldens for screen, LEDs, events and audio, all runnable on the Mac.

## Steps (test first: write a scenario that fails, then make it pass)
1. **New runner commands** in `host/x0x_host.c` (document each in the header comment, in the same style):
   - `dump FILE.json`: one JSON object with `transport` (`playing`, `bpm`, `bar`, `beat`, `step`, `sample`), `parts` (for each part: `ppat`, `pcue`, `mute`), `view`, `leds` (`keys`: 27 values in pitch order F3..G5, 0 = off; `buttons`: object of lit buttons). The sample counter is the only clock.
   - `wait-bar N`: run until N bar boundaries have passed (use the sequencer's bar boundary, not milliseconds).
   - `events FILE.txt`: start logging every engine command (note on/off, parameter change, cue, mute) with its sample time, one per line; `eventsstop` stops it.
   - New `expect` keys: `bar`, `beat`, `key K` (LED level of key K), `button B` (lit 0/1).
2. **Goldens.** `tests/golden/<scenario>/` holds the expected `*.png`, `*.json`, `events*.txt` and an `audio.json` per WAV (`frames`, `peak`, `rms`, `sha256` of the PCM). A comparer (`tests/golden.sh` or a small C/Python tool) diffs a scenario's output dir against its golden dir and prints the first mismatch per file. `GOLDEN_UPDATE=1` copies output over the goldens instead. Scenarios without a golden dir keep working as today.
3. **Make targets** (Mac, no deltron needed; build the host binary with `host/build_host.sh`):
   - `make unit`: the host unit suites from `tests/run_tests.sh` that run on macOS.
   - `make scenarios`: every `tests/scenarios/*.x0x`, each into `build/scenarios/<name>/`, compared with goldens where they exist.
   - `make screens`: runs `tests/scenarios/screens_*.x0x` (T2b writes these; until then the target is a no-op that says so) and builds `build/screens/contact.png`, every shot at 1:1 in a grid with its file name under it.
   - `make parity`: the existing WASM-vs-native parity test (find it under `web/` or `tests/`). If Node is missing, fail with a clear message.
   - Keep `make test` (remote, full) working.
4. **Seed goldens** for 3 existing scenarios that cover transport, cueing a pattern, and mute. Run with `GOLDEN_UPDATE=1` and list every PNG in the report with what it should show; the Opus reviewer looks at them (you cannot view images).
5. Prove determinism: run `make scenarios` twice; outputs must be byte-identical.

## Done
- `make unit scenarios parity` pass on the Mac; `make screens` runs.
- A deliberately broken expectation fails `make scenarios` with a readable message (show it in the report, then revert it).
- Small commits with the attribution lines from `CLAUDE.md`'s instructions.
- `reports/T2a.md`: commands added, goldens seeded (with what each PNG should show), timings for each make target.
- `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T2a.md`.
