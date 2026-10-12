# FM-1 custom firmware playbook

A handoff for the next agent building custom firmware for the M-VAVE FM-1, on any base (X0X, Felucca, stock-derived or another fork). It distils fm1-rocket-os: research to installed RC in about 25 hours of wall clock (2026-10-10 to 2026-10-12), 370 commits, 55 merges, about 22 unattended device installs and zero bricked units. The sources are the session transcripts, `docs/`, `reports/` and the git history of this repo.

Read sections 1 and 2 before planning anything. The rest is reference.

## 1. The rules that would have saved the most time

1. **Fork, never move.** Pick a base by reading its code, then add product code in new files. Moving or splitting inherited files (a planned `core/` move and a `ui.c` split) fights every upstream merge and was cut at plan review.
2. **Flash the unmodified base on day one.** Installing stock X0X 1.0.5 before any of our code existed proved the USB path, the identity string and rescue in minutes.
3. **Pin the loader before any unattended flash.** The device accepts any loader that carries the `FELUCCA-LOADER-1` tag (`ota.c:271-276`), so the host must refuse every package whose loader sha256 differs from the approved one.
4. **Budget RAM and stack in every task spec.** RAM was the binding limit all run: two branches that each fit overflowed the pool when merged, and New song needed 28.8 KB of stack on a 24.3 KB stack. Measure the real compiler's frames from the disassembly on every build.
5. **Build firmware on the merged tree after every merge.** Worker ladders passed; merged trees broke the pool, the stack gate, goldens and links.
6. **Treat heartbeats as proof of nothing.** The two biggest losses (about 4 h and about 3 h) were dead workers whose heartbeats kept arriving. Confirm a worker starts by watching its files change, and watch for stalls.
7. **Write the invariants before the first UI or ISR task.** No hung notes, interrupts never wait, the ISR never reads main-loop data or pushes to main-loop rings, flash writes only while stopped and idle. Hung notes were rediscovered through four separate paths before the invariant existed.
8. **Design ISR/main-loop handoffs before coding them.** The take recorder took five review rounds (about 11.5 h, the critical path) because the first design had one staging slot per role.
9. **An independent reviewer with a shell, writing failing repros, on every risky task.** Every feature branch had real bugs that the worker's own ladder missed.
10. **Mutation-test every checker.** The fuzzer caught 0 of 4 planted hung-note bugs until an all-off call before its silence check was removed. Then it found a real hang in the installed build.
11. **Lock the product vocabulary and keep internal codes out of owner-facing text.** The owner asked "What is TPOL?" twice and "What is classic mode? ... I feel like the names keep changing." A coordinator-invented mode ("Classic") was built overnight and then removed at the owner's request.
12. **Run the pre-flight checklist (section 2) before the owner goes to bed.** SSH host keys, the public-repo creation and Python packages each needed the owner.

## 2. Day-zero checklist

Run all of these while the owner is still around:

- [ ] Build host: `ssh -o BatchMode=yes <host> true` works (host key accepted), and the JieLi toolchain is installed there (`tools/get_toolchain.sh`). It runs on Linux x86-64 only. A remote host (`FW_HOST=deltron.local`, rsync, one remote directory per checkout) worked better than Docker on an arm64 Mac.
- [ ] The clean fork reproduces the upstream release byte for byte. X0X 1.0.5 rebuilt to sha256 `080b1907…`, which proves the toolchain and the SDK packaging inputs.
- [ ] The git remote exists. The auto-mode classifier refused `gh repo create --public`, so the owner ran it.
- [ ] Python venv with `mido` and `python-rtmidi`; `tools/fm1_install.py --info` sees the device.
- [ ] Stock base installed and identity recorded (X0X reports `FM-1_9010005` for a release and `FM-1_900` for a dev build; the MIDI port is named "Felucca" on every Felucca-family base).
- [ ] Rescue path tested or at least documented for this base (section 4).
- [ ] Mac awake: `pmset`, AC power, lid open.
- [ ] Each worker provider answers a one-line probe with the exact model you will use (`opencode run -m opencode-go/deepseek-v4-flash "Reply ok"`, `codex exec "Reply ok"`), and none is near its usage window limit.
- [ ] The owner has stated the unattended-flash rule, the provider fallback order and where the run must stop.
- [ ] Glossary of product terms in `docs/README.md`, agreed with the owner.

