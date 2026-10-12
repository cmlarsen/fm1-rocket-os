# TFIXS — save-path fixes

Implemented on `cmlarsen/tfixs-save`, from `86a0b8f`; verified implementation tree:
`8ef96a6`. Read TFIXS, both model review reports, architecture and the model contract
before editing. No hardware was flashed; storage tests used simulated NOR only.

## Findings and regression evidence

| Finding | Change | Failing-first test and passing proof |
|---|---|---|
| 1 — New song stack overflow | Templates write directly to their destination. Create and Copy use the existing guarded song scratch, with release on success and failure. Noncurrent Rename also uses it because the 8 KB gate exposed its full-song local. The last-song delete path uses the corrected create operation. | `tests/host/template_stack_test.py` failed on Empty's 14,432 B host frame, then passed; extending it to Rename failed at 14,416 B before that fix. Device regression `Gate.test_old_device_code` uses actual pre-fix pi32v2 prologues and the New → Starter call, proving 14,388 + 14,420 = **28,808 B**. Final device frames: Create 32 B, Copy 40 B, Rename 24 B, Empty 28 B, Starter 56 B. |
| Stack gate | `tools/build.py` runs `tools/check_stack.py`'s analysis before either device package is emitted, so both `make fw` and `make fw-debug` fail on excess stack. | `tests/host/stack_gate_test.py` first failed because the gate did not exist. Its 14 tests cover split allocations, saved registers, an over-8 KB frame, aggregate overflow, recursion, tail transfers, callback maxima, product/inherited indirect policy, jump-table labels, FPU decode halves, empty input, late allocations and callback initializer auditing. Running the gate over the complete old device disassembly also exits 1: deepest bound **29,548 B**, plus five over-8 KB frames. |
| 2 — Motion prevents saving | Validator, lane creation and value setters share `core/model/motion_contract.h`: valid target/voice/index IDs and exact pot maxima. | `test_motion_persistence` in `song_io_test.c` failed before the fix, then saves/reloads all four actual motion-page parameters (`0x600d`, `0x4007`, `0x400b`, `0x4002`). It checks invalid IDs and out-of-range values as well. `motion_contract_test.c` compares every one of 65,536 encodings against the engine descriptors. |
| 6 — Shortened clips prevent saving | Length edits trim notes starting past the new end and snapshot the length plus notes for undo/redo. Add/set clamp note starts and durations to the clip length. Editor sessions reuse their existing snapshot. | `test_shorten_persistence` failed before the fix. It shortens Starter BASS A to 8, verifies exact undo restoration, redoes, saves/reboots, clamps out-of-range note add/set (including duplicate lookup after clamp), and saves/reboots again. |
| 4 — Failed flush discards dirty edits on open | A dirty, failed flush returns an error without retrying. The SONGS open wrapper refuses switching and its list displays SAVE FAILED. | `test_failed_save_no_retry` failed on both error assertions before the fix; afterwards it also proves no additional flash operations occur. `save_failure_ui_test.c` drives the real SONGS ops and UI with a failed program: the pre-fix UI opened another song; the fixed UI keeps the current slot, dirty/failed flags and edited key mapping, shows the toast, and opens only after an explicit successful save. |
| 7 — Failed take clear acknowledged | Empty-take writes propagate the storage rc. Failure keeps the request and failure flag, and adds no durable undo entry; successful retry clears the pending/failure state. | `test_failed_take_clear` failed before the fix on error, pending/failure and undo assertions. Afterwards it injects failure specifically into the empty-take commit, verifies retained pending state and unchanged undo depth, retries, reboots and confirms the take is empty. |

The model regressions produced 38 failed checks before their fixes; the flush/clear
regressions produced five before theirs. Existing store fixtures were updated from
arbitrary legacy “below 0x0800” values to real parameter IDs and matching pot ranges;
the byte/op power-cut checks and 100,000-mutation phases continue to pass.

## Device stack measurements

