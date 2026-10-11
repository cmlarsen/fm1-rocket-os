# ADR-0001: Fork an existing GPL-3.0 FM-1 firmware; do not write a base

- **Status:** Accepted in principle, 2026-10-10. The owner confirmed that we fork rather than build from scratch and that the project is open source under GPL-3.0. **Fork target: X0X** (`charlesvestal/fm1-x0x` v1.0.5, `6cffa40`), confirmed by the owner 2026-10-10. The evidence is in the [fork evaluation](../research/fork-evaluation.md).
- **Context:** [firmware landscape](../research/firmware-landscape.md) · [dev tooling](../research/dev-tooling.md) · [hardware](../research/fm1-hardware.md)

## Decision
- **Fork** the open FM-1 firmware that is closest to the target. Its HAL, loader, USB rescue, storage, engines, UI framework and emulator all come with it.
- **Port** features from other GPL-3.0 forks where they fit better, keeping their attribution.
- **License** the project GPL-3.0, and publish the source alongside every binary.

## Candidates
1. **Felucca.** The upstream platform. It is the most mature and has the most hardware time. It includes FM6 (msfa) along with 12 other engines, a WASM emulator, about 60 host tests, a screen sweep with layout lint, and audio goldens. Its main gap is that each track has only one pattern.
2. **Melodee.** A Felucca fork with 8 banks per track, queued to each track's loop end. That is the closest shipping feature to per-role clips. It keeps FM6 and uses both cores.
3. **FM1 Move.** The only true clip grid (8 tracks × 6 clips plus scenes), but it has never run on hardware.
4. **X0X.** Has the cleanest `plat.h` boundary, a scenario-script host runner, per-part cueing and song record. Its engines are drum-focused.

**The evaluation ranks X0X first.** It already has:
- per-part cued patterns (`ppat`/`pcue`), which is effectively clips per role with a bar queue
- song record of cues and mutes
- motion lanes
- an engine command queue we can tap for session logs
- the `plat.h` boundary
- a scripted headless scenario runner
- a WASM emulator with a parity test
- hardware safe mode with a single-core fallback

Its gaps are sound: there is no FM6 and no poly synth. FM6 can be ported from Felucca as a self-contained Apache-2.0 module, which is the cheapest gap of any candidate. Felucca ranks second: it has the strongest platform and engines, but we would have to rewrite its sequencer model and add a scenario runner.

The deciding factors were: how much work it takes to reach N clips per role with quantized launch, how healthy the upstream is (can we still pull HAL fixes from Felucca?), and the quality of the test and emulator infrastructure.

## Consequences
- Every binary ships with its source. The proprietary JieLi SDK and RTOS libraries are never linked into it.
- We inherit the fork's code style and should change it only where our work requires it.
- Every fork, X0X included, branched from Felucca 1.0. Felucca's later HAL, loader, USB and gfx fixes have to be **ported by hand**, because Felucca publishes squashed releases. We add Felucca as a second git remote so we can diff against it.
- We must strip X0X's BREAK generator, which is used under a permission-only licence.
- We must preserve the attributions for the code we port (see [fork plan](../research/fork-evaluation.md#fork-plan)).

## Superseded
An earlier draft proposed Dinghy, a stripped-down platform template, plus a new core. The owner rejected that approach because it rebuilds too much.
