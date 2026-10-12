# Tests / host / tools review

Review date: 2026-10-11. Findings: **12 total — BUG 9, EDGE 1, SIMPLIFY 0, CLARITY 0, MAINTAIN 2; high 1, medium 10, low 1.** Review only; no product, test or documentation changes and no hardware flashing. Read CLAUDE.md, the glossary, architecture invariants and interaction model first. Scope inspected: Makefile; host runner/build/critical-section shim; host unit suites, scenario scripts, golden comparator, UI fuzz and shrinker, layout/flash lints; installer/loader checker/rescue, help/README screenshot tools, Orca health scripts; WASM parity and web harness. Inherited DSP/HAL/loader implementation was not reviewed for its own defects.

## Execution evidence

The exact requested `make unit unit-core scenarios screens-lint fuzz-ui check` was run once on this macOS checkout, with a fresh `build/` and no hardware connection. It exited 2 after **8.915 s** at `unit`: drum-pan, stereo and delay_clamp could not find `x0x_drum_tables.h`. The late host-build step then generated the files, but make stopped and did not execute subsequent requested goals. Remaining goals were therefore run individually once to provide their actual results. Timings below are elapsed wall time including compilation, not device performance numbers.

| Goal | Elapsed | Exit | Result |
|---|---:|---:|---|
| exact multi-goal command / unit | 8.915 s | 2 | Missing generated drum headers; stopped before unit-core |
| unit-core | 30.673 s | 0 | Passed, optimized + ASan/UBSan suites |
| scenarios | 12.335 s | 0 | Passed, all 112 scenarios and existing goldens |
| screens-lint | 4.487 s | 0 | Passed, current tour has zero layout findings |
| fuzz-ui | 281.729 s | 0 | Passed, 200 seeds × 2,000 events |
| check | 18.091 s | 0 | Passed, remote firmware build/symbol limits, local guards/help and 20 × 500 fuzz |

Extra checks: `make parity` failed in **9.243 s**, reporting `160256 samples, peak 0.000, max diff from the host 4.00e-1, screen true, lights true`; Python installer simulation passed in **3.389 s**; `node web/test_web.mjs` failed in **0.031 s** with ENOENT for `web/editor.html`; flash-region guard passed in **0.116 s**, and the no-sample-writers lint passed separately. Unit's inherited 909/808/303 reference comparisons were explicitly skipped because neighboring reference checkouts were absent, and update-path tests were skipped because no package existed at that point. Standalone installer tests were run anyway. After check produced the firmware package, the approved-loader checker passed in 0.041 s and its negative-package suite passed in 0.177 s (decoy loader name, duplicate type-100 entry and broken CRC). The current package therefore has the correct loader; finding 1 concerns enforcement on future installs/builds. Focused repros below were kept under `/tmp/fm1-tests-review`, never committed. The initial missing generated-header failure is not a product compilation failure: `unit-core` and host builds subsequently succeeded.

## Ranked findings

### 1. EDGE / high — installation does not enforce the approved loader

**Location:** `tools/fm1_install.py:364–375`; `Makefile:166`.

`load_package` trusts the encoded product name and the occurrence of `FELUCCA-LOADER-1` anywhere in the raw package. It does not parse the type-100 entry, validate UFW structure or enforce `tools/x0x_loader.sha256`; `--force` removes even the marker check. Release `make check` also does not run the loader checker (debug-check does). The project policy depends on installing only X0X 1.0.5's loader, whose flash-write restrictions preserve recovery; the ordinary install/check path does not establish that property.

**Scenario:** a rebuilt package contains a changed type-100 loader that preserves its marker, or a marker placed in unrelated payload -> package is served to the device without proving the approved loader or its write policy. A stock updater can validate package transport integrity without establishing this project's exact-loader policy. This is a missing safety gate, not a claim that the current loader writes protected regions.

**Repro run:** constructed a 1,200-byte non-UFW file with valid FM-1 identity markers and the loader string at byte 1,000. `load_package(path, False)` accepted it as FM-1_900; `check_loader.sh` rejected the same file with exit 2, wrong chip tag. No USB was opened. Key acceptance line: `if LOADER_MARK not in raw and not force`.

**Fix:** invoke a shared strict package/type-100 validator and approved SHA check before device discovery/write; enforce it in release check too. Separate an explicitly supported stock-rescue path from ordinary project installation if other loaders must remain supported.

