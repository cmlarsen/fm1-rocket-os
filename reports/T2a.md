# T2a: test harness

Branch `cmlarsen/t2a-lane`, started from `integration`. New runner commands, goldens with a
comparer and `GOLDEN_UPDATE=1`, the `unit` / `scenarios` / `screens` / `parity` make targets, and
goldens for three existing scenarios (transport, cue, mute) plus the new harness scenario.

The one change outside `host/` and `tests/` is the coordinator-approved event tap: a single
`void (*engine_tap)(uint32_t sample, uint32_t cmd)` in `engine.c`/`engine.h` (NULL by default,
one check in the command queue's drain path), so the runner and, later, T9's take recorder read
the real engine command stream with its sample time.

## Runner commands added (`host/x0x_host.c`)

Documented in the file's header comment, in the same style:

| Command | What it does |
|---|---|
| `dump FILE.json` | one JSON object: `transport` (`playing`, `bpm`, `bar`, `beat`, `step`, `sample`), `parts` (per part `ppat`, `pcue` `-1` if none, `mute`), `view`, `leds` (`keys`: 27 levels in pitch order F3..G5, `0` off / `1` dim / `2` bright; `buttons`: the lit buttons). `sample` = frames rendered since boot (the only clock); `bar` = the 909's wraps since the transport started; `beat` = `pos/4`, `step` = `pos` (0-based) |
| `wait-bar N` | run until N bar boundaries have passed. The boundary is the sequencer's bar (the 909's wrap), not a wall-clock wait; a stopped transport fails after 120 s |
| `events FILE.txt` | start logging every engine command (drum note on, bass note on, bass note off, parameter change) from `engine_tap`, and every cue and mute the app sets, one per line as `<sample> <kind> ...`; `eventsstop` stops it |
| `expect bar N` / `expect beat N` / `expect step N` / `expect sample N` | transport checks (the `expect` bound syntax `<N` / `>N` applies) |
| `expect key K L` | key K's LED level (`L`: 0 off, 1 dim, 2 bright); K as `w0..w15`, `b0..b10` or `0..26` |
| `expect button B L` | button B (its printed label) lit, `L` 0 or 1 |

`events` output kinds: `param T V I VAL`, `drum KIT VOICE VEL`, `bon PART NOTE ACCENT SLIDE`,
`boff PART`, `cue PART PATTERN`, `mute MASK`. The command word is engine.c's `type:4|a:8|b:8|c:12`.

## Goldens

`tests/golden/<scenario>/` mirrors a scenario's output directory: `*.png` (compared by decoded
pixels), `*.json` (by parsed structure), `events*.txt` / any other text (by line), and one
`audio.json` with `frames`, `peak`, `rms` for each WAV (compared within loose bounds — 1 % of the
frame count, 1.5 dB on peak/rms — never by hash, so a DSP change does not churn a golden). The
comparer is `tests/golden.sh` (a wrapper around `tests/golden.py`); it prints the first mismatch
of every file and exits non-zero. `GOLDEN_UPDATE=1` copies the output over the goldens instead
(and writes `audio.json` from the WAVs). A scenario without a golden directory is not compared.

### Seeded goldens

`make scenarios` compares each of these. `make unit scenarios parity` pass; `make screens` runs.

**`harness`** (the new scenario: the commands above) — `harness_playing.png`,
`harness_events.txt`, `transport.json`.

| PNG | What it should show |
|---|---|
| `harness_playing.png` | the 909 part screen (V_PART) while PLAY runs: the kick step 1 and the selected BD track light; one bar has passed |

`harness_events.txt` (sample time, then the event):
```
17664 param 0 0 0 64      # the `param` command sets the 909's first pot to 64
17664 drum 0 0 3071       # black key b0 plays voice 0 (vel 3071/4095)
```
`transport.json` shows `playing 1`, `bpm 125.0`, `bar 1`, `beat 0`, `step 0`, all four parts on
pattern 0 with no cue and no mute, `view 1`, and the LED array.

**`groove`** (transport: PLAY, playing, stop; the only seeded scenario with a WAV).

| PNG | What it should show |
|---|---|
| `01_boot_909.png` | the power-on 909 part grid: empty (no steps lit) |
| `02_909_grid.png` | the 909 grid with the recorded BD/SD/CH/OH steps and the accent row |
| `03_808_grid.png` | the 808 part grid with the cowbell and clap steps |
| `04_tb3po_a.png` | the TB-3PO page for 303 A (a generated line) |
| `05_303b_sound.png` | 303 B's sound page, the filter opened (K1/K2 turned) |
| `07_playing.png` | the 303 B part grid while the 4-bar groove plays (the playhead) |
| `08_home.png` | the HOME view: the pattern bank with the parts |
| `09_fx.png` | the FX screen |
| `10_mix.png` | the MIX screen, MASTER page: the level history, the compressor's GR and AUDIO LOAD |

