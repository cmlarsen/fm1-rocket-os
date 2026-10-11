# T2b: anti-aliased fonts, screen lint, `make check`

Part of T2 in [build-plan.md](../build-plan.md). T2a (harness) runs at the same time in another worktree; stay inside your ownership paths.

Read first: `CLAUDE.md`, `docs/engineering/architecture.md`, the worker rules in `docs/engineering/build-plan.md`, `docs/product/screens.md` (visual language), `design/project/fm1.css` (tokens), and X0X's `firmware/src/gfx.*` and how `firmware/src/app/ui.c` draws text.

**Ownership:** `firmware/src/gfx*`, font sources and generated font tables, `tools/gen_aa_font.py`, `assets/fonts/`, `LICENSES/`, a new `firmware/src/ui_tokens.h`, `tests/scenarios/screens_*.x0x`, `tests/lint/`, `tools/check_*.sh`, and the `check` target in `Makefile`. You may edit `ui.c` **only** to switch its text calls to the new fonts and tokens; no layout or behaviour changes.

## Goal
X0X renders with Felucca's anti-aliased Inter Tight text and the GREY palette, a lint catches unreadable or broken layouts, and `make check` catches forbidden code.

## Steps
1. **Port Felucca's AA text.** Source: `git show felucca/main:firmware/src/gfx.c`, `felucca/main:tools/gen_aa_font.py`, `felucca/main:assets/fonts/InterTight[wght].ttf`, `felucca/main:LICENSES/OFL-InterTight.txt`. Generate S 12/400, M 15/500, L 28/600 (Felucca's sizes). Keep X0X's existing gfx API names working (implement them on the new renderer) so `ui.c` changes stay small. Keep the strip-canvas design and blit-only-on-change behaviour. No heap, no `double`.
2. **Tokens.** `firmware/src/ui_tokens.h` with the GREY palette and derived tokens from `design/project/fm1.css` (bg, surf, text, theme, accent, mid, dim, line, sel, raise, key, rec, plus the five role colours) as RGB565. Point X0X's colours at these where the mapping is obvious; list the rest in the report.
3. **Screen tour.** `tests/scenarios/screens_tour.x0x` (more files `screens_*.x0x` if needed) visits every X0X view and overlay (`V_HOME V_PART V_GEN V_FX V_MIX V_GLO V_SONG V_PERF V_SEQ`, `O_LIST`, `O_ASK`, the help card) and takes a `shot` of each.
4. **Layout lint.** In the host build only, record every text draw (string, font size, bounding box, colour, background) per frame. A checker (`tests/lint/`) reports: text outside the 240×240 screen or clipped by its box, two text boxes overlapping, text smaller than **12 px** (provisional minimum until the owner's RC0 check), and text contrast below 3:1 against its background. `make screens-lint` runs the tour and the checker; exit non-zero on any finding. Fix findings in the X0X screens only when the fix is a font-size or colour token change; list anything else in the report instead.
5. **`make check`** (keep T1's behaviour, add):
   - forbidden symbols in `build/x0x.bin`'s link map or `nm` output: `malloc calloc realloc free`, 64-bit division helpers (`__divdi3 __udivdi3 __moddi3 __umoddi3`), soft-double helpers;
   - a host unit test that the flash write path refuses `0x0000–0x3FFF`, `0xE8000`, `0xFF000` and the BTIF region (find where X0X guards writes; add the guard if missing and say so);
   - prove it: a script that plants a `malloc` call in a temp copy and shows `make check` fails.
6. Build on deltron (`make fw`) and run `tools/check_loader.sh build`: the loader must stay byte-identical to X0X 1.0.5.

## Done
- `make check`, `make screens-lint` and `make fw` pass. List every tour PNG in the report with the view it shows; the Opus reviewer looks at them at 1:1 (you cannot view images).
- Small commits with the attribution lines from `CLAUDE.md`'s instructions.
- `reports/T2b.md`, then `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T2b.md`.