### 2. BUG / medium — an expected WAV can disappear and the golden still passes

**Location:** `tests/golden.py:166–183`.

`cmp_audio` iterates WAVs actually present in OUTDIR rather than the expected filenames in audio.json. Removing a scenario's `wav` command or failing to produce audio eliminates all comparisons; audio.json itself is skipped by the ordinary file loop.

**Scenario:** audio.json expects required.wav with 44,100 frames -> OUTDIR is empty -> exit 0, “golden ok”. **Repro run:** exactly that setup returned 0. Key line: `for name in sorted(f for f in os.listdir(outdir) if f.endswith(".wav"))`.

**Fix:** compare expected and actual WAV filename sets first, fail for every missing expected file, then check metrics.

### 3. BUG / medium — parity runs different gestures in native and WASM

**Location:** `tests/parity.sh:14`; `tests/host/emu_test.mjs:24–37`.

Native now runs Starter -> PLAY -> key 0; WASM still runs GLO -> SEL -> keys 1/0/14 -> PLAY, based on the removed Classic pages. They cannot establish native/WASM parity for the same performance.

**Scenario/repro run:** `make parity` produced native audio but silent WASM, peak 0, max difference 0.400, exit 2. This is a harness divergence; it does not prove DSP divergence.

**Fix:** execute one shared scenario through both surfaces with identical event/sample boundaries. Also compare both channels and lengths: the current comparator truncates to `Math.min(host.length, out.length)`, ignores the right channel and allows `1e-4` (~3.28 normalized PCM LSB), wider than the project's 1-LSB requirement.

### 4. BUG / medium — fresh-checkout unit target fails before generating its inputs

**Location:** `tests/run_tests.sh:60–64,96`.

Drum-pan, stereo and delay tests compile against build/gen before the script's final host-build generates drum samples/tables. An empty build directory makes the advertised Mac unit target fail; a previously warmed build hides it.

**Scenario/repro run:** the exact requested command in this fresh checkout failed all three tests with missing `x0x_drum_tables.h`, then printed host-build success and UNIT TESTS FAILED.

**Fix:** make generated headers explicit prerequisites before the first consumer. Do not rely on ordering an unrelated build at the end.

### 5. BUG / medium — scenario success does not imply artifacts were written

**Location:** `host/x0x_host.c:439–448,525–531,1133–1139`.

PNG and JSON open failures only log and return; WAV open failure silently leaves wav null. They do not increase `fails` or return an error to main. Existing scenario output directories also retain old files, so a missing new artifact can be compared using stale output.

**Scenario/repro run:** `shot missing/shot.png; dump missing/state.json; wav missing/audio.wav; wait 10; wavstop` against an output directory without `missing/` printed PNG/JSON errors and exited 0. No artifacts were produced. With no golden, Makefile reports success; WAV disappearance also escapes finding 2.

**Fix:** propagate artifact open/write/close errors to runner exit status, and recreate each scenario output directory or enforce a fresh output manifest per run.

### 6. BUG / medium — fuzz minimization uses the obsolete three-second FM6 tail

**Location:** `tests/fuzz/shrink.py:28–30,47–64`; compare `host/x0x_host.c:1631–1632`.

The fuzzer correctly permits released FM6 tails beyond three seconds, while the shrinker appends `wait 3000; expect voices 0`. Its tail regex also does not strip the new `wait 22000`, `expect nfm6voices 0` or `expect fm6held 0`. Any nonzero exit is accepted as preservation of a failure, so it may minimize a real invariant failure into a normal bell release or a different assertion failure.

**Scenario:** a failing random sequence also plays GLASS BELL -> minimizer deletes the real bug while keeping a bell note -> obsolete tail alone fails -> misleading minimized reproduction.

**Repro run:** used the existing fm6_release scenario's note setup, replacing its 12-second wait with stop/3-second wait/voices=0. It returned 1 with one voice; the original scenario passed in the scenario sweep. Full ddmin was not run.

**Fix:** share the updated tail definition, delimit it explicitly in recordings, and preserve the original failure signature instead of any nonzero return.

### 7. BUG / medium — web harness cannot reach its updater tests

**Location:** `web/test_web.mjs:27,462`.

The script unconditionally reads removed editor.html before running fm1ota tests. The package section also retains the Felucca output filename instead of x0x.fwsc. No Makefile target runs this harness, which helps it remain stale.

