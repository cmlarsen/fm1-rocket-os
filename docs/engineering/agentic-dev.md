# Agentic development loop

This page describes how an agent (Claude) builds and verifies a change end to end with no human and no hardware. The commands below are **planned targets**. They don't exist until milestone M0 in the [roadmap](../product/roadmap.md) builds them. When a target lands, update this page so it matches reality.

Related: [architecture](architecture.md) · [screen design](screen-design.md) · [dev tooling research](../research/dev-tooling.md)

## Principles
- **Evidence before claims.** A change is done only when every tier below passes and the agent has looked at the artifacts it produced (PNG diffs, contact sheets, WAV metrics).
- **Fast tiers first and fail fast.** Every target exits non-zero on failure and prints the paths of its artifacts.
- **Goldens change only on purpose.** The agent runs `GOLDEN_UPDATE=1`, views the new PNGs or contact sheet, and states what changed and why in the commit message.
- **The host and WASM builds never need the JieLi toolchain.** `make fw`, `make check` (it uses `-m32`) and `make emu-binary` run on the Linux build host `deltron.local` (`FW_HOST`), using rsync and ssh. Everything else runs on the Mac.
- **Determinism scope.** Host and WASM output is bit-identical. Device behaviour is checked by its event stream.

## Test pyramid

| Tier | Target | What it checks | Speed |
|---|---|---|---|
| 1. Static | `make check` | Host compile with `-Wall -Werror`, ASan/UBSan and `-m32`. Forbidden-symbol checks (no 64-bit division, no heap, no HAL outside `targets/`). | seconds |
| 1b. Device compile | `make fw` | JieLi Clang on deltron.local, size and RAM budget, static pi32v2 CPU budget (Felucca's `target_budget.py` approach) | ~1 min |
| 2. Unit | `make unit` | Golden lists of sequencer events (swing, polymeter, quantized launch, one-shot, legato, take replay). Model round-trip and migration. UI reducer transitions. Fuzzers for loading songs and takes. | ms |
| 3. Scenarios | `make scenarios` | The headless host runner executes `tests/scenarios/*.scn`. Each produces PNG, WAV, a JSON state dump, an LED log and a MIDI log. | seconds |
| 4. Screen goldens | `make screens` | Every screen in every state is rendered to PNG and compared pixel-exact against `tests/golden/screens/`. A diff PNG is written on failure. A layout lint flags overlap, clipping, minimum text size and contrast. | seconds |
| 5. Audio goldens | (part of 3) | A hash per render, plus tolerant metrics: peak, RMS, DC, clicks, pitch | seconds |
| 6. WASM parity | `make parity` | Node runs the same scenarios against `fm1.wasm`. Framebuffers and audio must be byte-identical to native. | seconds |
| 7. Browser E2E | `make e2e` | Serve the emulator page and drive `window.fm1.{press,turn,key,wait,screen,leds,state}` through `~/.claude/bin/ab`. Assert that the canvas matches the golden, that the worklet produces audio, and that input mapping works. | ~30 s |
| 8. Binary check | `make emu-binary` | Run the real `.fwsc` in [fm1-emulator](https://github.com/simonjohansson/fm1-emulator) (QEMU) headless with an input script, capture the LCD at fixed guest times, and compare against the host goldens. | ~1 min |
| 9. Hardware | owner | RC0–RC3 gates: flash, smoke test, measure CPU headroom and text legibility | — |

## Scenario script format (planned, modelled on X0X and ChoralRoot)
```
# tests/scenarios/launch_quantized.scn
tempo 120
tap PLAY
wait 100
key Cs4 down        # launch Bass A (black key cluster 2)
wait 50
key Cs4 up
expect role.bass.queued 0      # clip index 0 queued
expect role.bass.playing none
wait-bar 1
expect role.bass.playing 0
shot launch_after_bar.png
wav launch.wav
expect audio.peak_db_min -30
```
- Time is simulated, so tests are deterministic and run faster than real time.
- `expect` reads named fields from the core state dump.
- Each scenario documents a single behaviour. **When you fix a bug, add a scenario that reproduces it first.**

## The loop for one change
1. Read the relevant docs: [requirements](../product/requirements.md), [UX principles](../product/ux-principles.md), [interaction model](../product/interaction-model.md) and [architecture](architecture.md).
2. Write or extend a unit test or scenario that fails (TDD).
3. Implement the change in `core/`. Never put logic in `targets/`.
4. Run `make check unit scenarios screens`. Read each failure's diff artifacts.
5. For UI changes, view the contact sheet at 1:1 and at 2×, and check it against the [UX principles](../product/ux-principles.md).
6. Run `make parity e2e`. Before any release candidate, also run `make fw emu-binary`.
7. Update the docs. If behaviour changed, update the interaction model. Record any new decision in an ADR. Fold anything learned back into the relevant `docs/` page.

## Hardware safety rules for agents
- **Agents never flash hardware.** Flashing is a human release step.
- Agents never write code that erases or programs outside the user-data regions listed in [architecture](architecture.md#constraints-that-shape-everything).
- Every OTA and loader change must keep the fork's USB rescue and the "return to stock" path working.
