# Verification: ui.md #1–3, #7 and tests.md #1–3 (Opus, 2026-10-11, on integration 55f82cc)

All seven are CONFIRMED by host repros, built from a `git archive` copy. The help repros use the `card` probe, because the review's `help` probe reads a stale field.

- **ui #1, a clear survives losing its modifier or view.**
  - `studio_tick()` (`ui_studio.c:224`) checks only the pad or SEL, never HOME or `ui.view`.
  - Releasing HOME mid-hold, entering LIVE mid-hold, or SEL + EDIT each erased clip 0. Undo recovers it.
  - Fix: cancel `st.clear_clip` in `set_view()` and on a mode change, and require HOME to stay held for a HOME clear.
- **ui #2, REC help clears the take.**
  - `input()` runs `live_rec_tick()` before `help_glo_press()`.
  - With the REC card up, `takehas` went 1 → 0, and a short GLO + REC armed recording.
  - Fix: skip any edge or timer whose button is in `ui.btn_used`, or while GLO is held.
- **ui #3, PLAY and FX help perform their actions.**
  - With the PLAY card up, the transport starts, and a 700 ms hold enters LIVE. With the FX card up, the FX layer opens.
  - Fix: same as #2.
- **ui #7, cancelling a SEL clear launches the clip.**
  - TPOL r1 fixed only HOME + pad. The SEL path (`ui_studio.c:192`) starts the clear without setting `btn_used`, so an early release reaches `studio_launch_cursor()`.
  - Fix: set `ui.btn_used |= 1u << B_SEL` when the SEL clear starts.
- **tests #1, the installer does not enforce the approved loader.**
  - `load_package()` (`tools/fm1_install.py:364–375`) only checks the identity regex and the `FELUCCA-LOADER-1` string, and `--force` skips even that.
  - It never calls `check_loader.sh` or reads `x0x_loader.sha256`, and release `make check` doesn't either.
  - Fix: gate `load_package` on the loader hash, keep `--force` only as the stock-rescue path, and add the check to `make check`.
- **tests #2, a missing expected WAV still passes the golden.** `cmp_audio` loops over the WAVs that exist, not the ones expected. Fix: fail for every expected file that's missing.
- **tests #3, parity runs different gestures.**
  - `parity.sh` taps PLAY and key 0. `emu_test.mjs` still drives the removed Classic pages: WASM peak 0.000, max diff 0.400.
  - The comparator checks only the left channel, truncates to the shorter length, and allows 1e-4.
  - Fix: drive both sides from one shared script, and compare both channels, equal lengths and a ≤1-LSB tolerance.

**Fix order:**
1. The loader gate.
2. Help ownership before the PLAY/REC/FX ticks.
3. The clear-state fixes (with `studio_clear_sel_abort` and `studio_clear_view_exit` scenarios).
4. The WAV presence check.
5. Parity.
