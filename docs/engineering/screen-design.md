# Screen design workflow

This page describes how to design and iterate on the 240×240 screens so the mockups and the device can't drift apart.

Related: [UX principles](../product/ux-principles.md) · [agentic dev loop](agentic-dev.md) · [dev tooling research](../research/dev-tooling.md#display-and-ui-rendering)

## Why it matters
- Two FM-1 developers learned the hard way that mockups lie. The FM1 Quest developer said: "everything looked great in Figma and in the web simulator, but on the physical FM1 screen, a lot of stuff ended up too small". The Baud Girl v96 tiny-text sequencer was rolled back ([sentiment](../research/community-sentiment.md#4-tiny-text-and-legibility)).
- The display is a 1.54" 240×240 RGB565 IPS panel (ST7789V).

## Pipeline
1. **Explore ideas freely.** For early direction, use:
   - **The `/design` skill in Claude Code.** It is the owner's primary tool for iterating on screens. Give it the 240×240 canvas size, the palette tokens and the screen spec, so its mockups start out at device scale.
   - The `design:design-critique` and `design:design-system` skills, for reviewing designs and building the token set.
   - [MvaveFM1-Designer](https://github.com/Quixotic7/MvaveFM1-Designer) (MIT). Its JSON format is meant for LLMs to edit, and it renders 240×240 mockups and LED states in Felucca's palettes.
2. **Write the screen spec in markdown** under `docs/product/screens/<name>.md`. List which data the screen shows, which controls do what, the hint line text, and its states.
3. **Build the view in `core/ui`.** The real design artifact is the PNG that the same C code renders: 240×240, 1:1, RGB565-quantised.
4. **Add the screen to the screen catalogue** as a name plus a state fixture or setup scenario. `make screens` renders every catalogued screen in every state to individual PNGs, contact sheets, and 2× and 4× nearest-neighbour upscales.
5. **Lint the layout.** gfx hooks flag overlap, clipping, text below the minimum size, low contrast and misalignment, in Felucca's style.
6. **Review.** The agent views the contact sheet at 1:1 and checks it against the UX principles. For a human review, publish the contact sheet as an artifact or send it with SendUserFile.
7. **Accept.** Accepted renders become goldens in `tests/golden/screens/`.

## Single source of truth for look and feel
- Palette tokens, font sizes and the role colours live in one generator input (for example `tools/look.toml`). That file generates the C header and the emulator page CSS, as Lunar's `tools/palette.py` does.
- Fonts and icons are rasterised at build time from TTF/SVG (Felucca's `gen_aa_font.py` approach), so the mockup and the device use the same glyphs.
- Role colours are fixed per role type. The same colour appears on the screen grid, the hint line, and anything tied to the K1–K4 legend.

## Still to decide
- Minimum text height in pixels and the type scale. Both should be checked at arm's length on hardware once, then enforced by lint.
- Whether to support palettes, for example a high-contrast theme.
