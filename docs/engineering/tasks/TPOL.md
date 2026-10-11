# TPOL: gesture polish and the SEL rename

Read first: [interaction-model.md](../../product/interaction-model.md), [ux-principles.md](../../product/ux-principles.md), [reports/T11h.md](../../../reports/T11h.md) (the model-versus-code audit), [ADR-0004](../../decisions/0004-remove-classic-shorten-delay.md), and the TPOL entry in [build-plan.md](../build-plan.md).

## Scope
1. **HOME + touching a knob clears that knob's motion lane everywhere.** Classic is gone (ADR-0004), so there is no exception. TCLS may already have done this. Check it, and add a scenario if none pins it.
2. **HOME + tap launches now in STUDIO**, as it already does in LIVE. The launch is immediate, not quantized. Add a scenario.
3. **Rename SCL → SEL.** The layer toggle is X0X's `B_SEL`, and the panel prints SEL (owner, 2026-10-11, [open questions](../../open-questions.md)).
   - Rename it in UI strings (`ui_help.c` "SCL toggles the layers", `ui_live.c` "SCL pads", the footer), in code comments that name the button, in the docs (CLAUDE.md, interaction-model.md, screens.md, ux-principles.md, MANUAL.md, `docs/manual.html`, build-plan.md), in the design files (`design/project/*.dc.html`, `canvas.json`), and in `tools/check_help.py`.
   - Fix interaction-model.md's button table: it lists SCL as a button and says "SEL is the push on the SELECT encoder". One button, labelled SEL, toggles the layers in LIVE and confirms in menus and overlays.
   - Rename a scenario file (`snd_hang_scl.x0x`) only if nothing else refers to it.
   - Don't touch historical records (`reports/`, `docs/reviews/`, `docs/plans/`, earlier task specs).
   - Don't touch anything where SCL means an I²C clock pin (`plat.h`, `docs/research/fm1-hardware.md`). Check every hit before you change it.
4. **Help cards.** Re-run `tools/check_help.py`, and check every card and the cheat sheet against the final handlers, including TLSET's LIVE SETUP.
5. **LIVE footer descender.** T11h and TCLS noted that lowercase descenders clip the footer. Fix it, and make screens-lint catch it.
6. **Live-note take offset.** Live notes in a take replay 256 samples late, in both the PLAY layer and on assigned keys (`reports/TLSET.md` r1). Find the cause, fix it, and turn on `eventscmp` in `tlset_take_note.x0x`.
7. **Glossary.** Add a short glossary to `docs/README.md` with these terms: STUDIO, LIVE, layer (PADS / PLAY / FX), role, clip, song, take, launch, quantize, stage lock, punch FX, motion lane. One line each, in plain words.

## Rules
- Write a failing scenario or unit test first for every behaviour change.
- Regenerate goldens only with `GOLDEN_UPDATE=1`. View the changed PNGs at 1:1 on 240×240 and list what changed in the commit.
- Keep X0X's layout. Never flash hardware.

## Done when
- `make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` pass on the committed tree, and `tools/check_loader.sh` passes on both packages.
- Pool spare is at least 8,192 B on both builds.
- `grep -rw SCL` outside the excluded paths finds nothing.
- `reports/TPOL.md` lists each item, its test, and the pool numbers.
