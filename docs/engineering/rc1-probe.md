# RC1p: measuring CPU headroom on the device without hands

RC1 asks one question: **does the FM-1 have the CPU headroom for all five LIVE roles**, FM6
chords included, on a single core? Nobody is at the FM-1, so the answer has to be measured over
USB. This page is the tooling for that measurement: a development firmware build, three debug
editor commands, and `tools/rc_probe.py`, which drives them and summarises the numbers.

**The worker never installs or flashes.** `make fw-debug` produces `build/debug/x0x.fwsc`; the
coordinator (or the owner) installs it with `tools/fm1_install.py`, under the unattended-flashing
rule in [build-plan.md](build-plan.md#overnight-run-2026-10-10). Its loader is byte-identical to
X0X 1.0.5's (`tools/check_loader.sh` passes), which is what that rule requires.

## Build

```sh
make fw-debug        # X0X_DEBUG=1 on deltron -> build/debug/x0x.fwsc (+ x0x.bin, x0x.elf, x0x.dis)
make fw-debug-check  # fw-debug, then the forbidden-symbol and loader checks on it
```

`tools/build.py` compiles a `X0X_DEBUG=1` build with `-DENG_FM6_VOICES=8` (a release build keeps
`eng_fm6.h`'s **4**). That is the only difference that matters to a measurement: the FM6 adapter's
voice array can hold eight voices, so the stress command can raise the cap without touching
`eng_fm6.c`. It also means a debug build's `.pool` is ~17 KiB larger (the stress song); a release
build is unchanged.

## The debug commands

They ride the editor protocol (`F0 7D 46 4C cmd args.. F7`), the same one `tools/fm1_debug.py`
uses. A number is five 7-bit words, least significant first. They exist only in `X0X_DEBUG` builds;
a release build answers none of them (and the editor never replies to an unknown command).

| Command | Code | Args | Reply |
|---|---|---|---|
| `ED_STRESS` | 60 | `n`, `cap` | `n`, `ok` (1 = started) |
| `ED_STATS` | 61 | – | `count` (= 21), then every field |
| `ED_STATS_RESET` | 62 | – | `1` |

`ED_STATS`'s fields, in order (the Python tool's `ST_*` indices):

| # | Field | # | Field |
|---|---|---|---|
| 0 | `audio_cpu_pct` (mean block load, %) | 8–16 | every PROF bucket (909, 808, 303A, 303B, FX, MASTER, SEQ, FM6, NOISE) |
| 1 | `audio_peak_pct` (highest block since reset) | 17 | `.data+.bss` in use, bytes |
| 2 | `audio_xruns` (dropped halves since boot) | 18 | `.pool` in use, bytes |
| 3 | guard engaged (1/0) | 19 | `.pool` region size (0x54000) |
| 4 | guard engagements (count) | 20 | the interrupts' stack high-water, bytes |
| 5 | core 1 running (1/0) | | |
| 6 | core 1 gave up (0 no, 1/2 why) | | |
| 7 | FM6 voices sounding now | | |

`.bss` and `.pool` have no runtime high-water on this firmware, so their **sizes** are reported;
the interrupt stack is the one high-water that exists (`plat_irq_stack_used`).

### `ED_STRESS n [cap]` — the levels

It may only do what a user could do (plus the voice-cap override), and it works from any screen.
The busy content is **generated in the debug build, never loaded from or written to the project**:
a fixed 909/808 pattern (16ths, accents) and two TB-3PO 303 lines with slides, entered through the
same LIVE path the UI uses (live entry, clip launches, the PLAY-layer keys). A run cannot make the
project dirty, so it cannot autosave.

| n | What it plays | FM6 voices | Guard |
|---|---|---|---|
| 0 | stops the stress and puts the sequencer and the view back | – | restored on |
| 1 | the X0X baseline: 909 + 808 + 303 A + 303 B, the sequencer's own path | – | on |
| 2 | LIVE: a busy clip on DRUMS/BASS/LEAD/FX, **CHORDS a held 4-note FM6 chord**, FX on NOISE | **4** | on |
| 3 | level 2 plus every wired punch FX (T10i has not wired any yet, so it equals 2 for now) | 4 | on |
| 4 | level 2 with the FM6 cap raised to `cap` (default 6; `cap` 6 or 8) | `cap` | **off** (the guard would shed FM6 to two voices, which is not what "the cap" means) |

Level 2 holds exactly **four** notes on purpose: a release build's `ENG_FM6_VOICES` is 4, so level 2
measures what a release build would actually play, even though the debug array is 8. Level 4 is the
headroom experiment.

## Running the probe

```sh
.venv/bin/python tools/rc_probe.py --stress 2 --seconds 60 --json build/rc1-stress2.json
.venv/bin/python tools/rc_probe.py --stress 4 --cap 6 --seconds 30
.venv/bin/python tools/rc_probe.py --info          # the device identity (fm1_install.py --info)
```

It resets the stats, starts the stress, samples `ED_STATS` every 500 ms for `--seconds`, stops the
stress, and prints a table plus a JSON summary. `--info` is a pass-through to
`tools/fm1_install.py --info`; **nothing here ever installs.** It needs `mido` with
`python-rtmidi` (the repo's `.venv`).

The table is one row per sample (`cpu`, `peak`, `xruns`, `guard`, FM6 voices). The JSON summary is
what RC1 records:

```json
{ "stress": 2, "cap": 0, "seconds": 60.0, "samples": 121,
  "cpu_pct_mean": 41.2, "cpu_pct_max": 58, "peak_pct": 71,
  "xruns": 0, "guard_engagements": 0, "guard_active": false, "fm6_voices": 4,
  "cpu2": true, "cpu2_gave_up": 0,
  "prof_share_pct": {"909": 22.0, "808": 9.0, "303A": 12.0, "303B": 13.0,
                     "FX": 6.0, "MASTER": 5.0, "SEQ": 1.0, "FM6": 24.0, "NOISE": 8.0},
  "pass": {"mean_under_70": true, "max_under_85": true, "no_xruns": true,
           "guard_never": true, "rc1_ok": true} }
```

How to read it:

- **`cpu_pct_mean` / `cpu_pct_max`** are the render's share of the block budget (the same numbers
  the PERF screen shows). Mean is the steady load; max is the worst half buffer.
- **`peak_pct`** is the highest single block seen since the reset — the headroom's upper bound.
- **`xruns`** counts dropped half buffers. Any is a real dropout.
- **`guard_engagements`** is how often the overload guard stepped in during the run (it sheds
  oversampling, NOISE density and FM6 voices). A nonzero count means the CPU was past ~85–92 %.
- **`prof_share_pct`** is each engine's share of the render, from the PROF counter deltas — the
  FM6 share is what the chord costs.

### RC1 pass criteria

At **stress 2** (all five roles, a 4-note FM6 chord, NOISE FX), over 60 s:

- **mean CPU < 70 %**,
- **max < 85 %**,
- **0 xruns**,
- **the guard never engaged**.

`rc_probe.py --stress 2` prints `rc_probe: RC1 PASS|FAIL` from exactly those four checks. If it
fails, the JSON says which one; the FM6 share and the level-4 run then show how much the cap must
come down.

## The host half

`firmware/src/app/rc_debug.c` is include-style: the device's `editor.c` includes it under
`#if X0X_DEBUG`, and `host/x0x_host.c` includes it too, so the runner's `stress N [cap]` and
`stats` script commands call the **same functions** the device's `ED_STRESS` / `ED_STATS` do.
`tests/scenarios/rc1_probe.x0x` starts every level, checks the stats reply is well formed
(`statswords 21`) and that LIVE actually plays (`liveclip0`, `fm6voices 4`, non-silent audio). The
host build's FM6 array is the release's 4, so the scenario exercises `--stress 4 --cap 4`; the 6 and
8 caps are a device-only measurement.

## Files

New: `firmware/src/app/rc_debug.{c,h}`, `tools/rc_probe.py`, `tests/scenarios/rc1_probe.x0x`,
this page, `reports/RC1p.md`. Changed: `firmware/src/app/editor.c` (the three commands, inside
`#if X0X_DEBUG`), `host/x0x_host.c` (`stress` / `stats` / `statsreset`, the `stress*` and
`statswords` expects), `tools/build.py` (`-DENG_FM6_VOICES=8` for `X0X_DEBUG=1`), the `Makefile`
(`fw-debug`, `fw-debug-check`).
