# TDOC: README usage guide with rendered screens

The owner asked for a README that teaches someone with a fresh FM-1 how to use fm1-rocket-os,
with screen images like the ones in the design canvas. The guide is now the top of `README.md`,
under `## Using fm1-rocket-os`; X0X's own README stays below it, under a heading that says its
pattern and song sections don't apply.

**Result:** six `tests/scenarios/readme_*.x0x` scenarios perform the steps the guide describes and
prove each with `expect` lines, so the guide can't silently go stale; they run in `make scenarios`.
Twenty chosen shots are copied from `build/scenarios/` into `docs/img/readme/` at 480×480
(nearest-neighbour, the 240×240 panel at 2×) by `tools/readme_shots.py`. Every gesture was checked
against [interaction-model.md](../docs/product/interaction-model.md) and the code; four mismatches
are noted below and none changes behaviour. No firmware behaviour changed, and hardware was never
flashed.

## README sections, scenarios and images

| README section | Scenario | Images (in `docs/img/readme/`) |
|---|---|---|
| Install, and return to stock | — (no firmware behaviour) | — |
| The two modes | `readme_modes.x0x` | `modes-studio-home`, `modes-live-enter`, `modes-live-pads` |
| Record a clip (a sequence) | `readme_record_clip.x0x` | `record-empty`, `record-recording`, `record-clip`, `record-notes` |
| Erase a clip | `readme_erase_clip.x0x` | `erase-ring`, `erase-undo`, `erase-locked` |
| Play clips together, and in order | `readme_play_together.x0x` | `together-roles`, `together-take` |
| LIVE as a launchpad | `readme_live.x0x` | `live-play`, `live-fx`, `live-setup`, `live-assign`, `live-take`, `live-clear-take` |
| Cheat sheet and help | `readme_help.x0x` | `help-sheet`, `help-card` |
| (the panel drawing) | — | `panel.svg` |

What each scenario proves with `expect`:

- **`readme_modes`** — boot is STUDIO (`view 10`); a ~600 ms PLAY hold opens LIVE (`mode 1`,
  `view 9`, `layer 0`) and leaves it again (`mode 0`, `view 10`); a short PLAY tap starts the
  transport (`playing 1`).
- **`readme_record_clip`** — the cursor sits on an empty slot (`clipused4 0`); REC opens the
  editor and arms a pass from the next bar (`view 13`, `editclip 4`, `recrec 1`); REC again closes
  it (`recrec 0`, `clipused4 1`, `liveclip0 4`, `drumhits4 >0`); SEQ on a different empty slot
  creates and opens a clip (`view 13`, `editclip 15`).
- **`readme_erase_clip`** — a hold released before the fill keeps the clip (`clipused0 1`); a full
  1.5 s hold clears it (`clipused0 0`, `undodepth 1`); HOME+REC restores it (`clipused0 1`); with
  stage lock on (`stagelock 1`) a clear while playing is refused (`clipused0 1`).
- **`readme_play_together`** — three roles play together (`liveclip0 0`, `liveclip1 7`,
  `liveclip2 12`); a second launch on a role replaces the first (`liveclip0 1`); a take records
  (`takehas 1`) and plays (`takeplaying 1` → `0`).
- **`readme_live`** — a pad queues at its quantize (`liveque0 0`) and lands (`liveclip0 0`);
  tapping it queues a stop (`livestop0 1`) and the role goes quiet (`liveclip0 -1`); HOME + tap
  launches now (`liveclip0 1`); SEL toggles the layer (`layer 1` → `0`); a tap latches a punch FX
  (`fxlatch0 1`), an upper key mutes a role (`fxmute0 1`), HOME+FX resets (`fxlatch0 0`); REC
  records a take (`takerec 1`, `takehas 1`, `takestate 0`); assign-keys commits (`keyclip0 5`);
  the take card plays and stops (`takeplaying 1` → `0`); holding REC clears the take
  (`takehas 0`).
- **`readme_help`** — holding HOME shows the sheet (`sheet 1` → `0`); GLO + REC shows that
  button's card (`card 12` → `0`).

