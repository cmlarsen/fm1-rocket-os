# TPOL: gesture polish and the SEL rename

Gesture polish on the merged tree (T7b, T9i, TCLS, TLSET): HOME + a knob clears the motion lane
everywhere, HOME + tap launches now in STUDIO, the layer toggle is SEL everywhere, the LIVE footer
descender is fixed and linted, a replayed live note is event-exact again, the help cards and the
cheat sheet match the handlers, and `docs/README.md` has a glossary. Each behaviour change starts
from a failing scenario; the ladder runs on the committed tree. Nothing was flashed.

## Scope, and what pins it

### 1. HOME + a knob clears the motion lane everywhere

`ui.c`'s sound-page knobs used the X0X **SAVE + knob** combo for `motion_forget`; they use
**HOME + knob** now, so the gesture matches the model and T7b's clip MOTION page
(`ui_clip.c`'s `clip_motion_knob`, unchanged). TCLS left no X0X lane that can still be allocated
(`seq.playing` is always 0, so `motion_touch` never creates one), so the sound-page path answers
"NO MOTION ON …"; the live lane is the clip's.

- `tests/scenarios/motion_knob_home.x0x` (new) — on the 909 page, SAVE + K1 no longer says
  "NO MOTION ON …" and HOME + K1 does, and HOME + K1 does not go back. It fails on the old combo
  (the SAVE case reports a message) and passes here.
- `tests/scenarios/studio_motion_clear.x0x` (new) — the clip MOTION page: one recorded lane, then
  HOME + K1 clears it (`mlany7.0` 1 → 0).

### 2. HOME + tap launches now in STUDIO

`ui_studio.c` records the key on HOME + key down; a release before the 1.5 s clear completes
launches the clip **now** (`live_bridge_launch_now`, the clip engine's next step, as LIVE does),
and a completed hold still clears. A plain tap keeps the clip's own quantize. A refused clear
(stage lock) still launches on the tap. The pending tap is dropped on entering STUDIO and on the
one hop back from the clip settings page that does not run `studio_enter`, so a key held across a
view change cannot launch late.

- `tests/scenarios/studio_home_tap.x0x` (new) — a plain tap queues at the bar (`liveclip0` -1,
  `liveque0` 0); HOME + tap plays it (`liveclip0` 0, `liveque0` -1). It fails on the old tree
  (the tap did nothing).
- `tests/scenarios/studio_clear.x0x` — its "releasing early cancels" step now also launches, so
  the scenario pins that (`liveque0` 0) and cancels the queue again with a plain tap.

### 3. SCL → SEL

The layer toggle is one panel button, labelled SEL and X0X's `B_SEL`: it toggles the LIVE layers
and confirms in menus and overlays. Renamed in the UI/help strings, code comments, the docs
(CLAUDE, interaction-model, screens, ux-principles, MANUAL + the generated manual.html, build-plan,
open questions, fm1-hardware), the design files, `tools/check_help.py` and the scenario comments.
`interaction-model.md`'s button table no longer calls SEL the SELECT encoder's push (the encoder
row is SELECT, ALGORITHM, PRESETS, K1–K4). `snd_hang_scl.x0x` is renamed to `snd_hang_sel.x0x`
(its golden directory and its output names too); nothing else referred to it.

`grep -rw SCL` outside the historical records (reports/, docs/reviews/, docs/plans/, earlier task
specs) finds **only** `web/test_web.mjs:68`, where `scale.label === "SCL"` is the inherited scale
parameter's label, not the button; it was checked and left. `plat.h` and `fm1-hardware.md` had no
I²C-clock use of SCL, so their button-name uses were renamed too.

### 4. Help cards and the cheat sheet

`tools/check_help.py` passes (14 gesture-table buttons, all have cards in STUDIO, LIVE and SONGS).
Checked against the final handlers, including TLSET's LIVE SETUP:

- STUDIO's FX card and the STUDIO cheat-sheet row no longer claim a HOME+FX reset: HOME+FX reset
  is LIVE's (`ui_live.c`), and STUDIO's FX button only opens the inherited FX page.
- The LIVE EDIT card now names TLSET's steps: EDIT opens setup, SELECT picks a row, ALGORITHM
  changes its value, SEL opens Assign keys or the Take.
- The STUDIO HOME card and the sheet already read HOME + tap: launch now / HOME + hold: clear clip
  / K1–K4: clear knob lane; items 1 and 2 make those true.

### 5. LIVE footer descender

TCLS had uppercased the LIVE footer to dodge the clip. The footer is the design's lowercase again
(`SEL play`, `hold PLAY exit`, `keys play <role>`, `SEL pads`, `tap latch hold moment`,
`HOME+FX reset`), and `FOOT_Y` moves up one row (71 → 70) so a lowercase descender reaches row 82,
not past the 82 px knob strip. The tour's LIVE PADS/PLAY/FX shots exercise the lowercase footer, so
`make screens-lint` catches a regression: with `FOOT_Y` back at 71 it reports
`canvas 240x82: box 4,71..48,83 'SEL play'` and exits 1. `make screens-lint` is 0 findings here.

### 6. Live-note take offset

The take records a live note at the block-start sample the ISR applies it on, but the T2a host tap
logs a drained command at the block's **end** (T9i kept that so T2a's goldens would not move), so a
recorded note and its replay sat one 256-sample block apart (`rec 384` vs `replay 128`). The replay
sink (`live_bridge.c`'s `take_sink_ev`) now reports a stored command at the block's end too; the
note is applied at the block's first frame either way, so the audio is unchanged.

- `tests/scenarios/tlset_take_note.x0x` now logs the replay and runs `eventscmp`; it fails without
  the fix (line 2: `384 bon 0 63` vs `128 bon 0 63`) and passes here. `t9i_take_replay`,
  `t9i_take_reboot`, `tlset_take` and `t9i_take_full` still pass.

### 7. Glossary

`docs/README.md` gains a **Glossary**: STUDIO, LIVE, layer (PADS / PLAY / FX), role, clip, song,
take, launch, quantize, stage lock, punch FX and motion lane, one plain line each.

## Goldens (GOLDEN_UPDATE=1, viewed at 1:1)

- **Every screen's footer moves up one row** (the `FOOT_Y` fix): 68 PNGs across `harness`,
  `help_cards`, `live_*`, `snd_hang`, `songs_*`, `studio_*`, `t9i_take_screens`, `tlset_assign`
  and `tlset_rows`. The first differing pixel is at y=228 in every one.
- **The LIVE footer is lowercase again**: `live_autosave`, `live_chordfx`, `live_fx`, `live_hold`,
  `live_leds`, `live_pads`, `live_play`, `live_press`, `studio_live`, `t9i_take_screens` (their
  footer text changes as well as the row; already in the count above).
- **The STUDIO sheet drops its FX row**: `help_cards/help_sheet_studio.png` and
  `help_sheet_undo.png`.
- **The LIVE EDIT card**: `tlset_rows/tlset_setup_help.png`.
- **studio_clear**: `studio_clear_ring.png` (the ring's arc lands on a different frame phase after
  the scenario's added steps) and `studio_clear.json` (the absolute `transport.sample` moves).
- **New**: `tests/golden/motion_knob_home/`, `studio_motion_clear/`, `studio_home_tap/`, and
  `tests/golden/tlset_take_note/play_notes.txt`.

## Ladder (committed tree)

| Step | Result |
|---|---|
| `make unit` | pass |
| `make unit-core` | pass |
| `make scenarios` | pass (all scenarios, goldens compared) |
| `make screens-lint` | pass: 0 clipped, 0 overlap, 0 tiny (1585 frames, 3224 canvases) |
| `make fuzz-ui` | pass: 200 seeds × 2000 events |
| `make check` | pass (forbidden symbols, flash guard, no-sample-writers, `check_help.py`, short fuzz) |
| `make fw` | pass: image 549,880 B |
| `make fw-debug` | pass: image 554,424 B |
| `tools/check_loader.sh build/x0x.fwsc` | ok: the loader is X0X 1.0.5's |
| `tools/check_loader.sh build/debug/x0x.fwsc` | ok: the loader is X0X 1.0.5's |

## Pool numbers

| Build | Image | RAM .data+.bss | Pool used / total | Pool spare |
|---|---|---|---|---|
| release | 549,880 B | 72,896 / 98,304 | 301,940 / 344,064 | **42,124 B** |
| debug | 554,424 B | 73,524 / 98,304 | 301,940 / 344,064 | **42,124 B** |

Both clear the 8,192 B rule. No hardware was flashed.

## Notes

- **The sound-page motion lane is vestigial.** TCLS removed X0X's sequencer, so `seq.playing` is
  always 0 and `motion_touch` can never allocate an X0X lane; the sound-page HOME + knob therefore
  answers "NO MOTION ON …". The live lane is the clip's (T7b), and item 1's scenario pins the
  gesture on both. If a sound-page lane is ever wanted again, `motion_touch` needs a transport.
- **HOME + tap vs HOME + hold.** A release before the 1.5 s clear completes is a tap and launches;
  a completed hold clears. This is the model's "HOME + tap launches now / HOME + hold clears"
  applied to the one HOME + pad gesture. `studio_clear` is updated to say so.