## 3. Choosing a base

Every open FM-1 firmware descends from Felucca's HAL and loader, so platform knowledge transfers across bases. Compare candidates at code level before writing architecture docs; the fork choice changed twice in the first hour here and the owner had to correct the plan ("we shouldn't plan to write the base from the ground up").

| Base | Gives you | Watch out for |
|---|---|---|
| **Felucca** (hugelton, GPL-3.0-only, v1.5.1 `a4d7ca2`) | HAL, loader with pre-erase CRC, bootguard, 13 engines including FM6 (msfa, Apache-2.0, integer), WASM emulator, ~60–70 host tests, `ui_render.c` screen sweep, audio-hash goldens, static instruction budget (`target_budget.py`) | One pattern per track (`track_t` embeds `step[64]`), heavy globals, no scenario runner. Releases are squashed single commits, so every port is a hand port. |
| **X0X** (charlesvestal/fm1-x0x, GPL-3.0, v1.0.5 `6cffa40`; used here) | 82-line `plat.h` boundary, per-part pattern cueing, song record, motion lanes, SPSC engine command queue (`type:4|a:8|b:8|c:12`, the natural tap for logging), host scenario runner (`host/x0x_host.c`), WASM parity test, SAFE MODE, core-1 fallback, reproducible release | 4,119-line unity-build `ui.c`, floats in seq/engine, a BREAK generator under a permission-only licence (stripped), X0X's 2 s delay buffer eats most of the pool |
| Melodee, SLOOP, FM1 Move, Dinghy, Optimist | Ideas: Melodee's per-track banks, SLOOP's in-app rescue (OCT− at power-on), FM1 Move's clip grid (never run on hardware), Dinghy's build checks | Crash-prone, churning, or untested on hardware |
| Lunar Modulator (MIT) | Best CI and verification-ladder write-up | Not FM-1 code |
| GrooveOS, fm1-polyseq | Nothing | Closed with anti-reverse-engineering terms / no licence. Never copy code, assets or text. |

Reusable on any base: FM6 (`fm6_core.c`, `eng_fm6.c`, `fm6_vbank.c`, `tools/gen_fm6_tables.py`, `gen_fm6_patches.py`), the NOISE engine, the anti-aliased `gfx.c` with `tools/gen_aa_font.py` and `aa_raster.py` (Inter Tight, OFL), `ui_name.c` name entry, `tools/fm1_install.py`, `fm1pkg_make.py`, `lz4blk.py`, and the A/B CRC storage pattern. T5c ported FM6 from Felucca bit-identical in under 30 minutes. Keep Apache and OFL headers and licence files. Never link JieLi's SDK or RTOS libraries.

Full method and ranking: `docs/research/fork-evaluation.md`, `docs/research/firmware-landscape.md`, `docs/decisions/0001-base-platform.md`.

## 4. Platform facts

Sources: `docs/research/fm1-hardware.md`, `firmware/app.ld`, `docs/engineering/architecture.md` (RAM plan), `docs/reviews/2026-10-11-rc1-device-results.md`, and the reports cited. Numbers marked *X0X* come from the X0X linker script and build; re-read them for another base.

