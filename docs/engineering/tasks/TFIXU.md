# TFIXU: UI and tooling fixes from the code review

Source: [reports/review/ui.md](../../../reports/review/ui.md), [tests.md](../../../reports/review/tests.md) and the verification [reports/review/verify-ui-tests.md](../../../reports/review/verify-ui-tests.md). Read them, plus [interaction-model.md](../../product/interaction-model.md).

## Scope, in order
1. **The installer enforces the approved loader** (tests 1).
   - `tools/fm1_install.py` refuses any package whose loader doesn't match `tools/x0x_loader.sha256`, using the same logic as `tools/check_loader.sh`. `--force` stays only for the stock-rescue path, behind an explicit flag and a printed warning.
   - Add the check to `make check`.
   - Test with a tampered package. Don't run it against a device.
2. **Help ownership** (ui 2, 3). While GLO is held, or a help card is showing, PLAY, REC and FX must not act. Their tick and edge handlers skip buttons in `ui.btn_used`. Tests:
   - GLO + REC with a take must not clear or arm it.
   - GLO + PLAY must not start the transport or enter LIVE.
   - GLO + FX must not open the FX layer.
3. **Clear state** (ui 1, 7).
   - Cancel a pending clear on `set_view()` and on a mode change, and require HOME to stay held for a HOME clear.
   - Set `btn_used` for SEL when a SEL clear starts.
   - Scenarios: `studio_clear_sel_abort` and `studio_clear_view_exit`.
4. **Test harness** (tests 2, 3, 4, 5, 8).
   - A missing expected WAV fails the golden.
   - Scenario success requires its artifacts.
   - `GOLDEN_UPDATE=0` doesn't overwrite.
   - Parity drives native and WASM from one shared script and compares both channels at equal length within 1 LSB.
   - A fresh-checkout `make unit` generates its inputs first.
5. **The UI report's other medium findings, where the fix is small and testable.**
   - FX keys use physical keys.
   - HOME + FX resets mutes, or the docs say it doesn't; the TDOC report found the same mismatch.
   - Entering the FX layer releases PLAY notes.
   - The tempo gesture on the sound pages.
   - LIVE PLAY uses the song's key and scale.
   - Recordings over 4 bars.
   - Wide names on screen.
   - LIGHTS Off vs the LIVE LEDs.
   - Skip anything that needs an owner decision or a redesign, and list it in the report.
6. **Remove the unreachable Classic UI code from `ui.c`** (ui 16), but only code that is provably unreachable. Inherited files stay; only dead paths are cut.

## Rules
- Write a failing test first for each behaviour change.
- Regenerate goldens only with `GOLDEN_UPDATE=1`, view them at 1:1, and list the changes in the commit.
- Keep X0X's layout. Never flash hardware.

## Done when
- `make unit unit-core scenarios screens-lint fuzz-ui check parity fw fw-debug` pass on the committed tree, and `tools/check_loader.sh` passes on both packages.
- `reports/TFIXU.md` lists each finding, its test, and what was skipped and why.
