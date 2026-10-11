# Interaction model (draft v2)

This is the proposed control scheme. It is a **draft to prototype in the emulator and on hardware, not a final spec**. Anything undecided is marked **OPEN** and tracked in [open questions](../open-questions.md).

**v2 (2026-10-11)** applies the [build plan review](../reviews/2026-10-11-build-plan-review.md) and the owner's answers:
- **SCL** toggles the LIVE layers. SEL means "confirm" everywhere.
- Only a long press on PLAY leaves LIVE.
- Quick assign moved into LIVE SETUP.
- Each song keeps one take.
- One save model.
- The v1 feature set is smaller.

Related: [requirements](requirements.md) · [UX principles](ux-principles.md) · [screens](screens.md) · [hardware panel](../research/fm1-hardware.md#1-device) · [ADR-0003 sound design scope](../decisions/0003-sound-design-scope.md)

## Panel vocabulary

| Control | Count | Notes |
|---|---|---|
| Keys | 27: 16 lower, 11 upper (grouped 3-2-3-2-1), F3–G5 | Each key has a white LED with dim, breathe and mid levels. Velocity is **OPEN**. |
| Buttons | FX, SCL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC, OCT−, OCT+ | REC is red. PLAY can also light green. **SEL is the push on the SELECT encoder.** |
| Encoders | SELECT (push = SEL), ALGORITHM, PRESETS, K1–K4 | Endless, detented |
| Screen | 240×240 colour | Minimum text size is set at RC0 on hardware |

## One grammar, everywhere
- **SELECT** turns to move between rows. **SEL** (its push) confirms or opens. **ALGORITHM** moves the cursor across, or changes a value. **HOME tap** goes back. **HOME hold** is SHIFT and shows the cheat sheet.
- **Keep or relabel the inherited X0X pages** (part, mix, FX, global) so they follow this grammar. X0X uses ALGORITHM for the part and PRESETS for the pattern, and those pages adopt the meanings above. **OPEN:** which X0X views survive is decided in the first serial build task.

## What the user sees
- **Song:**
  - tempo, key and scale
  - **roles** (DRUMS, BASS, LEAD, CHORDS, NOISE/FX). Each role has a sound (a preset the song owns), its clips, a mute state and a MIDI channel
  - the **pad map**
  - **one take**
- **Clip:** notes or hits, plus knob motion. Each clip has:
  - a length;
  - a mode: **Loop** or **Once** (Gate is deferred);
  - a launch quantize: **Bar** (default), **Beat** or **End of clip**.
- **Take:** the most recent LIVE recording, stored as an event log. Recording again replaces it, and undo brings the old one back.

## Saving (one model, no hidden state)
- **The song on screen is the song.** It autosaves when the transport is stopped and no voices are sounding.
- **SAVE tap saves now.** The header dot means "not yet written".
- **There is no separate saved copy** that can disagree with what you hear. Recovery comes from **undo**, which covers clears, copies, record passes and the take.

## Modes

| Mode | Enter | Leave | Purpose |
|---|---|---|---|
| **STUDIO** (after boot) | — | — | Build the song: roles, clips (SEQ), sounds (EDIT, ENV, LFO), songs (SAVE held) |
| **LIVE** | **Long-press PLAY** (~600 ms ring) | **Long-press PLAY** again | Perform: launch, play, FX, knobs, record the take |

- **PLAY tap** always starts or stops the transport.
- **HOME never exits LIVE.** A HOME tap closes overlays. A HOME hold shows the cheat sheet.
- Playing clips carry on across the SCL layer change inside LIVE. **Leaving LIVE stops the clip engine and releases its notes**, and hands the transport back to X0X's sequencer (T3). Entering LIVE takes the transport: X0X's sequencer stops and the clip engine starts stopped.

## LIVE mode

### Two layers, toggled by SCL
| Layer | Keys do | Screen |
|---|---|---|
| **PADS** (the default) | Launch clips, laid out by the pad map | Role × clip grid with clip states |
| **PLAY** | Play the **focused role** live, in the song's scale. OCT−/OCT+ transpose. | Role name, sound, scale, and a strip showing clip status |

- The SCL LED shows the layer: off means PADS, lit means PLAY. A header badge says the same. Switching layers never changes what's playing.
- **Focused role:** whichever role launched most recently. Turn ALGORITHM to step to another role.
- **SEL does nothing in LIVE** except confirm inside overlays. **Turning SELECT does nothing in LIVE**, so a stray bump can't change anything.
- **Simultaneous launch and play** (R11):
  - An external keyboard on TRS MIDI IN plays the focused role.
  - The FM-1's own keys can't launch and play at the same moment. The owner accepted this trade-off for v1.

### Pad map: keyboard zones (default and only map in v1)
The keyboard splits at its natural gaps, the places where two lower keys meet with no upper key between them:

| Zone | Keys | Slots |
|---|---|---|
| DRUMS | F3–B3 | 7 |
| BASS | C4–E4 | 5 |
| LEAD | F4–B4 | 7 |
| CHORDS | C5–E5 | 5 |
| NOISE/FX | F5–G5 | 3 |

- Clips are lettered in pitch order (A, B, C…).
- **Tap a clip** to launch it at its quantize point. It replaces whatever that role is playing (R9).
- **Tap the playing clip** to stop it, also quantized.
- **HOME + tap** launches immediately.
- Deferred: the 4×4 grid, Split and Custom maps, and scenes.

### LIVE SETUP (EDIT in LIVE) — configuring keys (R10)
- **Rows:**
  - **Assign keys:** hold a key and turn PRESETS to choose what it triggers (a clip of any role, a role stop, a punch FX or a live note), then release. A toast confirms, for example "A4 → CHORDS B".
  - Launch default
  - Stage lock
  - External keys channel
  - Program Change launch
  - The take (play, clear)
- **These settings are saved with the song.**
- **Holding a pad on the LIVE screen itself does nothing in v1.** Pad holds mean copy in STUDIO and clear when combined with HOME.

### Recording the take (R14)
- **REC tap arms recording.** It starts at the next bar, or at your first launch if the transport is stopped. While recording, REC is solid red and the screen gets a red border.
- **What gets logged** as events: launches, stops, mutes, FX punches, K1–K4 moves, tempo, and notes played in the PLAY layer.
- **REC tap again stops the recording.** The take replaces the song's previous take, and undo brings the old one back.
- **Play it back** from LIVE SETUP → Take.
- **Clear it** by holding REC for 1.5 s while a ring reading "CLEAR TAKE" fills. Release early to cancel. Undo restores it. Clips are never touched.
- Recording never stops the music.
- Deferred: keeping a take as the song's arrangement, and exporting MIDI.

### Feedback
| Clip state | Key LED | Screen cell |
|---|---|---|
| Empty | off | hollow (dashed) |
| Loaded, stopped | dim | filled with the role tint |
| Queued to start | fast blink | blinking outline |
| Playing | pulses on the beat | role colour, with a progress bar |
| Queued to stop | slow blink | slow blink |

- **Header:** mode, layer badge, play/rec state, BPM, bar.beat, the unsaved dot and battery.
- **Footer:** the next useful moves.
- **No countdown text** (the owner removed it on 2026-10-11).

### Performance controls (both layers)
- **K1** is a bipolar filter: low-pass to the left, high-pass to the right.
- **K2** is Space, **K3** is Echo and **K4** is Drive.
- **FX held or double-tapped** turns the first six lower keys into punch FX: **repeat 1/8, repeat 1/16, tape stop, filter dive, echo throw, freeze**.
  - Tap to latch, hold for momentary.
  - Upper keys mute roles.
  - HOME+FX resets everything.
- **K1–K4 changes you didn't record revert** when you leave LIVE.

## STUDIO tasks (R15)
| Task | Gesture |
|---|---|
| New song | SAVE held → SONGS → `+ New song` → Empty, Starter or Copy current |
| Open, copy, rename or delete a song | SONGS → EDIT on the song → menu. Delete is hold-to-confirm and undoable. |
| New clip | Cursor on an empty slot. REC records from the next bar (pressing REC again closes the loop), or SEQ opens step entry. |
| Edit a clip | SEQ cycles NOTES (or the DRUM grid) → MOTION → SETTINGS |
| Copy a clip | Hold the source pad, then tap an empty `+` slot. To move a clip, copy it and then clear the original. |
| Clear a clip | HOME + hold the pad for 1.5 s. A ring names the clip. Undoable. |
| Record knob motion | Turn knobs while recording a clip. HOME + touch a knob clears that lane. |
| Undo / redo | HOME+REC / HOME+PLAY. A toast names the action. |
| Save | SAVE tap. Autosave also runs while stopped. |
| Help | GLO + any button shows that button's card |

Deferred: HOME+SAVE capture, OCT± clip banks, and the Gate clip mode.

## External MIDI (R12)
- Use **TRS MIDI IN**. The FM-1 has no USB host.
- Notes on the live channel play the focused role.
- Program Change launches clips, quantized.
- Clock comes in over USB or TRS. Clock out over USB comes after v1 if it's easy.

## Open
- Which inherited X0X views survive.
- Velocity.
- Whether drum clips share the melodic step model.
- The minimum text size, set at RC0.