### CPU and toolchain
- JieLi AC791N/WL82, **pi32v2** (not ARM), 240 MHz from a 24 MHz crystal, about 512 KiB SRAM, 1 MiB SPI NOR (JEDEC 0x856014), code runs XIP. No SD, no SWD/JTAG.
- Single-precision FPU exists. X0X builds with `-mcpu=r3 -mfprev1 -ffp-contract=off`. `float` is fine; `double` pulls in soft-double routines.
- JieLi Clang 4.0.1, `-target pi32v2`, Linux x86-64 only. **No standard headers on the device, not even `<string.h>`.** No heap. **No 64-bit division** (`__udivdi3`, `__udivmoddi4`). Ban `__floatsidf`, `__floatunsidf`, `__truncdfsf2`, `__floatdidf` in a symbol check (`tools/check_symbols.sh`) and lint the source (`tools/check_no_64_div.py`).
- **Second core:** some units misread RAM on core 1 (x0x#10). X0X's check word covers the job hand-off, not the data. Never require core 1; budget single-core worst case, and test that output is bit-identical with core 1 on and off. A T5perf candidate silently skipped FM6 with core 1 off and was caught only by a 0.0% FM6 profile line.
- The device build is a single translation unit (`x0x.c` includes the platform files), so a `static` in a header is one instance.
- X0X's `irq_save` is a global `cli` and `irq_restore` always runs `sti`: not nest-safe. A counter was added.

### Memory (*X0X* `firmware/app.ld`)
| Region | Size | Notes |
|---|---|---|
| XIP code | `0x8DFBC` (~568 KiB) | image ended at ~554 KB |
| RAMTEXT | 24 KiB | flash driver, core-1 code; build checks it makes no calls |
| `.data+.bss` | 98,304 B | X0X alone used ~95 KB; put big state in `.pool` |
| POOL | 344,064 B | X0X's build requires ≥ 8,192 B spare |
| User stack | 24,320 B | 256 B guard bands; `fm1_guard.h` traps overflow as "X0X CRASH" |
| IRQ stack | 7,936 B | audio and TIMER5 ISRs nest on it |

Known RAM hogs: X0X's `dly_buf` (176 KB at 2 s; cut to 0.75 s), gfx `cv_px` (69 KB), the Classic pattern bank (~17 KB, removed). A `song_t` is ~14.3 KB. JieLi clang puts whole structs on the stack, so any function holding a song by value costs ~14 KB of stack; build in place or use shared scratch.

### Flash
- **Never write** `0x0000–0x3FFF`, `0xE8000` (stock VM backup), `0xFF000` (key/MAC), BTIF (`0xE9000`). Enforce it with a lint in `make check`.
- Stock: header `0x0`, isd_config `0x38D0`, app `0x4000–0x93FFF` (encrypted JLFS), VM `0x94000`, BTIF `0xE9000`, key/MAC `0xFF000`.
- Felucca/X0X: app `0x4000–0x93000`, projects `0x97000–0x9EFFF`, FM6 bank `0x9F000` (read in place), user samples `0xA0000–0xDBFFF` (240 KiB, reused here for songs and takes), presets `0xDC000`, OTA staging `0xE0000–0xE4FFF`, autosave A/B `0xE5000`, FM6 voices `0xE7000`, settings `0xFC000–0xFEFFF`.
- The X0X 1.0.5 update loader erases any sector in `0x93000–0xFC000` whose last 256 B hold a valid update record. Audit every inherited flash writer on day one: X0X's USB sample upload wrote `0xA0000`, the same region as the new song store, and would have wiped every song.
- A unit that previously ran another firmware has stale data in these regions. Validate on boot (magic, version, header CRC, payload CRC) and never erase at boot.
- Flash writes stall XIP and glitch audio. Write from RAM in short IRQ-off windows, only while stopped and idle, never during a performance mode. A/B sectors with the CRC'd header written last survive power cuts (21 cut points tested, 0 bad).
- A failed save keeps the last good copy, never retries on a timer (autosave plus a dirty flag made an endless flash loop), clears on an explicit SAVE, and offers the user a way out.

### Update, identity and rescue
- `.fwsc` over USB-MIDI SysEx, unsigned, CRC-16 only. The device picks the loader by entry type 100, last one wins. The loader runs from RAM and writes only `[0x4000, 0x93000)`; a power cut mid-update re-runs it. Upgrade SysEx `F0 22 24 35 7F F7`.
- `tools/fm1_install.py` exit codes 0–6 suit automation. After install, wait 30–40 s and check `--info` for `[running]` to rule out a boot loop. `--info` proves USB enumeration only; add a build-id query in the next project.
- Rescue, in order: X0X SAFE MODE after 2 crashed boots; hold OCT− + OCT+ 5 s for UBOOT; mask-ROM UBOOT (`WL80UBOOT1.00`, USB `4c4a:8057`, SysEx `F0 22 24 35 7D F7`), which cannot be overwritten; `tools/fm1_rescue.py`/`.sh` restores stock V15 from a Mac. The rescue path's AC791N tag and single-type-100 check were never run against a real V15 file.

### Audio, timing, I/O
- 44.1 kHz. X0X uses 512-frame I2S halves (11.6 ms) rendered in calls of at most 256 frames (`firmware/src/app/main_fm1.c:13-15`); Felucca's halves are smaller. The sequencer runs in the audio ISR.
- `core/seq/clip_engine.c` defaults `CLIP_ENGINE_FS` to 48000; the build overrides it to 44100 and a static assert ties it to `FS`. Keep a check like that for every compile-time rate.
- At 120 BPM bars start on block boundaries, so "event at offset 0" is the normal case. X0X stamps events at block end (256 frames late); stamp with `eng_prof_frames - n`.
- Measured CPU on the device: X0X baseline (909 + 2×303) ~65% of the block with core 1 on; five roles plus a 4-voice FM6 chord 76–77% before moving FM6 to core 1, 66.4% after. Host ns/sample is a poor proxy (the host over-weights FM6 about 4×). Only the device probe counts.
- X0X's overload guard engaged at 85% and released below 65%, so it stuck; release was moved to 75%.
- A full 240×240 redraw over ~12 MHz SPI takes ~77 ms. Main-loop work that feeds the ISR must tolerate 40–100 ms stalls.
- Display: ST7789V 240×240 RGB565 on SPI1. PA2 is the backlight and also gates the key matrix and the 595 LED chain. The QEMU fm1-emulator accepts only SPI1 `BAUD=4`, and X0X sets `LCD_BAUD 0`, so the emulator cannot boot X0X; that tier was never built.
- 27 keys (F3–G5) with single-colour LEDs and **no velocity**, 14 buttons (panel prints SEL/SCL; code `B_SEL`), 7 endless encoders, 1 master pot. Tap vs hold at 250 ms.
- TRS MIDI in only; USB device only (no host). X0X's MIDI ring overflows and drains at most 64 packets per block, so clock ticks drop; timestamp clock in the USB ISR and publish with a seqlock. Safety commands (Stop, all-off) need reserved queue headroom or they drop in MIDI bursts.

## 5. Architecture patterns that held

- **`plat.h` is the only hardware boundary**; logic lives in pure-C `core/`, and `targets/` stay thin. This made host tests, WASM and the scenario runner possible.
- **Freeze the data model header before parallel lanes** (`core/model/model.h` plus `model.md`). Workers then asked instead of editing it.
- **One tap function pointer in the engine command drain** (NULL on the device) served both host event logs and the take recorder.
- **Invariants written into `architecture.md` as found**, each with a host trap where possible: one held-note table and release on every view or mode change; `engine_all_off` on every audio-source handoff; the ISR never reads `song->clip[]` and never calls `cq_put`; interrupts never spin or wait; staging under the critical section; no flash in performance mode; the engine never plays a clip without its data; a monophonic release silences a part only if that key's note is sounding.
- **Host race-injection hooks** compiled only under the host build. Host renders are synchronous, so ISR races are invisible without them.
- **Tests never drive product defaults.** A worker wanted an empty first-boot song because tests were easier; the host got a command to set one up instead.
- **No masking hacks.** A proposal to force-retire stuck voices on a timer was refused; the real cause was a preset that rings for ~8 s.

## 6. The verification ladder

Fastest first. Times are from `reports/review/tests.md`.

| Tier | Target | Catches | Time |
|---|---|---|---|
| Static | `make check` (64-bit div lint, remote `fw`, `check_symbols.sh`, flash-region lint, `check_help.py`, short fuzz) | soft-double, 64-bit div, heap, pool spare, forbidden flash writes | 18 s |
| Device build | `make fw`, `fw-debug` on the build host; `tools/check_stack.py` runs inside `tools/build.py` | toolchain-only failures (`<string.h>`), stack overflow | ~15 s |
| Unit | `make unit`, `unit-core` (~20 suites, ASan/UBSan) | event-list goldens, storage power cuts, model round trips | 31 s |
| Scenarios | `make scenarios` (129 scripts → PNG, WAV, JSON dump, event log vs goldens) | behaviour regressions | 12 s |
| Screens | `make screens-lint` (1:1 contact sheet, layout lint) | clipping, overlap | 5 s |
| Parity | `make parity` (WASM vs native within 1 LSB) | float/libm drift | 9 s |
| UI fuzz | `make fuzz-ui` (200 seeds × 2,000 events, ddmin shrinker writes a repro script) | hung notes, crashes | 282 s |
| Loader gate | `tools/check_loader.sh <pkg>` | any loader other than the approved one | <1 s |
| Device probe | `tools/rc_probe.py --stress N` on a debug build | CPU mean/peak, xruns, guard, core-1 share | 60 s/run |

Lessons about the ladder:
- **Audit the harness.** The end-of-run review found parity had silently diverged (WASM played removed gestures into silence), `tests/golden.py` passed when an expected WAV was missing, `GOLDEN_UPDATE=0` still overwrote C goldens, and a fresh checkout failed on missing generated headers. A stale golden also survived a merge.
- **Static stack analysis is mandatory on this chip.** `tools/check_stack.py` reads prologue frames from the disassembly, walks direct and tail calls, fails on recursion, and has a reviewed allowlist for indirect calls (`call rN`) that fails when a listed target disappears. The old objdump splits FPU instructions at `3f e5`/`3f f5`. Indirect calls added in one lane failed the gate after merging another.
- Packaging must refuse to write a release `.fwsc` when any gate fails; `fw` printed an image even when the stack gate exited 2. `build/x0x.fwsc` is overwritten by whichever of `fw`/`fw-debug` ran last, so always rebuild release before installing, or give the debug build its own name.
- Event-log goldens beat audio-hash goldens; parity tolerance is a number (1 LSB, observed max 3.05e-5), not "byte for byte".
- Never-built tiers (QEMU `emu-binary`, browser `e2e`) are still in some docs. Delete aspirational tiers from the plan or build them.
- Workers on non-vision models (DeepSeek Flash) can't read PNGs; reviewers check goldens.

## 7. Device measurement without hands

`firmware/src/app/rc_debug.{c,h}` adds debug-only (`X0X_DEBUG`) SysEx commands for stress and stats (`F0 7D 46 4C cmd … F7`), built on X0X's RAM peek; `tools/rc_probe.py` drives them with retries (LIVE drops some replies). This turned the RC1 hardware gate into an overnight measurement: it failed RC1 (77% mean, guard stuck), and verified the fix 45 minutes later (66.4%, 0 xruns). Pass criteria (`docs/engineering/rc1-probe.md`): mean < 70%, peak < 85%, 0 xruns over 60 s, guard never engaged, and the same with core 1 forced off. A self-timed `core1_pct` field is needed because core-0 load hides core-1 work.

The device "still up after 40 s" check never pressed New song, which would have crashed. Add a scripted button walk through every view to the post-install check.

## 8. Planning

- Get a fresh-agent review of the plan before launching (`docs/reviews/2026-10-11-build-plan-review.md`). It cut plan v1 from 17 tasks to 11, replaced Docker with the remote host, moved flashing to day one and found three gesture collisions.
- Split work into parallel **core lanes** (new files plus unit tests) and one serial **integration lane** (the files everyone touches: `ui*.c`, the sequencer, `engine.c`). Nine core lanes finished in about an hour in parallel; integration took five hours serially. Four concurrent workers was the most the coordinator could review and merge.
- Each lane appending targets to one Makefile line caused add/add conflicts on almost every merge. Use per-lane `mk/<lane>.mk` fragments with `-include mk/*.mk`, or glob `unit-*`.
- Have workers merge `integration` into their branch before review.
- Give every spec a RAM and stack budget row, and size checks to risk: the owner asked "are we being too nit picky with over testing and over validating such that it's slowing things down?", and cutting low-value checks (QEMU stretch, planted-malloc proof runs, contrast lint, audio sha goldens) saved about an hour without losing the protective gates.
- Owner gates used here: G0 (plan), G1 (design sign-off, moved before T1), RC0–RC3. Loader-changing builds always wait for the owner.

## 9. Orchestration

### Roles and providers
- Cheap workers for implementation (opencode with `opencode-go/deepseek-v4-flash`), Opus for reviews, the coordinator as PM keeping its own context small. Fix rounds go back to the same worker and worktree.
- The owner's fallback order: "if for some reason, you hit a usage limit with deepseek, switch over to codex Sol, if that runs out, check if deepseek is back, otherwise switch to claude. but check if the others usage are back before spawning other claude models."
- Failure modes: DeepSeek runs out of funds and shows "Insufficient account funds" only on its own screen; codex hits its 5-hour window mid-task (twice in one night; check "5h N% left" before dispatching a big task); opencode can wedge in its title generator ("We need title only…") after finishing, without sending `worker_done`.
- A new provider can continue committed work in the same worktree (`worker-start --worktree path:<wt> --agent claude`) with nothing lost.

### Liveness
- After every `worker-start`, run `tools/orca/start_check.py <dispatch> <worktree>`: pass on the first worktree file change, fail on provider-error regexes. Exclude files the setup script writes; it gave one false pass.
- `tools/orca/wait.sh` is the single wait loop: it strips `_keepalive` records (they broke the JSON parser once, right when a `worker_done` arrived), auto-acks heartbeats, takes an ACK argument, and runs `health.py` every 10 minutes (agent error on screen, dead terminal, no file change for 40 minutes, or a clean report with commits and no `worker_done`). Monitors must tolerate partial records; a crashing monitor is another silent failure.
- Start the wait loop only through the harness's background-task mechanism. Twice it was started with `& disown` and its output went to `/dev/null`. Guard it with a lockfile; duplicates piled up.
- Heartbeat-only wake-ups cost a full coordinator turn each (about 16 in one overnight stretch). Filter them in the script.
- Workers sharing a machine must never use broad `pkill`; one lane's cleanup killed another's ladder.
- Coordinator cost: about 374M Opus tokens by mid-run, 96% cache reads, mostly one long coordinator session. Smaller coordinator turns and fewer wake-ups are the lever.

### Reviews
- Use a reviewer agent type that has a shell. The `code-reviewer` type has no Bash and could not run `git diff` or `make`.
- Ask questions specific to the change: does it fit RAM, are notes held across the loop close, stale data from a previous firmware, torn reads in the ISR double buffer. End every review with **"Safe to install: yes/no"**, which maps onto the flashing gate.
- Coordinator decisions go in **bold** in `reports/<ID>-review.md`, which the worker reads. Every fix needs a test that fails before and passes after; copy the reviewer's repros into `tests/scenarios/` and re-run them yourself before merging.
- Re-review with the same reviewer (SendMessage keeps its context), re-running its own repros rather than the worker's new tests.
- Repros must drive the real code or host runner. Two reviewer repros re-implemented logic and gave false failures.
- Know when to stop: after the structural fix, run the reviewer's repros yourself instead of another full review.
- For a whole-codebase review: five parallel codex lanes (engine, model, audio, UI, tests) found 64 findings in about 9 minutes; Opus verifiers confirmed 18 of 21 highs and refuted 2. Codex overstates severity, so nothing goes on a fix list unverified.

### Merges and installs
1. `git merge --no-ff` the lane into `integration`, one lane at a time.
2. Full ladder on the merged tree: `make unit unit-core scenarios screens-lint fuzz-ui check parity fw fw-debug`, checking each exit code.
3. `make fw` again (release last), copy to `build/rc/<project>-<name>-<sha>.fwsc`, `tools/check_loader.sh` on it.
4. `fm1_install.py PKG --yes` under a timeout, wait 30–40 s, `--info` shows `[running]` (poll 3× at 10 s).
5. Fast-forward `main`, push, `worker-release`, remove the worktree after `merge-base --is-ancestor` and rescuing untracked reports. Clean up at merge time; the owner once found 26 finished children in the sidebar.

## 10. Working with the owner

- Use plain names in messages. Task codes (TPOL, TFIXA) and internal names (Classic, SCL) confused the owner; explain with an example first.
- Product-visible concepts the coordinator invents go into `docs/open-questions.md`, not into code. The overnight "Classic patterns" mode cost 1–2 hours to build and remove.
- When a request is ambiguous ("how to change sequences together"), ask with a concrete example before building or writing. Here the answer was "It's fine how it is."
- The owner prefers simplification over keeping legacy features ("Reduce the delay. But also remove classic mode. That is confusing.") and expects idiomatic defaults rather than open questions ("Use the ideomatic option for the live toggle.").
- Batch decisions into one AskUserQuestion before the owner sleeps. Disclose every product choice the coordinator made ("tell me if you'd rather…") and record answers in the right doc.
- The owner checks status and doesn't trust heartbeats; both multi-hour stalls surfaced only when he asked. Proactive health checks are the fix.
- The owner tracks cost and wall-clock time, and asked what the bottleneck was (it was agent speed, not the build host).

## 11. Where time went

| Loss | Cost | Cause | Prevention |
|---|---|---|---|
| Take recorder review rounds | ~11.5 h critical path, ~2.5 h of patches before the structural fix | one staging slot per role; later, slots without event indices | design note for ISR handoffs (slot depth, indexing, RAM) before coding |
| opencode title-loop wedge | ~4 h | finished worker never sent `worker_done`; coordinator noted it and waited | stall watchdog in `health.py` |
| DeepSeek out of funds | ~3 h | error only on the worker's screen; heartbeats continued | `start_check.py` after every dispatch |
| Hung notes, four paths | ~1 h | no general invariant or fuzzer before UI tasks | invariant plus mutation-tested fuzzer first |
| Classic mode built and removed | 1–2 h | coordinator-invented concept | open-questions, glossary |
| Over-engineered plan v1, fork churn | ~1 h | refactor tasks, Docker, no code-level fork comparison | rules 1–2, fresh plan review |
| RAM found mid-task (punch FX needed 49 KB, 7.8 KB free) | one fix round | no budget in spec | RAM ledger |
| Post-merge breakage (pool, stack gate, goldens, links) | 5–15 min each, many times | lanes checked alone | merged-tree ladder |
| Wait-loop faults and heartbeat turns | ~30 min plus context | parser, duplicates, detached launches | `tools/orca/wait.sh` |
| Makefile add/add conflicts | minutes per merge | shared umbrella target | per-lane fragments |

## 12. Starter kit

Copy these into the next project, and fix the noted gaps first:

| File | Purpose | Fix before reuse |
|---|---|---|
| `Makefile` (`FW_HOST`, `push`/`pull`/`fw`/`fw-debug`/`release`/`check`), `tools/build.py`, `tools/get_toolchain.sh` | remote JieLi build | per-lane `mk/*.mk`; separate debug output name |
| `tools/check_loader.sh`, `tools/loader_check.py`, `tools/x0x_loader.sha256`, `tests/check_loader_test.py` | approved-loader gate with decoy, duplicate, bad-CRC and truncated tests | pin the new base's loader hash |
| `tools/fm1_install.py`, `tests/install_test.py`, `tools/fm1_rescue.py`/`.sh` | installer with loader pin and stock-only `--force`, rescue | add a build-id query |
| `tools/check_stack.py`, `tools/check_symbols.sh`, `tools/check_no_64_div.py`, `tools/target_obj_check.sh` | pi32v2 stack, symbol and 64-bit lints | stack bounds come from the new linker script; rebuild the callback allowlist |
| `firmware/src/app/rc_debug.{c,h}`, `tools/rc_probe.py`, `tools/fm1_debug.py`, `docs/engineering/rc1-probe.md` | device stress and stats probe | port to the base's debug SysEx |
| `host/x0x_host.c` commands, `tests/golden.py`, `tests/parity.sh`, `tests/fuzz/` (`run_fuzz.sh`, `shrink.py`), `tests/lint/check_layout.py` | host runner, goldens, parity, fuzzer, layout lint | make a missing golden fail; ship planted-bug mutants |
| `tools/readme_shots.py`, `tools/check_help.py` | README images rendered from scenarios; every gesture has help | generate footer hints from the input binding table too |
| `tools/orca/{start_check.py,health.py,wait.sh}` | worker liveness | lockfile, setup-file exclusion, usage-limit and funds probes, silent heartbeats |
| `tools/gen_aa_font.py`, `tools/aa_raster.py`, `design/project/fm1.css` and parts | fonts and the 240×240 design canvas | none |
| `docs/engineering/build-plan.md`, `docs/engineering/tasks/T4c.md`, `reports/T3-review.md`, `reports/TFIXS.md`, `docs/decisions/0004-*.md`, `docs/open-questions.md` | plan, task spec, review, report, ADR and questions templates | remove never-built tiers |

### Task spec skeleton (`docs/engineering/tasks/<ID>.md`)
```
# <ID>: <title>
Context: one line, link to the plan.
Read first: CLAUDE.md, <docs>, core/model/model.h (frozen).
Ownership: exact paths; new files only where possible.
Budget: RAM <n> B pool, stack <n> B, CPU <n>%.
Do: numbered steps, each with "ask the coordinator if …".
Tests (write first): named cases; ASan.
Done: make <ladder>; tools/check_loader.sh on both packages; pool ≥ 8,192 B;
      reports/<ID>.md; worker_done --outcome --files-modified --report-path.
Rules: commit as you go; never flash; never pkill broadly; rerun a step killed by SIGTERM.
```

### Review skeleton (`reports/<ID>-review.md`)
```
# <ID> review (Opus): <merge / merge after fixes / NOT safe to install>
Safe to install: yes/no
Ladder run: <command and results>; RAM/stack table
ISR checklist: reads main-loop data? pushes to main-loop rings? waits? mask nest-safe?
               critical-section cost vs block? flash outside stopped and idle?
Findings: numbered, file:line, repro path, BLOCKER / SHOULD-FIX / NIT
Coordinator decisions: in bold
Every fix needs a test that fails before and passes after.
```

### Report skeleton (`reports/<ID>.md`)
Branch and base; what changed; findings → test table; device sizes (pool, `.data+.bss`, deepest stack chain); ladder results with exit codes; what did not work; commit list.
