# T10i review (Opus): pass with small fixes. Safe to install.

Coordinator decisions are in **bold**.

1. **Echo-flag race** (`perform.c:460-467`). In `perform_echo_targets`, clear `echo_changed` *before* reading `echo_on` and the targets, with a barrier, so a toggle in the ISR between the read and the clear isn't lost. Add a test that toggles between the two.
2. **`clip_engine_mute_clear` while playing** (`ui_live.c:76`, `:90`). Make clearing a ring command, or drain the pending mute commands first, so a queued `CLIP_CMD_MUTE` can't re-apply after the clear. Keep the behaviour documented in the header.
3. **Mutes aren't persisted.** **Live mutes are performance state, not song state. Don't write `song.role[].muted` from the live mute path. On song load, clear the mutes.** Test: mute, leave LIVE stopped, autosave, reboot, and nothing is muted.
4. **Visual** (`tests/golden/live_fx/live_fx.png` against `design/project/Live-FX.dc.html`):
   - put the knob strip above the FX grid, as in the design;
   - active cells get solid fills: latched uses `TOK_THEME` with ink text, held uses `TOK_ACCENT` with ink text, and inactive uses `surf`, so latched and held differ by colour as well as by word;
   - don't let the "FX" toast cover the header while FX is held; show the layer in the header chip as the design does.
   Re-seed and list the PNG.
5. **Not now:** don't resolve the T7b merge. I'll do it. Note in the report that `CLIP_CMD_MUTE` must get its own enum value when merged with T7b's `CLIP_CMD_RELOAD`.

Then run `make unit-core scenarios screens-lint parity check fw fw-debug` and `tools/check_loader.sh` on both packages, commit, update reports/T10i.md, and send worker_done.
