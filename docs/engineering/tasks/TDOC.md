# TDOC: README usage guide with rendered screens

The owner wants the repo README to teach someone with a fresh FM-1 how to use fm1-rocket-os, with screen images like the ones in the design canvas.

Read first: [interaction-model.md](../../product/interaction-model.md) (the source of truth for every gesture), [screens.md](../../product/screens.md), the glossary in [docs/README.md](../../README.md), `design/project/` (to see how the design canvas presents screens), and the current `README.md`.

## Scope
1. **A usage section at the top of `README.md`**, after a short intro. Keep X0X's README below it, under a heading that says it's the upstream base's README and that its pattern and song sections don't apply to this firmware. The sections:
   - **Install, and return to stock.** Install with `tools/fm1_install.py`; the X0X web installer can put M-VAVE's firmware back. Short.
   - **The two modes.** STUDIO builds the song; LIVE performs it. A long press on PLAY switches between them.
   - **Record a clip (a sequence).** Put the cursor on an empty slot; REC records from the next bar; REC again closes the loop. Include step entry with SEQ as the alternative.
   - **Erase a clip.** HOME + hold the pad for 1.5 s while the ring fills. Releasing early keeps the clip. Undo with HOME+REC. Mention stage lock.
   - **Play clips together, and in order.** One clip plays per role, and the roles play together. There is no follow-on chain in v1: X0X's chain and arranger were removed (ADR-0004), and keeping a take as the arrangement is deferred. To play clips in order, launch them in LIVE (each waits for its quantize point) and record a take, which replays the whole sequence. Say this plainly; don't invent a chain feature.
   - **LIVE as a launchpad.** The PADS layer (the keyboard zones map: which keys launch which role's clips), tap to launch at the quantize point, tap the playing clip to stop it, HOME + tap launches now, and SEL toggles the PLAY layer for playing notes. Cover punch FX, mutes, and LIVE SETUP (EDIT): assign keys, launch default, stage lock, the MIDI rows, clock in, and the take. Cover recording and playing the take, and clearing it (hold REC).
   - **A short cheat sheet** (HOME hold) and help (GLO + a button).
2. **Rendered screens.** Every image comes from the real firmware through the host runner, never from mockups.
   - Write one scenario per workflow, `tests/scenarios/readme_<topic>.x0x`. Each one performs the steps the README describes, uses `expect` lines to prove each step works (clip used, clip playing, clip cleared, take recorded), and takes a `shot` at each step the README shows.
   - The scenarios run in `make scenarios`, so the README can't silently go stale.
   - Copy the chosen PNGs into `docs/img/readme/`, upscaled 2× with nearest-neighbour (480×480) so the pixels stay crisp. Check in a small script (`tools/readme_shots.py`) that regenerates them from `build/scenarios/`.
   - Optional: if it's cheap, draw a simple panel diagram showing where PLAY, REC, SEL, HOME, EDIT, SEQ and the keys are.
3. **Accuracy.** Every gesture in the README must match interaction-model.md and the code. If you find a mismatch, the code and interaction-model win; note it in the report, and don't change behaviour.

## Rules
- Plain, short sentences for a musician, not an engineer.
- No firmware behaviour changes, and no golden changes outside the new scenarios.
- Never flash hardware.

## Done when
- `make scenarios screens-lint check` pass on the committed tree. Run the full ladder if anything outside docs and tests changed.
- `reports/TDOC.md` lists each README section, the scenario that proves it, and any mismatches found.
