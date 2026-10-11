# TCLS: remove Classic patterns, shorten the delay to 0.75 s

Owner decision: [ADR-0004](../../decisions/0004-remove-classic-shorten-delay.md). Read it first, then the Classic section of [interaction-model.md](../../product/interaction-model.md#classic-patterns) and the RAM plan in [architecture.md](../architecture.md).

## Scope
1. **Delay.** `DLY_LEN` in `firmware/src/app/engine.c` goes 44,100 → 33,075. Find every place that computes or clamps a delay time (fxbus, synced delay, GLOBAL/FX pages, songs that store a delay time) and make sure no read or write can exceed the shorter line. Add a unit test at the clamp edge (longest synced time at 60 and 80 bpm).
2. **Remove Classic from the UI.** Drop the GLOBAL → Classic patterns row and the Back to song row, the CLASSIC header state, the `GLO: BACK TO SONG` footer, the help cards that mention Classic, and every key path that reaches X0X's pattern editors or its SONG page. HOME + knob now clears the motion lane everywhere (there is no "outside Classic" exception); X0X's SAVE + knob lane clear goes away with Classic.
3. **Stop running X0X's pattern sequencer.** The clip engine is the only thing that owns the audio. Remove the hand-over paths (clip engine ↔ X0X sequencer) and their all-off handoffs that only exist for Classic, keeping `engine_all_off` on every remaining audio-source handoff. MIDI input (`midi_live.c`) must not have a Classic branch left.
4. **Free the project patterns' RAM.** X0X's project pattern bank is no longer allocated in the pool. A first boot or empty song slot builds the song from `song_templates.c`, not from the project. Do not erase or write the X0X project region on flash. Anything still needed from the project (sound/kit settings) is read once at boot or comes from the template.
5. **Keep inherited files.** Don't delete or rename X0X files; cut the code paths and the allocations. New code in new files where it's more than a few lines.
6. **Tests.** Remove the Classic scenarios and goldens (`studio_classic_exit` and the Classic parts of others); regenerate affected goldens with `GOLDEN_UPDATE=1`, view the PNGs, and list what changed in the commit. Fuzz-ui must not find any route to a pattern editor.
7. **Docs.** Remove the Classic section from interaction-model.md (point to ADR-0004), update architecture.md's RAM plan with the new numbers, screens.md, help text, MANUAL.md.

## Done when
- `make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` pass on the committed tree and `tools/check_loader.sh` passes on both packages.
- The report gives release and debug pool before/after (expect roughly +39 KB spare) and RAM `.data+.bss`.
- Never flash hardware.