**Scenario/repro run:** `node web/test_web.mjs` exits 1 at ENOENT web/editor.html; package and updater tests never execute.

**Fix:** remove retired editor cases or keep them in a separate optional suite, use the actual package path, and expose a web-test target that runs supported installer and emulator checks.

### 8. BUG / medium — disabling golden update with `0` still overwrites C goldens

**Location:** `tests/host/clip_engine_test.c:327`; `tests/host/take_test.c:105`.

Both branches test only whether `getenv("GOLDEN_UPDATE")` exists; Python's golden updater requires exactly `1`. A CI or developer setting of `GOLDEN_UPDATE=0` switches these tests from checking to writing baselines, contrary to CLAUDE.md's explicit opt-in rule.

**Scenario/repro run:** copied clip goldens to `/tmp`, ran clip_engine_test with GOLDEN_DIR pointing there and GOLDEN_UPDATE=0 -> exit 0 and UPDATE for every case. Repository goldens were not changed.

**Fix:** accept only string `1`, consistently across all golden writers.

### 9. BUG / medium — deleting an entire help context passes the coverage checker

**Location:** `tools/check_help.py:101–111`.

It checks button coverage only within contexts the source parser happened to find. STUDIO/LIVE/SONGS themselves are not required; an entire missing context is invisible. The regex also checks initializers, not nonempty/helpful card content.

**Scenario/repro run:** removed the SONGS block from a temporary ui_help.c copy -> `check_help --file ...` returned 0, reporting only LIVE=14 and STUDIO=14.

**Fix:** require the expected context set, then validate every button and nonempty card in each; add negative fixtures for missing contexts/cards.

### 10. MAINTAIN / medium — `check` is not the documented comprehensive verification gate

**Location:** `Makefile:166–178`; `tests/run_tests.sh:106–110`.

Release check runs firmware symbol/size checks, three lints and a short unsanitized UI fuzz. It does not depend on unit-core sanitizer suites, scenario/golden comparisons, screens-lint, parity or loader identity. `make test`/test-local run scenarios through run_tests.sh without golden.sh, whereas `make scenarios` checks goldens. Two similarly named routes therefore certify materially different properties.

**Scenario:** change a screen's pixels or introduce a sanitizer-detectable core defect -> `check` and/or test-local can remain green even though the intended verification ladder fails. This is static target inspection, not an injected product mutation.

**Fix:** define and document one aggregate gate with explicit fast/local and device-build subtargets; have test/test-local and scenarios share the same golden-aware implementation. Clarify that Mac unit is not all product-core units.

### 11. MAINTAIN / medium — Orca health mistakes read-only progress for a failed start or stall

**Location:** `tools/orca/start_check.py:15–16`; `tools/orca/health.py:30–37`.

These helpers infer worker progress from filesystem mtime and emit START-FAIL after seven minutes or STALL after forty minutes without changes, even for a living reviewer/tester whose assigned output is written only at the end. Unreadable terminal output is also labeled STALL despite not establishing worker failure. Any build-output change can conversely certify a start even if unrelated to the worker.

**Scenario:** a review-only worker reads and tests for >420 s without tracked-file edits -> START-FAIL despite active orchestration heartbeats. Static proof from conditions; the timeout was not waited out.

**Fix:** use Dispatch liveness/heartbeat state and distinguish live, unverifiable and exited; treat mtime as supplemental evidence only, never authorization to retry/stop.

### 12. BUG / low — empty layout evidence is accepted as a clean layout

**Location:** `tests/lint/check_layout.py:99–107,131`.

No minimum frame/canvas/text count is required. If gfx instrumentation stops emitting while screenshots still work, screens-lint can certify zero findings from an empty log.

**Scenario/repro run:** empty file -> `layout lint: 0 frames, 0 canvases, 0 findings`, exit 0.

**Fix:** fail closed when the expected render tour has no instrumentation; preferably associate each expected shot with a frame and recorded draw evidence.

## Product-to-test map and remaining gaps

There are **112** scenario scripts, **74** with matching golden directories. Nearly all have explicit assertions; screens_tour and t9i_take_screens intentionally rely on visuals/goldens. The suite has substantial real assertions, not merely smoke-test printouts.

