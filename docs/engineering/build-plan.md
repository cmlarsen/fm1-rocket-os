# Build plan: orchestrated child agents (v2)

The firmware is built by workers that Orca supervises. One coordinator (Claude, in this repo's Orca workspace) runs a single Orca Run. Each Task is dispatched to a worker in its own **child worktree** (`--worktree new-child`). The coordinator reviews the result and merges it into an `integration` branch.

**Status: launched 2026-10-10 (overnight run).** G0 and G1 are cleared; see [Overnight run](#overnight-run-2026-10-10).

v2 (2026-10-11) applies the [plan review](../reviews/2026-10-11-build-plan-review.md):
- hardware gets flashed early and often;
- deltron.local is the build host;
- there are no pure-refactor tasks;
- the core work runs serially;
- the plan has 11 Tasks, with at most 2 running in parallel.

Related: [roadmap](../product/roadmap.md) · [architecture](architecture.md) · [agentic dev loop](agentic-dev.md) · [fork evaluation](../research/fork-evaluation.md) · [interaction model](../product/interaction-model.md) · [screens](../product/screens.md)

## Shape

```
G0 owner ─► G1 design sign-off ─► T1 import X0X + deltron build ─► RC0 (owner flashes stock X0X build)
   ─► T2 harness ─► T3 grammar + LIVE shell ─► T4 clip engine ─► T5 engines (FM6, NOISE) ─► RC1 (CPU on device)
   ─► T6 storage + songs core ─► ┬ T7 STUDIO clip UI      ┐
                                  └ T8 songs UI + CRUD      ┴► RC2 (first usable release)
   ─► ┬ T9 take recorder + playback   ┐
      └ T10 punch FX + LIVE setup     ┴► T11 MIDI in + undo/help polish ─► RC3 (public RC)
```

- **T1–T6 run serially**, one worker at a time, because they reshape the shared model and `ui.c`.
- **Parallel work happens only in pairs** (T7 with T8, T9 with T10), and only after T6 has frozen the model and the storage format.
- **Hardware gates are owner-only.** RC0, RC1, RC2 and RC3 each produce a `.fwsc` and a checklist. **Agents never flash.**

## Build host
- **`deltron.local`** runs the firmware build natively with the JieLi toolchain. The host reports `x86_64`, Ubuntu 26.04, glibc 2.43, 12 cores, 30 GB RAM, 49 GB free, plus Docker, gcc, rsync and qemu (checked 2026-10-11).
- **Targets that run there:** `make fw`, `make check` (it uses `-m32`, which arm64 can't run) and `make emu-binary`. Each one rsyncs the tree to `FW_HOST` (default `deltron.local`) and runs over ssh.
- **The fm1-emulator (QEMU) emulates the pi32v2 CPU.** KVM doesn't help here, but 12 cores are enough.
- **Targets that run on the Mac:** host unit tests, scenarios, screens and WASM.
- **Fallbacks:** a Docker recipe is documented in `tools/docker/README.md` but isn't built in v1. GitHub CI is deferred until public release.

## Merge policy
- **Worker branches** start from the `integration` tip at dispatch.
- **Merging a worker's result.** When a valid `worker_done` arrives, the coordinator:
  1. runs the ladder (`make check unit scenarios screens parity`, plus `make fw emu-binary` when target code changed);
  2. runs `/code-review`;
  3. merges `--no-ff`;
  4. releases the worker.
- **Failures** go back to the same worker as a follow-up Dispatch, with the artifacts attached. After two failures on one Task, the coordinator escalates to the owner.
- **`integration` → `main`** happens only at RC gates, as a PR that the owner approves.

## Rules every worker spec inherits
- **Read first:** `CLAUDE.md`, `docs/README.md` and the docs named in the task.
- **Tests first.** Start with a failing unit test or scenario. Done means the listed `make` targets pass. List every new or changed golden PNG in your report with what it should show. Workers (DeepSeek Flash) cannot view images, so the Opus reviewer checks each PNG at 1:1.
- **Keep X0X's layout.** Put new code in new files (under `core/` for new modules). Don't move or rename inherited files, so upstream fixes can still be ported.
- **Hardware and licensing limits:**
  - Never flash.
  - Never write flash `0x0000–0x3FFF`, `0xE8000`, `0xFF000` or BTIF.
  - Never link JieLi SDK/RTOS libraries.
- **Code limits:**
  - Use C99.
  - No heap, no `double`, no 64-bit division.
  - Prefer integers in new code.
  - Never *require* core 1. Budgets assume a single core.
- **Determinism scope.** Host and WASM output must be bit-identical. Device behaviour is checked against the event stream, not audio hashes.
- **Stay inside your ownership paths.** Ask the coordinator (via `ask`) for anything outside them.
- **Update** `interaction-model.md` and `screens.md` when behaviour changes.
- **Finish with one `worker_done`** carrying `--outcome`, `--files-modified` and `--report-path reports/<task>.md`.

## Gates and tasks

### G0 pre-launch (owner)
- [x] Confirm X0X as the base ([ADR-0001](../decisions/0001-base-platform.md)). Confirmed 2026-10-10.
- [x] Approve committing `docs/`, `design/` and `CLAUDE.md` to `main`. The owner allows commits at any point.
- [x] Build host: deltron.local is reachable over SSH as `caleb`, and x86-64 is confirmed. Docker, passwordless sudo, the JieLi package server and gitee are all reachable from it.
- [x] Agent mix: workers are **DeepSeek 4.1 Flash via opencode** (`--agent opencode`, model `opencode-go/deepseek-v4.1-flash`, pinned in the repo's `opencode.json`). Reviewers are **Claude Opus 5.5** subagents. The coordinator is Claude and acts as project manager, keeping its own context small.

### G1 design sign-off (owner, before T1 so it overlaps with build setup)
- [x] (Approved by the owner 2026-10-10; later changes come back as follow-up tasks.) Review the [canvas](https://claude.ai/artifact/VrmkPN8wop1rqGqmc2o8PL) and [interaction model v2](../product/interaction-model.md). Sign off, or list changes.

| ID | Task | Ownership | Acceptance |
|---|---|---|---|
| **T1** | **Import X0X and build remotely.** Add `x0x` = charlesvestal/fm1-x0x `6cffa40` with full history, and create `integration` from it with `main` merged on top. Add the `felucca` remote for diffs. Add `FW_HOST` remote targets for `make fw` and `make emu-binary`. Strip the BREAK generator and its tools. Add `LICENSES/` attribution. | whole repo | X0X tests and WASM parity pass unchanged on the Mac. `make fw` on deltron produces a `.fwsc` byte-identical to X0X's release recipe output (or the differences are explained). `make emu-binary` boots it and captures the home screen. No BREAK symbols remain. |

### RC0 (owner flashes)
- [ ] Flash T1's build: an unmodified X0X without BREAK.
- [ ] Prove the installer and return-to-stock paths work.
- [ ] **Read S 12 / M 15 / L 28 px text on the panel at arm's length** and set the minimum text size.
- [ ] Note the PERF screen's CPU load.

| ID | Task | Ownership | Acceptance |
|---|---|---|---|
| **T2** | **Harness.** Add JSON state dumps and new `expect` keys for transport and LEDs to the host runner. Add `wait-bar`. Support `GOLDEN_UPDATE=1`. Add audio metrics. Add a screen catalogue with goldens and Felucca's layout lint (minimum size taken from RC0). Port Felucca's AA `gfx.c` with Inter Tight and the GREY tokens from `design/project/fm1.css`. Add a forbidden-symbol and flash-region check to `make check`. | `host/`, `tests/`, gfx files, `tools/` | Existing scenarios pass. Every X0X screen renders in the new font with zero lint findings, viewed at 1:1. `make check` fails on a planted violation. |
| **T3** | **Grammar and LIVE shell.** Decide which X0X views survive, then relabel them to the [one grammar](../product/interaction-model.md#one-grammar-everywhere). Add the STUDIO/LIVE mode switch (long-press PLAY in and out), the SCL layer toggle, header and footer, and the zone key map with LED states. These are wired to X0X's existing per-part patterns so the result is playable at once. LIVE is a new view file; `ui.c` is not split. | new `ui_live*` files, minimal `ui.c` hooks | Goldens for LIVE 1–3 and 5. Scenarios: enter and exit LIVE; SCL flips layers; a zone key cues the part's pattern on the bar. A view list is in the report. |
| **T4** | **Clip engine (M1).** Role table (DRUMS, BASS, LEAD, CHORDS, FX). A per-role clip pool, packed like FM1 Move's. Global transport. Per-role launch queue with Bar, Beat and End-of-clip quantize. Loop and Once modes. Re-selecting cancels. Takes over from X0X's `ppat` and `pcue` behind the same LIVE view. | new `core/seq/clip*`, `core/model/*`, adapters in `seq/` | Event-list unit goldens for each quantize × mode. Scenario: BASS B queued at beat 3 starts on the next bar. LIVE goldens are unchanged. |
| **T5** | **Engines.** Port FM6 (`fm6_core.c` Apache-2.0, `eng_fm6.c`, `fm6_vbank.c`, voice allocation) and NOISE from Felucca behind X0X's command queue. Default sounds: 909 for DRUMS, 303 for BASS, FM6 for LEAD and CHORDS, NOISE for FX. Also port Felucca's **loader pre-erase CRC** (step 2b) with its loader tests. | new `dsp/eng_fm6*`, `dsp/eng_noise*`, loader files | Audio-hash goldens per preset (host). A single-core budget test with 5 roles. `ldr_test` covers a corrupt package being rejected before erase. |

### RC1 (owner flashes)
- [ ] Measure CPU headroom on the device with all 5 roles playing FM6 chords.
- [ ] Re-check the loader with a deliberately corrupt package. It must be refused, and the device must stay bootable.
- [ ] Record both results in `open-questions.md`.

| ID | Task | Ownership | Acceptance |
|---|---|---|---|
| **T6** | **Storage and songs core.** N song slots. A/B CRC records with the header written last. Autosave when stopped and silent. The dirty flag. A versioned format (no migration until v2 exists). A load fuzzer and a power-cut test. Undo stack core: clear, copy, record pass, take, delete song. This **freezes the model and the format** for the parallel tasks. | new `core/model/song*`, `store*`, `undo*` | Fuzz, power-cut and round-trip tests. `make emu-binary` shows a song surviving a reboot. The frozen model header is attached to the report. |
| **T7** | **STUDIO clip UI.** Song home grid and cursor. The empty-slot flow: REC records from the next bar and closes the loop on the second press; SEQ enters steps. Editor pages NOTES or DRUM, MOTION and SETTINGS, cycled by SEQ. Copy by hold and tap. Clear with HOME and hold, plus its ring. Undo toast. | new `ui_studio*` | Goldens for STUDIO 1–5, 9, 10, 14 and 15. Scenarios: record a 2-bar loop; step-enter 4 notes; copy LEAD A → CHORDS C; clear; undo; redo. |
| **T8** | **Songs UI and CRUD.** Songs list, New song (Empty, Starter, Copy), song menu, rename (port of Felucca `ui_name.c`), delete with hold-to-confirm and undo, sound browser. Runs in parallel with T7, and uses only T6's frozen API. | new `ui_songs*`, `ui_name*` | Goldens for STUDIO 6–8 and 11–13. Scenarios: create, rename, copy, delete, undo, then reboot and check the state persists. |

### RC2 (owner flashes) — first usable release
- [ ] Smoke checklist: make a song from scratch, perform it in LIVE, power-cycle mid-autosave, and return to stock.

| ID | Task | Ownership | Acceptance |
|---|---|---|---|
| **T9** | **Take recorder and playback.** A session event log stamped with the sample counter. It taps the engine command queue plus launches, mutes and FX. REC arms and stops. Hold REC clears. One take per song, and undo restores the previous one. Playback from LIVE SETUP. | new `core/seq/take*` | **Event-exact** replay on host and device (emu-binary). Audio-hash-exact on host. Goldens for LIVE 4 and 8. |
| **T10** | **Punch FX and LIVE setup.** Six FX: repeat 1/8, repeat 1/16, tape stop, filter dive, echo throw and freeze. Tap latches, hold is momentary. Upper keys mute roles. HOME+FX resets. LIVE SETUP rows, including assign keys (hold a key plus PRESETS). | new `dsp/perform*`, `ui_live_fx*`, `ui_live_setup*` | Goldens for LIVE 6, 7 and 9. Scenarios: tape stop restores the audio; an assigned key launches its new target. |
| **T11** | **MIDI and polish.** TRS and USB note input on the live channel. Program Change launches clips (quantized). Clock in. Cheat sheet (HOME hold) and help cards (GLO + button). | `midi*`, new `ui_sys*` | MIDI-script scenarios for notes, PC launch and clock follow. Goldens for SYSTEM 1–2. |

### RC3 (owner flashes) — public release candidate
- [ ] The full smoke checklist again, plus external keyboard tests.
- [ ] Enable GitHub CI, and add the Docker fallback recipe.

## Deferred past v1
These are listed so workers don't build them:
- **Not ported:** USB UAC 48 kHz and the descriptor-order fixes.
- **Pad maps and launch:** extra pad maps (4×4, Split, Custom), scenes, 2/4/8-bar quantize, legato, the Gate mode, OCT clip banks.
- **Takes:** a takes list, keeping a take as the arrangement, MIDI export.
- **Capture and FX:** HOME+SAVE capture, punch FX beyond the six.
- **Songs:** the recently-deleted song slot (undo covers delete until power-off), format migration tests (no v2 format exists yet).
- **Hardware and infrastructure:** clock out, BLE, GitHub CI and the Docker fallback.

## Coordinator loop
The commands below are illustrative. Before launch, run `orca skills get orchestration --reference references/coordinator-loop.md` to get the exact flags.

```text
orca status --json
orca orchestration run-create --objective "fm1-rocket-os v1 per docs/engineering/build-plan.md" --json
orca orchestration task-create ...        # one per Task: row + inherited rules + linked docs; deps as drawn above
orca orchestration worker-start --task <id> --worktree new-child --name <t-slug> --agent claude --setup run --json
orca orchestration check --wait --types "worker_done,escalation,question" --timeout-ms 900000 --json
```

- **Every gate pauses the Run.** The coordinator reports to the owner and waits for an explicit go.
- **The coordinator never flashes,** and never merges anything that hasn't been reviewed.

## Overnight run (2026-10-10)
Owner decisions for unattended work:
- **Unattended flashing (owner, 2026-10-10).** The FM-1 is on the Mac's USB. The coordinator may install a build with `tools/fm1_install.py` **only if its loader is byte-identical to X0X 1.0.5's** and it has passed the full ladder. Builds with the T5 loader change wait for the owner. Recovery: SAFE MODE over USB, then `tools/fm1_rescue.sh`. Screen, sound and feel checks go on the owner's morning checklist. With this, the run may continue past RC1.
- **RC0 is deferred.** T2 uses Felucca's 12 px as a provisional minimum text size. The RC0 and RC1 `.fwsc` files are left in `build/rc/` for the owner to flash. The run stops at RC1 unless the device is attached (see above), because RC1's CPU numbers gate the UI-heavy tasks after it.
- **The repo is public** on GitHub (cmlarsen/fm1-rocket-os, GPL-3.0). The coordinator merges reviewed work into `integration` and fast-forwards `main` at the end of each task; there is no owner-approved PR step tonight.
- **Review:** every worker result gets an Opus 5.5 review subagent plus the test ladder before merge.

## Lanes (v3, 2026-10-11)
Overnight the owner asked for more parallelism. Each task with a self-contained core is split into a **core** part (new files and unit tests, nothing shared) and an **integration** part (the wiring into `ui.c` and the sequencer).
- **Core lanes run in parallel:** clip engine (T4c), FM6 + NOISE (T5c), storage + undo (T6c), take recorder/player (T9c), punch FX DSP (T10c), MIDI-in parser (T11c), name entry port (T8c).
- **The integration lane runs serially:** T2 → T3 (LIVE on the clip engine; absorbs T4i) → T5i → T6i → T7 → T8 → T9i → T10i → T11i. Each step wires cores that are already tested.
- **The unlock** is `core/model/model.h` (Song, Role, Clip, Take types and limits), written and frozen by the coordinator right after T1. Changing it later needs the coordinator.
- **T5L (owner-gated):** porting Felucca's loader pre-erase CRC is split out of T5. It changes the loader, so its build can't pass `tools/check_loader.sh`, and only the owner may flash it.
- **At most 4 workers run at once:** the integration lane plus 3 core lanes. Core lanes go in critical-path order: T4c, T5c, T6c, then T9c, T10c, T11c, T8c.
- **Worker fallback on usage limits (owner, 2026-10-11).** 1) DeepSeek 4.1 Flash via opencode. 2) If DeepSeek hits a limit: Codex Sol (`--agent codex`, its default `gpt-6.1-sol`). 3) If Codex also runs out: check DeepSeek first, then Codex; only if both are still limited, use a Claude worker. Before every later Claude worker, check DeepSeek and Codex again, and go back to them as soon as either works. A limit shows up as the worker stalling or reporting a quota/429 error; probe with a one-line `opencode run` / `codex exec` before relaunching. Retry the same Task with `--retry-of`.
- **Reviews scale with risk.** Sequencer, storage and loader changes get a full Opus review. Deletions, docs and standalone modules get a quick pass.

## Follow-ups found in review (not yet scheduled)
- **LIVE (from T3):**
  - A slide gets a note-off just before its note-on, so the 303 glide isn't legato (`engine.c` `live_apply`).
  - `s_bon` applies X0X's old probability lanes, so a LIVE bass note can be skipped by chance. LIVE should ignore probability.
  - `all_off_now` sends no MIDI note-offs or clock stop to external gear.
  - K1–K4 changes made in LIVE are saved into the project.
  - The screen tour (`screens_tour.x0x`) doesn't visit LIVE yet, so `screens-lint` doesn't cover it.
- **Device install log:** the RC1 release build `build/rc/rocket-rc1-1662c01.fwsc` (songs on flash, FM6 on core 1) was installed unattended on 2026-10-11 and stayed up for 40 s through its first song autosave. Before that, the T3 build (`build/rc/rocket-t3-*.fwsc`) was installed unattended on 2026-10-11, and the FM-1 stayed up for 30 s, answering `FM-1_900 [running]`.

## Riskiest tasks
| Task | Risk | When we'll know |
|---|---|---|
| T1 | The JieLi toolchain's prebuilt binaries on Ubuntu 26.04 | Immediately |
| T5 | CPU headroom on one core | Measured at RC1, before any UI work depends on it |
| T9 | Event-exact replay | Its scope has already been narrowed so it can be reached |
