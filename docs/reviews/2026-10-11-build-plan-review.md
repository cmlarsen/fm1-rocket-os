# Build plan review (2026-10-11)

A fresh Fable agent reviewed the plan, read-only, against the owner's goals: "We don't want to over engineer, but we want a stable firmware, a good UX, and an intuitive interface." **Applied on 2026-10-11** as build plan v2 and interaction model v2. The owner chose to apply them with the coordinator's notes, cap takes at one per song, and use SCL (not SEL) for the LIVE layer toggle.

Related: [build plan](../engineering/build-plan.md) · [interaction model](../product/interaction-model.md) · [screens](../product/screens.md)

**Verdict:** the research and the fork choice are strong. The plan spends too much on the test harness and on refactors before anyone hears a clip launch on hardware, and the gesture map has three collisions.

## Recommendations, highest impact first

1. **Flash hardware on day one, not at G2.**
   - **RC0** comes right after T0.1. It is X0X 1.0.5 unchanged, so flashing it is nearly risk-free, and it proves the toolchain, the package format and the installer.
   - **A second flash** comes after T2.1 and T2.2 to measure CPU headroom on the device. Host instructions per sample are only a proxy.
2. **Build on deltron.local instead of Docker on the Mac.**
   - It removes T1.1's top risk.
   - `-m32` sanitizer builds and QEMU with KVM run natively there.
   - `make fw`, `make check` and `make emu-binary` would rsync and ssh to `FW_HOST`.
   - Docker stays only as a documented fallback. GitHub Actions is dropped from W1.
3. **Cut T0.2 (restructure) and T2.4 (split `ui.c`).**
   - Renaming paths makes every future upstream port a hand rewrite.
   - Keep X0X's layout. Add `core/` only for new files. LIVE becomes a new view file.
4. **Defer T2.3.** The loader is the only route to bricking a device, and USB UAC 48 kHz isn't needed for v1. Port the input debounce only if the hardware shows a problem.
5. **Fix three gesture collisions:**
   - **HOME hold** opens the cheat sheet *and* exits LIVE after 1 s. Exit LIVE only by long-pressing PLAY.
   - **Holding a pad** means copy (STUDIO), quick assign (LIVE) or clear (with HOME). Move quick assign into LIVE SETUP.
   - **SEL** confirms everywhere except LIVE, where it toggles layers. Keep it, but specify that it never confirms in LIVE and that turning SELECT does nothing there. Alternatively, test SCL as the toggle.
   - Also drop "SAVE paste", and document or drop "OCT±: clip bank".
6. **Resolve the save-model contradiction.**
   - Principle 1 says there is no gap between what you hear and what is saved. Principle 8 says autosave never overwrites an explicit save, which creates exactly that gap.
   - Fix: one song is one state, autosaved while stopped. SAVE means "save now", and undo covers record passes.
   - Cap takes at one per song in v1, because each take is 10–20 KB against about 292 KiB of flash.
7. **Narrow "bit-identical".**
   - X0X uses floats, and x86 libm won't match pi32v2, so host vs device can't be bit-identical.
   - Require host ↔ WASM bit-identical output only. Compare the device by its event stream.
   - T4.1 takes: event-exact everywhere, audio-hash-exact on the host only.
8. **Trim the v1 feature surface.**
   - **Quantize:** Bar (default), Beat, End of clip. Drop 2/4/8 bars and legato.
   - **Punch FX:** 4–6 instead of 16.
   - **Defer:** extra pad maps, scenes, the recently-deleted song slot, migration tests (keep the load fuzzer), capture, MIDI export and CI.
9. **Right-size the orchestration.**
   - The disjoint ownership across T3.x can't hold, because `ui.c` is a unity build with a shared `song_t`.
   - Run W0–W2 with one serial agent while the owner does the hardware loop. Allow at most 2 parallel in W3 and W4, after a serial "model + storage" task.
   - Split T3.3, T3.2 and T4.2.
   - Move T1.3's role `expect` keys after T2.1.
   - That gives about 11 tasks.
10. **Move G1 (design sign-off) before W1.** The owner is idle during W1 and W2. Also reconcile the countdown, which is still specified in `interaction-model.md`, `architecture.md` and roadmap M2.
11. **Check text sizes on hardware at RC0** before setting the lint minimum. S 12 px on a 1.54" panel is the "reading glasses" risk.
12. **Unify the UI grammar with the inherited X0X pages.**
    - X0X uses ALGORITHM for part and PRESETS for pattern, which conflicts with our grammar.
    - Decide which X0X views survive and relabel them.

## Keep as is
- The X0X fork with FM6 and NOISE ported from Felucca.
- `plat.h` as the boundary, the C99 rules, and never requiring core 1.
- Agents never flash. The no-write flash regions stay. Rescue and return-to-stock stay.
- The sample-counter clock, scenario-first TDD, the `GOLDEN_UPDATE=1` discipline, and the 1:1 review rule.
- The zone pad map, role+letter clip names, and copy-then-clear.
- The hold-to-confirm ring, the undo toast, the stage lock, and long-pressing PLAY for LIVE.
- A/B CRC records and autosave while stopped.
- Takes as event logs, GPL hygiene and the BREAK strip.
- Docs as the project brain.

## Coordinator response (to be confirmed by the owner)
- **Agree:** 1, 2, 3, 5 (HOME hold and pad hold), 6 (save model), 7, 8, 9, 10, 11 and 12.
- **Partly disagree with 4.** The loader's pre-erase CRC check (Felucca "step 2b") *reduces* brick risk: it rejects a corrupt package before anything is erased. Keep that one port, gated by the loader tests and an RC flash. Defer the USB and descriptor ports as suggested.
- **Owner's call on 5 (SEL):** the owner chose SEL for the layer toggle. Keep it, with the "never confirms in LIVE" rule, unless an emulator test shows accidental presses.
- **Owner's call on 6 (one take per song):** R14 asks for performance recording, not a take library. One take per song meets it, but this is a scope cut the owner should approve.