| Feature / invariant | Existing evidence | What it does not establish |
|---|---|---|
| R1/R13 songs, saving, recall | song_store/song_io/undo tests; song_fuzz; t6i_*; studio_persist; songs_* | Real flash timing, hardware reset/boot path, power-cut behavior of the physical chip; host `reboot` calls boot in the same process and preserves static host state |
| R2/R3/R6–R9 roles, loops/once, quantize, switching | 38 clip_engine golden cases, all_five_roles, once_*, beat/bar/end, queue cancellation, edits while playing, ring stress and counter_wrap; live_bridge | Actual ISR preemption of main-loop clip edits/staging, device ring ordering under interrupt interleavings |
| R4/R5/R10 LIVE/layers/key assignments | live_*, tlset_assign/none/studio_keys/rows, UI fuzz, LEDs/goldens | Coverage counters proving every view/gesture/state was reached; arbitrary starting songs and corrupted assignments in the whole-app fuzzer |
| R11/R12 panel and external MIDI | midi parser/glue, held-note scenarios, overflow/stall/clock/transport, legato; sample-clock interleaving unit | Combined randomized MIDI plus UI transitions; actual simultaneous USB/TRS producer preemption and IRQ masking; full app sample-counter wrap (core wrap is covered) |
| R14 take recording/replay | take full-buffer/wrap/dense-window/parity units; t9i replay/replace/clear/full/reboot; tlset_take* | Browser persistence and reload of a song/take; event equality across device/native/WASM; physical power loss throughout take save/recommit |
| R15 creation/copy/clear/undo and stage lock | studio_* recordings/overdub/motion/copy/clear-abort/undo-play, songs unit full/empty/list/menu/delete, tlset_lock | Human ease, button bounce/scan timing, simultaneous physical chords at ISR boundaries, hardware screen legibility |
| No hung notes / source changes | Dedicated sound/studio/MIDI hang scenarios; UI fuzz mid-transition held-note checks and release tail | A view-coverage assertion; panel+MIDI combined random sequence; thread sanitizer or actual concurrent app execution |
| Flash safety / rescue | FL_STORE_OK/FL_SONG_OK boundary tests; fake NOR byte/operation cuts; static no-writer lint; strict type-100 loader checker | Macro test does not execute all real driver wrappers or certify a loader used by installation; no rescue fake-device suite is present |
| D1–D3 host/browser/UI | Deterministic scenario/event goldens; text layout lint; screen contact sheet; WASM harness | Current parity is broken; no automated real-browser AudioWorklet startup/input/persistence lifecycle test; tests use raw WASM exports, not worklet/page wiring |

Additional weaknesses worth tracking without counting speculative product bugs: coarse WAV frame/peak/RMS goldens intentionally cannot distinguish wrong notes/order/timbre with the same energy; exact event logs are needed for musical behavior. JSON goldens pin internal numeric view IDs, obsolete seq part/cue fields and exact script sample offsets, so harmless refactors can churn them; use stable semantic state for UI contracts. run_tests.sh and unit-core duplicate compilation/execution of live_bridge/song_io/crit/delay; run_tests.sh also builds the host and the later scenarios/fuzz prerequisites build it again. Generated host headers are rebuilt only when absent, so generator changes can be hidden by a warmed build; declare source dependencies. The long fuzz uses deterministic seeds 1–200 and is unsanitized: it finds behavioral hangs but cannot prove absence of memory UB.

## Checked and found clean

The core units include meaningful edge assertions: empty and maximum-size songs, full takes, undo-ring saturation, sample-counter wrap, one-frame rendering windows, quantize cancellation, mute note-offs, malformed MIDI and overflow. Store tests cut at every programmed byte for small and large saves and at every operation across saves; fake NOR enforces page bounds and one-way programming. These are strong tests and passed, including ASan/UBSan. No new defects were found in those test assertions.

Flash guard tests passed for the protected boot/stock backup/BTIF/key pages and song-region bounds; product st_erase/st_prog visibly call the guard before the inherited driver. Rescue's sector-write method restricts aligned complete sectors to [0x4000,0x93000), verifies each sector, checks the exact stock/loader hashes and chip identity at the entry path, saves a full backup before flash writes, and verifies the whole expected image afterward. No protected-region bypass was found by inspection; rescue was not run on a device. check_loader selects type 100, verifies chip/header/list structure, rejects missing/duplicate/truncated loader entries and compares the approved SHA; it correctly rejected the malformed repro. README screenshot copying fails for missing sources and uses nearest-neighbor scaling. Native scenario behavior and current screen text layout passed; no goldens were updated.
