# Architecture (proposed)

This is the planned code structure. Nothing is built yet. It follows the decisions in [ADR-0001 (base platform)](../decisions/0001-base-platform.md) and [ADR-0002 (language)](../decisions/0002-language-c.md).

Related: [agentic dev loop](agentic-dev.md) · [screen design](screen-design.md) · [dev tooling research](../research/dev-tooling.md) · [hardware](../research/fm1-hardware.md)

## Constraints that shape everything
- **Toolchain.** The CPU is a JieLi pi32v2, which only JieLi Clang 4.0.1 can target. That compiler runs on Linux x86-64 only, so it runs on the build host `deltron.local`. Device code is **C99/gnu99**. There is no heap, no 64-bit division and no `double` in the audio path ([hardware](../research/fm1-hardware.md#4-toolchain)).
- **CPU and memory.** One core runs at 240 MHz. There is about 512 KiB of SRAM and 1 MiB of flash, with no SD card.
- **Flash writes stall execute-in-place**, which glitches audio. We only save while stopped and idle, or in small chunks during silence.
- **Audio** comes from a 128-frame half-buffer DMA ISR at 44.1 kHz. The sequencer runs inside the ISR once per control block, as Felucca's does.
- **Regions we never write:** `0x0000–0x3FFF`, `0xE8000`, `0xFF000`, and BTIF. This keeps the device recoverable and keeps a clean return to stock possible.

## Layers

```
core/                pure C. No I/O, no hardware registers, no wall clock.
  seq/               transport, clock (integer ticks, e.g. 96 PPQN, advanced by sample count),
                     clip players, launch queue + quantize, take recorder/player
  model/             song / role / clip / take structs, versioned serialisation, migration
  ui/                UI reducer: (state, input event) -> state'; view(state) -> draw calls
  gfx/               canvas renderer, generated fonts/icons/palette, lint hooks
  dsp/               engines, voices, FX (reuse Felucca / MIT DSP where licensing allows)
plat.h               the ONLY boundary (X0X style): ms clock, buttons/keys/encoders,
                     LEDs, lcd blit, MIDI in/out, storage read/erase/prog, cpu stats
targets/
  fm1/               plat_fm1.c on the forked Felucca HAL (bare-metal, OTA loader, USB rescue)
  host/              headless runner: scenario script -> PNG / WAV / JSON state / LED + MIDI logs
  wasm/              STANDALONE_WASM + AudioWorklet page + window.fm1 test API
tools/               font/icon/palette generators, golden updater, image diff, scenario lint
tests/               unit, scenarios, goldens (screens, audio, seq events), fuzzers
```

**Rules:**
- **Only `targets/*` may include `plat.h` implementations or HAL headers.** A build check enforces this, as Dinghy rejects register access outside `hal/`.
- **The audio sample counter is the only clock.** UI frames run every N ms of *device* time, and input is applied at block boundaries. The PRNG is seeded. Under these rules, **host and WASM output is bit-identical**, which X0X and Felucca already prove. **The device is checked by its event stream.** X0X's float engines and libm differ between x86 and pi32v2, so device audio is not bit-identical.
- **The sequencer exposes `seq_render(ctx, nframes, sink)`**, so tests can assert exact `(sample_offset, event)` lists.
- State lives in a context struct where practical. We avoid scattered globals so tests can run several instances.

## Sequencer and clip engine design notes
- **Clip pool.** Felucca embeds `step_t step[64]` inside `track_t`, which allows one pattern per track. We replace that with a **clip pool indexed by role**, so each role has N clip slots ([landscape implications](../research/firmware-landscape.md#implications)).
- **Launch queue.** Each role has `{pending_clip, at_tick}`. The quantize point is computed when the user presses the key: next beat, next bar (the default), or end of clip. Tapping the same key again cancels. HOME+tap sets `at_tick = now`.
- **Clip modes.** Loop repeats until stopped or replaced. Once plays one pass, then stops the role. Gate and legato are deferred past v1.
- **Motion.** Each clip owns its knob lanes, sampled per step with interpolation (as X0X does). We cap the number of lanes per clip and show which ones are used.
- **Take recorder.** Appends `(tick, type, role, a, b)` events: launch, stop, mute, FX on/off, and knob value (thinned by delta and time). The take player feeds these events back through the same launch queue and parameter paths. Replay must be **event-exact** everywhere and **audio-hash-exact on the host**. Each song holds one take in v1.
- **External MIDI.** USB and TRS MIDI land in the same event queue the panel uses. Program Change maps to clip launches.

## Memory and storage budget
These are first-pass estimates to validate in [M0](../product/roadmap.md).

| Item | Estimate | Basis |
|---|---|---|
| Clip, 64 steps, dense | ~640 B, plus motion | Felucca `step_t` is ~10 B |
| Song: 5 roles × 4 clips, dense | ~13 KB of steps, plus motion and sounds | 20 × 640 B |
| Song with sparse step encoding | a few KB | Store only filled steps |
| Take: 5 minutes with knob moves | ~10–20 KB | ~2–3k events × 6 B |
| RAM pool available | ~360 KiB | [Felucca map](../research/fm1-hardware.md#2-internals) |
| Flash for user data | ~292 KiB with Felucca's layout, more if we drop the 240 KiB sample area | [flash map](../research/fm1-hardware.md#flash-maps) |

**Storage format:**
- Each record is versioned and CRC'd, and written to an A/B sector pair with the commit header written last. This is Felucca's power-safe pattern.
- **Migration is never optional:** each format version has a fuzz test and a load-old-file test.
- Songs export and import as files through the web companion over WebMIDI SysEx.

## Fork, then evolve (GPL-3.0)
**We fork an existing firmware rather than writing a base** ([ADR-0001](../decisions/0001-base-platform.md)). The proposed base is **X0X**. Its `plat.h`, context-struct sequencer, host runner and WASM emulator already match the layer diagram. The [fork plan](../research/fork-evaluation.md#fork-plan) maps its directories onto `core/` and `targets/`. The layer diagram above is the *target* shape. We move toward it gradually inside the fork, and never as a rewrite:
- **Keep from the fork as-is:**
  - HAL, loader, USB rescue and OTA
  - storage driver
  - engines, including FM6 ([ADR-0003](../decisions/0003-sound-design-scope.md))
  - gfx and the font pipeline
  - WASM emulator and host tests
- **Don't restructure.** X0X keeps its directory layout. New modules go in new files, and `core/` holds only new code. The layer diagram above shows the target *responsibilities*, not a move of existing files. This keeps future X0X and Felucca ports mechanical.
- **Add first:** a screen catalogue with goldens and lint, plus runner upgrades. X0X already has the scenario runner and WASM parity.
- **Then change:** replace the per-track single pattern with a per-role clip pool. Add the launch queue, LIVE mode and the session recorder. Put new product code in new files, so we can still merge platform fixes from upstream Felucca.
- **Port from other GPL forks where they fit,** with attribution: X0X `plat.h`/host runner/song record, Melodee bank queueing, FM1 Move clip states.
- **Never use:** JieLi's proprietary SDK or RTOS libraries (GPL incompatibility), GrooveOS code, or fm1-polyseq (no licence).
