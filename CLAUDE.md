# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

fm1-rocket-os is custom firmware for the M-VAVE FM-1 pocket synth. It turns the FM-1 into a standalone clip-launching performance instrument: per-role clips on the keys, quantized launching, songs, and performance recording. The top product requirement is that it is **very user friendly**.

**Status:** research, design and planning only. Building follows [build-plan.md](docs/engineering/build-plan.md): Orca child-worktree workers in waves, with owner gates. G0 and G1 are cleared and the overnight run of 2026-10-10 is under way (T1 onward, stopping at RC1).

## The docs are the project brain

Read [docs/README.md](docs/README.md) first. It indexes everything. Keep the docs current:
- Behaviour changes go into [interaction-model.md](docs/product/interaction-model.md).
- New decisions become ADRs in `docs/decisions/`.
- Answered questions are removed from [open-questions.md](docs/open-questions.md).
- What you learn goes into the relevant doc, not only into the commit message.

[requirements.md](docs/product/requirements.md) is the source of truth for scope. [ux-principles.md](docs/product/ux-principles.md) holds the rules every screen and gesture must follow.

## Hard constraints

- **The CPU is JieLi pi32v2, not ARM.** The only compiler is JieLi Clang 4.0.1, which runs on Linux x86-64 only, so it runs on deltron.local.
  - Device code is C99/gnu99.
  - Device code must have no heap, no 64-bit division, and no `double`. The FPU is single precision, so `float` is allowed, but new core code should prefer integers.
  - Never *require* the second core. Some units misread RAM on core 1.
  - No Rust or arm-gcc on the device.
  - See [ADR-0002](docs/decisions/0002-language-c.md).
- **Fork, don't rebuild.** The project forks an existing GPL-3.0 FM-1 firmware and evolves it. The base is **X0X** (confirmed) (charlesvestal/fm1-x0x); FM6, NOISE and the newer gfx get ported from Felucca. See the [fork evaluation](docs/research/fork-evaluation.md). Do not write a new HAL, loader or base. Put product code in new files where possible, so upstream platform fixes still merge. The project is open source under GPL-3.0. Never link JieLi's proprietary SDK or RTOS libraries. Never copy code, assets or manual text from GrooveOS (closed, and its terms forbid reverse engineering) or from fm1-polyseq (no licence). See [ADR-0001](docs/decisions/0001-base-platform.md).
- **Sound design.** Keep the inherited FM6 engine and its pages, but keep them out of the LIVE path. Choosing a sound is preset browsing by default. See [ADR-0003](docs/decisions/0003-sound-design-scope.md).
- **Flash safety.**
  - Never write flash regions `0x0000–0x3FFF`, `0xE8000`, `0xFF000`, or BTIF.
  - Keep USB rescue and "return to stock" working.
  - Agents never flash hardware; flashing is a human release step.
  - Flash writes stall XIP and glitch audio, so save only while stopped and idle.
- **Determinism.** The audio sample counter is the only clock, and there are no wall-clock reads in `core/`. Host and WASM output must be bit-identical. The device is checked by its event stream.
- **Build host.** `deltron.local` (x86-64 Ubuntu, ssh as `caleb`) runs `make fw`, `make check` and `make emu-binary` through `FW_HOST`. The Mac runs the host and WASM tiers.
- **Keep X0X's layout.** Don't move or rename inherited files. New code goes in new files.

## Architecture (planned)

See [architecture.md](docs/engineering/architecture.md).

- **`core/`** is pure C: sequencer and clip engine, model, UI reducer and views, gfx, DSP.
- **`plat.h`** is the only hardware boundary.
- **`targets/fm1`** runs on the forked Felucca HAL.
- **`targets/host`** is a headless scenario runner that outputs PNG, WAV and JSON.
- **`targets/wasm`** is the browser emulator (AudioWorklet).
- **Logic never goes in `targets/`.**
- **Modes:** STUDIO builds the song. LIVE is entered and left with a long press on PLAY. In LIVE, **SCL** toggles the PADS and PLAY layers, and REC records the song's single take.
- **Data model:** a Song holds Roles; each Role holds Clips, and one clip per role plays at a time. A Take is a recorded performance, stored as an event log, never as audio.

## Development loop (planned; build these in M0, then update this section with real commands)

See [agentic-dev.md](docs/engineering/agentic-dev.md). The tiers below run fastest first:

1. `make check`: static checks plus sanitizers.
2. `make unit`: sequencer event goldens and model round-trip tests.
3. `make scenarios`: scripted `tests/scenarios/*.scn` runs, each producing a PNG, WAV and state dump.
4. `make screens`: golden PNG at 1:1 plus layout lint.
5. `make parity`: WASM output must match native byte for byte.
6. `make e2e`: emulator page driven with `~/.claude/bin/ab`.
7. `make fw` and `make emu-binary`: the JieLi build on deltron.local, then the real `.fwsc` is run in the QEMU fm1-emulator.

Rules for the loop:
- Write a failing scenario or unit test first.
- Regenerate goldens only with `GOLDEN_UPDATE=1`. Then view the new PNGs and say in the commit what changed.
- For UI work, check the contact sheet at 1:1 on 240×240 against the UX principles. Mockups at larger scales hide legibility problems.
- Screen designs: the review canvas https://claude.ai/artifact/VrmkPN8wop1rqGqmc2o8PL. Its source is in `design/project/` (shared tokens in `fm1.css`, reusable parts `Header`, `Knobs`, `PadGrid` and `Keys`). The inventory is in [screens.md](docs/product/screens.md).
- Screen design flow: [screen-design.md](docs/engineering/screen-design.md). The owner iterates on screens with the `/design` skill.

## Reference repos worth reading before implementing

- Felucca: platform, `gfx.c`, `seq.c`, `song_chain.c`, `tests/ui_render.c`, `web/emu/`. https://github.com/hugelton/Felucca
- Melodee: banks per track, queued to each track's loop end. https://github.com/keremimo/melodee
- FM1 Move: clip grid and key-light states (never run on hardware). https://github.com/Nicodesy06/FM1-MOVE
- X0X: `plat.h` boundary, `host/x0x_host.c` scenario runner, per-part pattern queueing, song record. https://github.com/charlesvestal/fm1-x0x
- Lunar Modulator (MIT): CI matrix and verification ladder. https://github.com/ip2k/lunar-modulator
- fm1-emulator: QEMU, runs real `.fwsc` headless. https://github.com/simonjohansson/fm1-emulator
