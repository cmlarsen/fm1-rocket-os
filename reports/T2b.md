# T2b: anti-aliased fonts, screen lint, `make check`

Task: [docs/engineering/tasks/T2b.md](../docs/engineering/tasks/T2b.md) (part of T2). Branch `cmlarsen/t2b-lane`, from `integration` (`574efa7`). T2a (harness) runs in parallel in another worktree; this work stays inside T2b's ownership paths (`firmware/src/gfx*`, fonts, `ui_tokens.h`, `tests/scenarios/screens_*.x0x`, `tests/lint/`, `tools/check_*.sh`, `tools/gen_aa_font.py`, `assets/fonts/`, `LICENSES/`, the `check` target) plus the minimal font/token wiring the task names.

## What was done

### 1. Felucca's AA text ported to X0X
- `tools/gen_aa_font.py` + `tools/aa_raster.py` are ported from Felucca (`felucca/main`). They rasterise a TTF at 16x and box-filter to 4-bit alpha, trimmed to the ink box, with horizontal sub-pixel phases (S/M 4, L 2), kerning and the capitals' band. The `x0x` preset is exactly Felucca's sizes: **S 12/400, M 15/500, L 28/600**.
- `assets/fonts/InterTight[wght].ttf` (SIL OFL 1.1, `LICENSES/OFL-InterTight.txt`) is the source. `firmware/src/ui_fonts.h` (generated, checked in) carries `AF_S` / `AF_M` / `AF_L` and `AF_*_CAP_Y` / `AF_*_CAP_B`. Reproduce with `python3 tools/gen_aa_font.py firmware/src/ui_fonts.h --preset x0x`.
- `firmware/src/gfx.c` is rewritten on Felucca's renderer (`aag_t`/`aafont_t`, `text_run` with phase selection and kerning, per-pixel coverage blended over what is already on the canvas). X0X's API names are kept — `cv_begin`, `cv_band`, `cv_blit`, `cv_blit_from`, `cv_pset`, `cv_rect`, `cv_line`, `cv_text`, `text_w`, `draw_text_box`, `palette_set`, `C_*`, `RGB` — as are the strip-canvas bands and the blit-only-on-change behaviour. No heap, no `double`.
- The host build compiles the same `gfx.c`; `ui.c` only changed its font names and colour tokens (`T/XS/S -> AF_S`, `B/M -> AF_M`, `L -> AF_L`; `caps()` now reads `AF_*_CAP_*`). `x0x.c`, `main_fm1.c` and `panel.c` (one-shots, boot/crash/safe-mode/calibration) were switched the same way.
- `tools/gen_font.py` (X0X's old bitmap generator, now unused by `gfx.c`) drops the OpenType `tnum` feature when libraqm is missing, so the existing host/device font step still runs on a machine without Raqqm (the macOS host). No change was needed to `tools/build.py` or `host/build_host.sh`.

### 2. Tokens
`firmware/src/ui_tokens.h` holds the GREY palette from `design/project/fm1.css` as RGB565: `TOK_BG SURF TEXT THEME ACCENT MID DIM LINE SEL RAISE KEY INK REC` and the five roles `TOK_DRUM BASS LEAD CHORD FX`. X0X's five-step palette and `C_BLACK`/`C_WHITE` are mapped onto them:

| X0X | token | css |
|---|---|---|
| `C_BLACK` | `TOK_BG` | `--bg #0e0e0e` |
| `C_WHITE` | `TOK_ACCENT` | `--accent #ffffff` |
| `C_LINE` | `TOK_LINE` | `--line #303030` |
| `C_DIM` | `TOK_DIM` | `--dim #5e5e5e` |
| `C_GRAY` | `TOK_MID` | `--mid #939393` |
| `C_AMB` | `TOK_THEME` | `--theme #e8e8e8` |
| `C_HI` | `TOK_TEXT` | `--text #cccccc` |

`PART_COL` uses the role tokens: 909=`TOK_DRUM`, 808=`TOK_FX`, 303A=`TOK_BASS`, 303B=`TOK_LEAD`. GREY is now the default theme (`project.c`, `THEME` list, `SEQ_P` default).

**Not mapped (listed per the task):** `TOK_SURF`, `TOK_SEL`, `TOK_RAISE`, `TOK_KEY`, `TOK_INK`, `TOK_REC` are defined but the inherited X0X UI has no cards, keycaps or REC chip to point them at yet (T7/T8/T10/T11 build those). The four non-GREY themes keep their old hard-coded two brightest steps; only the first three "neutrals" come from tokens.

### 3. Screen tour
`tests/scenarios/screens_tour.x0x` visits every view and overlay and takes a shot. **Every PNG below is 240x240, from `build/screens/tour/`:**

| PNG | shows |
|---|---|
| `s01_home.png` | **HOME** — the pattern grid and the knob strip |
| `s02_part.png` | **PART** (`EDIT`) — the part's drum grid |
| `s03_gen.png` | **GEN** (`ARP`, on 303A) — the TB-3PO generator |
| `s04_fx.png` | **FX** — reverb/delay sends |
| `s05_mix.png` | **MIX** — channel strips, COMP/PUMP, AUDIO LOAD |
| `s06_seq.png` | **SEQ** — the 303's step/line view |
| `s07_song.png` | **SONG** (`HOME` toggled) — the song's bars |
| `s08_global.png` | **GLOBAL** (`GLO`) with its list open |
| `s09_ask.png` | **O_ASK** — the "FACTORY RESET?" question |
| `s10_list.png` | **O_LIST** — the page list ("BD: THIS TRACK") |
| `s11_perf.png` | **PERF** — the CPU/load table |
| `s12_help.png` | the **help card** (SAVE held) |

### 4. Layout lint
A host build with `-DX0X_LINT` records every text draw (string, face size, ink box, colour) and every opaque rect to a log; `tests/lint/check_layout.py` groups by frame and reports text clipped by its region, two text boxes overlapping (an opaque rect drawn over a text hides it, e.g. a big readout over a list), and faces under 12 px. `make screens-lint` builds the lint host, runs the tour and the checker, and exits non-zero on any finding.

### 5. `make check`
Keeps T1's build checks (`make fw`) and adds:
- `tools/check_symbols.sh` — reads `build/x0x.elf` (nm) or `build/x0x.dis` and fails on `malloc calloc realloc free`, `__divdi3 __udivdi3 __moddi3 __umoddi3` and soft-double helpers. Verified it fails on a planted `malloc`/`__muldf3` and passes on the real build.
- `tests/lint/flash_guard_test.c` — a host unit test that the write path refuses `0x0000–0x3FFF`, `0xE8000`, BTIF (`0xE9000`) and `0xFF000`, and accepts the real object sectors. The guard already existed: `st_erase`/`st_prog` in `x0x.c` call `FL_STORE_OK` (`firmware/hal/fm1_flash.h`) before `fl_erase4k`/`fl_write`, and it is correct — **no guard was missing, none was added**.

## Results

- `make check` — **passes** (fw on deltron: image 464120 B, RAM .data+.bss 95048/98304, pool 328052/344064; forbidden symbols clean; flash guard ok).
- `make screens-lint` — **passes**, 0 findings (593 frames, 1215 canvases).
- `make fw` — **passes**; `tools/check_loader.sh build/x0x.fwsc` — **ok, byte-identical to X0X 1.0.5**.
- `make unit`, `make unit-core` — **pass** (all host suites, including the ASan/UBSan runs).
- `make scenarios` — **passes** after re-seeding the PNG goldens (below).
- `tests/run_tests.sh` on the Mac — every scenario passes, including `help`, `perf`, `tour` and the new `screens_tour`. The only failures are the pre-existing reference comparisons `drum909`/`drum808`/`bass303`, which need the `../schwung-*` checkouts that are absent here (same as T1).

## Screen fixes beyond the font/token rule (flagged for the reviewer/owner)

The 12 px minimum (S is Felucca's smallest face; X0X's footer used a 10 px face) made a few X0X strings overflow or touch. The task says to list non-font/token findings rather than fix them, but its Done also requires `make screens-lint` to pass, so these were fixed minimally and are called out here:

- **Footer strings** (`ui.c` `footer_text`): `SELECT: MOVE`→`SEL: MOVE` (list), `BLACK: MUTE`→`BLK: MUTE` (HOME, SONG), the drum page's `BLACK: TRACK / WHITE: STEP / BLK: MUTE` → `BLK TRACK / WHT STEP / HOME+BLK MUTE`, and the 303-record page's `KEYS WRITE NOTES / ENV+OCT: REST / LFO: TIE` → `KEY NOTES / ENV+OCT RST / LFO+OCT TIE`, plus `HOLD + KNOBS: EDIT`→`HOLD+KNOBS: EDIT`. Each hint now names the full gesture (the mute is HOME+black key, the tie is LFO+OCT). The design's footer is short keycap hints, so a follow-up could port the keycap footer instead.
- **SEQ knob label** `DIRECTION`→`DIRECT` (`ui.c` `SEQN`): its ink touched the column divider at x=180 at 12 px; `DIRECT` clears it by ~10 px.
- **MIX "AUDIO LOAD" value** moved `x 70 → 76` (its ink touched the label by 2 px).
- **PERF host placeholder** `"? (NO CYCLE COUNTER)"` → `"? (NO COUNTER)"` (host-only; the device shows "N MHz"). It overlapped the right-aligned "1 CORE".

The lint was **not** extended to flag text within 1 px of a rule/rect edge: the rect stream includes card and list-rule edges that legitimately sit near text, so the check would false-positive broadly. The screen is fixed instead, as the review allowed.

## T2b review fixes (round 2)

All items in [T2b-review.md](T2b-review.md) were addressed:

1. **Footer gestures** — drum mute and the 303 tie now name their full gestures (above); the accepted edits are kept. `expect helpfits 1` still passes.
2. **Body text colour** — the GREY palette's 5th step (`C_HI`, X0X's normal text/values) now maps to `TOK_TEXT` `#cccccc`, so the default text is no longer `#ffffff`. `TOK_THEME` `#e8e8e8` (`C_AMB`) and `TOK_ACCENT` `#ffffff` (`C_WHITE`) are the brighter emphasis/selected tokens. This matches the design roles (`design/project/Tokens.dc.html`: TEXT=body, THEME=values, ACCENT=the one hot thing).
3. **s06 SEQ** — `DIRECTION`→`DIRECT` (above).
4. **`make screens-lint`** — now `mkdir -p build/screens/tour`, fails on the host's "cannot write" message and on a shot count below the number of `shot` lines.
5. **`tools/check_symbols.sh`** — added `__floatsidf __floatunsidf __floatdidf __floatundidf __fixdfsi __fixdfdi __truncdfsf2 __extendsfdf2 __udivmoddi4 __divmoddi4` and a `^__.*(df2|df3|dfsi|dfdi|sf2|sfdf|dftf|tfdf)` pattern; verified it catches all of them planted.
6. **GREY default** — approved; `project.c` palette `1→0` and the `THEME` default `1→0` (it matches the owner-approved design; see the token table).
7. **Merge + goldens** — `origin/integration` merged (the `.PHONY` conflict resolved to keep `unit`, `unit-core` and every `unit-*` target plus `screens-lint`). The 16 PNG goldens were re-seeded with `GOLDEN_UPDATE=1 make scenarios`; **only PNG files changed** (`git diff --stat tests/golden`):
   `groove/01_boot_909.png`, `02_909_grid.png`, `03_808_grid.png`, `04_tb3po_a.png`, `05_303b_sound.png`, `07_playing.png`, `08_home.png`, `09_fx.png`, `10_mix.png`; `harness/harness_playing.png`; `patterns/11_chain.png`; `song/s01_303a_on_p2.png`, `s02_home_parts.png`, `s03_song.png`, `s04_song_playing.png`, `s05_motion.png`. Each shows the same screen as before with the new AA text on the GREY background.
8. **Docs** — neither `interaction-model.md` nor `screens.md` quotes an X0X footer string or the GREEN default (`screens.md` already says "the GREY palette"), so no change was needed there. `docs/MANUAL.md`/`manual.html` still say the THEME options are "green, amber, cyan, red or mono" (out of the review's scope) — flagged for a follow-up.

The tour now also shoots **HOME** explicitly: X0X boots into PART (`ui_init` sets `V_PART`), so the old `s01_home.png` was a duplicate of PART. `screens_tour.x0x` taps HOME before the first shot.

## Notes / follow-ups
- `firmware/src/ui_fonts.h` is a checked-in generated header (292 KB). Regenerating it needs Pillow; the generator works with or without libraqm.
- `interaction-model.md` / `screens.md` do not quote the old footer strings or the GREEN default (`screens.md` already names the GREY palette); `docs/MANUAL.md` still lists the THEME options as "green, …" and should follow.
- T2a owns `host/` and the `screens`/`unit`/`scenarios`/`parity` targets; this work adds `screens-lint` and the `check` additions only.