`audio.json` for `groove.wav` (frames 370432, peak 0.4006, rms 0.0576).

**`patterns`** (cueing a pattern on the bar) — `11_chain.png`.

| PNG | What it should show |
|---|---|
| `11_chain.png` | the HOME view with patterns 1 and 2 chained (white keys 1-2 lit, the chain alternating) |

**`song`** (mute: the song's bar mutes).

| PNG | What it should show |
|---|---|
| `s01_303a_on_p2.png` | the 303 A part screen, moved to pattern 2 by PRESET |
| `s02_home_parts.png` | the HOME view: 303 A on pattern 2, the other parts on pattern 1 |
| `s03_song.png` | the SONG screen with three arranged bars |
| `s04_song_playing.png` | the SONG screen playing bar 3, with the 808 part muted |
| `s05_motion.png` | the 303 A part screen while the recorded cutoff motion plays back |

## Make targets (Mac; no deltron, no toolchain)

| Target | What it runs | Time |
|---|---|---|
| `make unit` | `tests/run_tests.sh --unit`: the host unit suites (fastmath, sequencer, encoder, bass303_lite, uac, trs, undo, lz, motion, drum-pan, stereo, master, storage, host-build). The engine reference comparisons run when their checkout is present; absent, they are skipped with a message rather than failing | ~4.7 s |
| `make scenarios` | every `tests/scenarios/*.x0x` into `build/scenarios/<name>/`, compared with `tests/golden/<name>/` where one exists | ~5.3 s |
| `make screens` | `tests/scenarios/screens_*.x0x` (T2b's tour) and `build/screens/contact.png`, every shot at 1:1 with its file name under it. Until T2b lands there are no `screens_*.x0x`, so it prints that and exits 0 | ~2.5 s (no-op) |
| `make parity` | `tests/parity.sh`: host + `web/emu/build.sh` + `tests/host/emu_test.mjs`; fails with a clear message if `node` (or `emcc`) is missing | ~5.4 s |
| `make test` | unchanged: full `tests/run_tests.sh` on `$(FW_HOST)`, brings back `build/scenarios/` | — |

`tests/run_tests.sh` gained `--unit` (unit suites only) and calls `tests/parity.sh` for the
WASM comparison; the reference comparisons are now skipped (not failed) when their checkout is
absent, so `make test-local` is green on a plain macOS checkout.

`tests/contact_sheet.py` builds `build/screens/contact.png` (verified on two shots: 2 cells,
1:1, name under each); `make screens` uses it once T2b's tour lands.

## Determinism

The golden PNGs must be reproducible, so the host's CPU display is now deterministic: it has no
real CPU counter, so `plat_cpu_pct()` (and the load fed to the overload guard) report the
scripted `load N`, else 0, instead of the host's wall clock. Without this, the MIX MASTER and
PERF screens carried a run-to-run-varying AUDIO LOAD number. Only that screen changed; all
scenarios still pass, and `groove` renders byte-identical PNGs and WAV across runs.

## Notes

- **Test first.** `tests/scenarios/harness.x0x` was written before the commands: it failed with
  `unknown expect button` / `unknown command events`, then passed once the runner grew them.
- **Fonts / Pillow.** The host build generates `build/gen/felucca_font.h` with `tools/gen_font.py`
  (Pillow's `tnum` feature needs libraqm). The Mac's Pillow lacked it; I installed `libraqm`
  (Homebrew) and rebuilt Pillow with it (Pillow 11.3.0, FreeType 2.14.3). Goldens were seeded
  after that, and a clean `rm -rf build/gen && make scenarios` reproduces every PNG.
- **T2b will replace the fonts**, which will churn the seeded PNGs; regenerate them in T2b with
  `GOLDEN_UPDATE=1`.
- The `events` log is the engine command queue (what the app/keys send to the engine), not the
  notes the sequencer directly synthesizes; both the runner spec and the frozen take model treat
  the queue as the event stream.

## Files

New: `tests/golden.sh`, `tests/golden.py`, `tests/parity.sh`, `tests/contact_sheet.py`,
`tests/scenarios/harness.x0x`, `tests/golden/{harness,groove,patterns,song}/`, `reports/T2a.md`.
Changed: `host/x0x_host.c`, `firmware/src/app/engine.c`, `firmware/src/app/engine.h`,
`tests/run_tests.sh`, `Makefile`, `docs/engineering/agentic-dev.md`.

Nothing was flashed.