## Screens

- Each `readme_*.x0x` takes its shots with the host runner's `shot` command; the source PNGs are
  240×240 in `build/scenarios/readme_*/`.
- `tools/readme_shots.py` maps the chosen shots to `docs/img/readme/<name>.png`, upscaling 2× with
  nearest-neighbour so the pixels stay crisp, and exits non-zero (naming the missing file) if a
  scenario stops taking a shot. Run `make scenarios` first, then the script.
- `docs/img/readme/panel.svg` is the optional panel diagram: a drawing (not a screen) that reuses
  the panel geometry from `docs/img/fm1-panel.svg` and highlights the six buttons the guide uses —
  SEL, EDIT, HOME, SEQ, PLAY, REC — plus the knobs and the 27 keys.

## Accuracy: mismatches found

Every gesture in the guide matches the firmware. Four places where the docs and the code differ
were found; the guide follows the **code**, and no behaviour was changed:

1. **The clear-take ring text.** [interaction-model.md](../docs/product/interaction-model.md) says
   the ring reads "CLEAR TAKE"; the firmware draws a lowercase "clear" with "This song's take",
   "<len> long" and "clips stay - music keeps playing" (`ui_live.c:916-936`). The guide says the
   ring fills and the clips stay, without quoting "CLEAR TAKE".
2. **HOME + FX "resets everything".** The interaction model says HOME+FX resets everything; the
   firmware resets the six punch FX only (`engine_perform_reset`, `live_fx_button`,
   `ui_live.c:483-487`). Mutes and knob state are not touched (mutes clear when you leave LIVE, as
   the guide says). The guide says "resets every effect", and the LIVE help card already says
   "reset all FX".
3. **A take armed while stopped.** The interaction model says recording starts "at the next bar,
   or at your first launch if the transport is stopped". The firmware starts an armed-while-stopped
   take on the next transport start — PLAY, or MIDI Start/Continue — not on a launch
   (`live_bridge.c:34`, `:1006-1023`, `:1024-1048`); a launch while stopped only queues.
4. **REC's tap/hold window.** `LIVE_REC_TAP_MS 300` is defined in `ui_live.c:12` but unused; the
   real REC tap/hold boundary is the 1500 ms clear-hold (`ui_live.c:134-139`). Not user-visible
   (the clear ring's `live_clear_holding` leaves a wide gap), so the guide just says "hold 1.5 s".

One behavioural edge case worth knowing, not a doc mismatch: an *aborted* SEL-hold clear in STUDIO
(released between the 250 ms tap threshold and the 1.5 s fill) is treated as a SEL tap on release,
so the cursor clip launches; the HOME+pad hold does not do this. The guide only describes the
completed HOME+pad clear and the HOME+REC undo.

## Verification

On the committed tree (`make scenarios` and `make screens-lint` on the Mac; `make check` uses
deltron.local for the firmware build). Logs under `build/`.

| Command | Result |
|---|---|
| `make scenarios` | pass — all scenarios, incl. the 6 new `readme_*`; every existing golden still matches (no golden changed) |
| `make screens-lint` | pass — 1585 frames, 0 clipped, 0 overlap, 0 tiny |
| `make check` | pass — fw build (pool 301,940 B of 344,064; `.data+.bss` 72,960 B of 98,304), forbidden symbols, flash guard, help cards, size, short fuzz (20 × 500) |

Hardware was never flashed.

## Files

Added: `tests/scenarios/readme_{modes,record_clip,erase_clip,play_together,live,help}.x0x`,
`tools/readme_shots.py`, `docs/img/readme/` (20 PNG at 480×480 and `panel.svg`), `reports/TDOC.md`.

Changed: `README.md` (a `## Using fm1-rocket-os` guide after the intro, and a heading over X0X's
README saying its pattern and song sections don't apply).

Not changed: any firmware or host code, the Makefile, the goldens, or the interaction model and
screens docs. The `readme_*` scenarios add no golden directory (they take shots only), so
`tests/golden/` is untouched.