The gate counts 32-bit saved registers and split negative `sp +=` allocations.
Every linked function is a root, giving a conservative superset of reachable paths;
direct calls retain the caller frame, tail transfers reuse it, and retained-frame
recursion fails. Callback owners use checked-in candidate sets and charge the
deepest candidate. Unlisted product transfers fail; inherited transfers warn with
a count, per the coordinator's policy. Both final builds have **zero** such warnings.
Unexpected allocations after the prologue fail rather than receiving a zero bound.

JieLi objdump splits the `3f e5` / `3f f5` FPU instructions into two printed halves.
The parser skips the second half so decoded FPU operands do not become fake calls
or stack operations. Internal jump-table labels do not split function ownership.

| Measurement | Release | Debug |
|---|---:|---:|
| User stack (`app.ld`) | 24,320 B | 24,320 B |
| Reserved margin (coordinator-approved) | 2,048 B | 2,048 B |
| Gate budget | 22,272 B | 22,272 B |
| Deepest conservative linked chain | **2,372 B** | **2,472 B** |
| Budget left after that chain | 19,900 B | 19,800 B |
| Largest frame (`draw_main`) | 1,168 B | 1,168 B |
| Nested ALNK0 + TIMER5 chain | **1,720 B** | **1,732 B** |
| Supervisor stack / gate budget | 7,936 / 5,888 B | 7,936 / 5,888 B |

Deepest user chain in both builds:
`_start → fm1_cstart → ui_frame → input → button_tap → live_handle_button → studio_undo → song_io_undo → undo_run → undo_stack_redo → song_store_delete → sst_recommit_song → sst_load_song → sst_try_song → fb_read → plat_song_read → st_read → fl_read_ram`.

ALNK0's deepest chain runs through clip rendering and the event sink into the 808
render; TIMER5 can preempt it, so its own deepest chain (`isr_timer5 → fm1_timer5_irq
→ uac_service → sie_rd`) is added for the supervisor-stack check.

## Memory and packages

| Measurement | Release | Debug |
|---|---:|---:|
| Pool capacity | 344,064 B | 344,064 B |
| Pool used | **301,940 B** | **301,940 B** |
| Pool spare | **42,124 B** | **42,124 B** |
| Pool spare above the 8,192 B requirement | 33,932 B | 33,932 B |
| `.data + .bss` / capacity | 72,960 / 98,304 B | 73,588 / 98,304 B |
| App image | 550,864 B | 555,408 B |

The pre-fix release also used 301,940 B of pool: no additional large buffer was
introduced. Both packages retain the exact X0X 1.0.5 loader, verified by
`tools/check_loader.sh` against the type-100 entry.

Package SHA-256:

- `build/x0x.fwsc`: `041e0003f97d93f7143b4a0604b340c940b628a269ca6d8fcf25f1f6470b6ae1`
- `build/debug/x0x.fwsc`: `1c1b9b9e821638b323ea537e2ffef49d9f119238cff0e6e685e5ed65a9209933`

## Verification

On committed implementation tree `8ef96a6`:

- `make unit unit-core scenarios screens-lint check fw fw-debug`: exit 0.
- `make fuzz-ui`: exit 0 (200 seeds × 2,000 events; all passed).
- `tools/check_loader.sh build/x0x.fwsc`: exit 0.
- `tools/check_loader.sh build/debug/x0x.fwsc`: exit 0.
- Standalone stack gate against both final disassemblies: exit 0.
- Screen lint: 1,585 frames, 3,224 canvases, zero findings; scenario goldens passed unchanged.
- `make check` also passed its 20-seed × 500-event UI fuzz, forbidden symbols,
  protected flash-region guard, removed sample-writer and help-card checks.

`make unit` has its existing optional skips for missing 9W9/8W8/303 reference
checkouts and a local SDK-dependent loader unit. The required device builds and
both package loader checks ran on deltron; the product core suites and their
ASan/UBSan checks ran locally. An earlier long ladder received SIGTERM at clean
fuzz seed 193; the coordinator confirmed that was unintentional and authorized
the complete rerun, with full fuzz split out as the final step.

Findings 3, 5 and 9 remain for the next round: fallback extent protection, deferred
take restore outside LIVE, and preservation of RAM on failed durable take undo.
No work on those findings or hardware flashing was performed.
