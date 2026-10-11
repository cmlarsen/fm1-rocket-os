# T2b review (Opus): merge after these fixes

All 12 screens are crisp and match the palette. RAM is unchanged and the image grew by 7.6 KB. Coordinator decisions are in **bold**.

1. **Footer hints that now name the wrong gesture:**
   - `ui.c:3520` "BLK: MUTE" on the drum pages. The mute is HOME + black key (`ui.c:2057`).
   - `ui.c:3524` "LFO: TIE". A tie is LFO + OCT (`ui.c:2250`).
   **Hints must name the full gesture.** Shorten some other part of the line, abbreviate (for example `HOME+BLK MUTE`, `LFO+OCT TIE`), or drop the least important hint on that line. Keep the 12 px minimum. The other footer edits are accepted.
2. **Body text colour:** text renders `#ffffff`, but the token is `TOK_TEXT` `#cccccc` (`ui_tokens.h:19`), which is never used. Map X0X's normal text colour to `TOK_TEXT`. Keep the brightest white for selected or emphasised text (the `theme`/`accent` tokens).
3. **s06 SEQ:** "DIRECTION" touches the column divider at x≈180. Give it at least 2 px clearance, and extend the lint to flag text within 1 px of a drawn rule or rect edge, if that's simple. If it isn't, just fix the screen.
4. **`Makefile:98-99`:** `screens-lint` must `mkdir -p build/screens/tour` (or wherever shots go), and it must **fail** if any shot fails to write.
5. **`tools/check_symbols.sh:44`:** add `__floatsidf __floatunsidf __truncdfsf2 __extendsfdf2 __floatdidf __fixdfsi __udivmoddi4 __divmoddi4`, and every `__*df3` and `__*df2` helper, using a pattern.
6. **The default theme change to GREY (`project.c` palette 1→0) is approved by the coordinator.** It matches the owner-approved design. Mention it in the report.
7. **Merge `integration` into your branch** to resolve the `.PHONY` conflict in the Makefile, keeping every `unit-*` target and `unit-core`. Then re-seed the PNG goldens with `GOLDEN_UPDATE=1 make scenarios`. Only PNG files may change: check with `git diff --stat tests/golden`. Then run `make scenarios unit unit-core screens-lint check fw` and `tools/check_loader.sh build/x0x.fwsc`.
8. Update `docs/product/interaction-model.md` and `docs/product/screens.md` only where they quote X0X footer strings or the GREEN default.

Commit, update reports/T2b.md (list each re-seeded PNG), and send worker_done.
