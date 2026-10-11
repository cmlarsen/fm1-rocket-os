# Screen inventory (design v2)

v2 (2026-10-11) applies the [plan review](../reviews/2026-10-11-build-plan-review.md): SCL is the layer toggle, there is one take per song, six punch FX, assign keys lives in LIVE SETUP, and Gate, legato, 2/4/8-bar launch and SAVE-paste are gone.

This is the review canvas for every screen: **[FM-1 Rocket OS Screens](https://claude.ai/artifact/VrmkPN8wop1rqGqmc2o8PL)**. It's private until shared. A copy of the canvas source lives in [`design/project/`](../../design/project/) and is edited through the canvas.

Each artboard shows two things:
- the 240×240 screen at 2×
- the FM-1 key bed below it, with the LED state of each key

On the hardware, the upper row has 11 keys in groups of 3-2-3-2-1 and the lower row has 16. The visual language reuses Felucca's:
- the **GREY palette** and its derived tokens
- **Inter Tight** at S 12 / M 15 / L 28 px
- a 24 px header
- **K1–K4 knob cards** at 57×44
- **keycap hint chips**

Role colours are the only new tokens. See the Tokens artboard.

Related: [interaction model](interaction-model.md) · [UX principles](ux-principles.md) · [screen design workflow](../engineering/screen-design.md)

## Screens

| Page | Artboard | Shows |
|---|---|---|
| LIVE | 1 Enter | Hold PLAY in STUDIO: a ring fills. Release to cancel. |
| LIVE | 2 Pads, stopped | Every loaded clip is dim; tap a pad to start |
| LIVE | 3 Pads, playing + queued | Playing clips pulse. BASS B is queued and blinking. SCL switches to the PLAY layer. |
| LIVE | 4 Pads, recording | Red border, REC timer, knob cards marked as recorded, LEAD B queued to stop |
| LIVE | 5 Play layer | After SCL: the keys play LEAD in C minor, with a clip-status strip |
| LIVE | 6 Punch FX | FX held: six effects on the first six lower keys. One is latched, one is held. Upper keys mute roles. |
| LIVE | 7 Assign keys | In LIVE SETUP: hold a key and turn PRESETS to pick what it triggers |
| LIVE | 8 Clear take | Hold REC: a red ring names the take. The clips stay and the music keeps playing. |
| LIVE | 9 Setup | Assign keys, launch default, stage lock, external keys, Program Change, the take |
| LIVE | 10 The take | One take per song: timeline (role lanes plus a knob curve), play, clear |
| STUDIO | 1 Song home | Song card, selected clip, role × clip grid with cursor |
| STUDIO | 2 Empty slot | Cursor on an empty slot. REC records, SEQ steps it in. |
| STUDIO | 3 Clip, notes | Piano roll, step cursor, playhead, STEP/NOTE/VEL/LEN cards |
| STUDIO | 4 Clip, drums | 8-lane grid. White keys are steps 1–16; black keys pick the lane. |
| STUDIO | 5 Clip settings | Length, mode (Loop/Once), launch on (beat, bar, end of clip), swing |
| STUDIO | 6 Sound browser | Role-filtered presets with an engine tag. EDIT opens the deep editor. |
| STUDIO | 7 Songs | New song row, the list, storage meter |
| STUDIO | 8 New song | Empty / Starter / Copy current, plus tempo and key |
| STUDIO | 9 Clear clip | HOME + hold pad: a ring with a clip summary |
| STUDIO | 10 Undo | The toast names what was undone. HOME+PLAY redoes it. |
| STUDIO | 11 Song menu | EDIT on a song opens Open, Copy, Rename and Delete |
| STUDIO | 12 Rename | Felucca's name entry (ui_name.c), reused as-is: white keys multi-tap letters, black keys are ← SPACE → DEL and mode, K3 gives a random name, OCT+ saves |
| STUDIO | 13 Delete song | Hold SEL on Delete. A ring names the song and its contents, and it can be undone until the next delete. |
| STUDIO | 14 Copy clip | Hold a clip and tap any `+` slot. Empty keys blink slowly as targets. |
| STUDIO | 15 Clip motion | The editor's page 2. SEQ cycles NOTES → MOTION → SETTINGS. Each knob lane has a curve. HOME + knob clears that lane. |
| System | 1 Shift sheet | HOME held lists every combo |
| System | 2 Help card | GLO + any button shows its card |

## Design decisions made while drawing
These are proposals and need the owner's review:

1. **Default pad map = keyboard zones.** The FM-1's natural gaps (E–F and B–C, where two lower keys meet with no upper key between them) split the 27 keys into five zones:

   | Zone | Keys | Slots |
   |---|---|---|
   | DRUMS | F3–B3 | 7 |
   | BASS | C4–E4 | 5 |
   | LEAD | F4–B4 | 7 |
   | CHORDS | C5–E5 | 5 |
   | NOISE/FX | F5–G5 | 3 |

   Clips run in pitch order (A, B, C …). This replaces the "black-key clusters plus OPEN white-key extension" idea in the interaction model.
2. **Help cards are GLO + button, not a long press.** A long press already means something on REC (clear take), PLAY (LIVE) and pads (clear), so it can't also show help.
3. ~~The footer becomes the countdown.~~ The owner removed this on 2026-10-11. Queued state now shows only as a blinking cell on screen and a fast-blinking key LED. If a countdown comes back, it should not live in the footer.
4. **K1–K4 stay as performance knobs on every LIVE screen** (FILTER bipolar, SPACE, ECHO, DRIVE), drawn as Felucca knob cards. A red dot marks a knob whose moves are being recorded.
5. **FX layer black keys mute roles.** This matches Felucca's FX layer, where the black keys mute tracks.

## Create, read, update and delete

| | Song | Clip (sequence) |
|---|---|---|
| Create | SONGS → `+ New song` → template (STUDIO 7, 8) | Empty slot → REC (play it in) or SEQ (step it in) (STUDIO 2–4) |
| Read | SONGS → SEL opens it, and the current song autosaves first | Cursor shows role, length and mode. LIVE tap plays it. |
| Update | Edit anything; autosave runs while stopped, and SAVE forces a save | SEQ pages: notes (3, 4), motion (15), settings (5). REC overdubs. |
| Copy | Song menu → Copy (11) | Hold the clip, then tap a `+` slot (14) |
| Rename | Song menu → Rename (12) | Clips have no names (see below) |
| Delete | Song menu → hold Delete (13), undoable | HOME + hold the pad (9), undoable (10) |

Decisions made in this pass:
- **Clips have no names in v1.** A role plus a letter (BASS B) can be read at a glance and needs no typing. Names can come later if songs grow past about 7 clips per role.
- **There is no separate move gesture.** To move a clip, copy it, then clear the original. Both steps can be undone.
- **SEQ cycles the clip editor's pages**: NOTES (or the DRUM grid) → MOTION → SETTINGS. The footer shows which page SEQ opens next.
- **Deleting a song can be undone until the next delete.** One deleted song is kept in a "recently deleted" slot, which needs one spare flash slot.
- **Name entry reuses Felucca's code and gestures unchanged.**

## Not yet drawn
- Sound deep-edit pages. These are inherited from the fork ([ADR-0003](../decisions/0003-sound-design-scope.md)).
- Settings/GLO, MIDI, the boot and safe-mode screens, the alternate 4×4 grid and Custom pad layouts, and the commit-tweaks prompt.
- The alternate Felucca palettes (AMBER, ICE, PAPER, HI-CON). Swapping them only needs `design/project/fm1.css`.
