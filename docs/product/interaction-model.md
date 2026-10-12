# Interaction model (draft v2)

This is the proposed control scheme. It is a **draft to prototype in the emulator and on hardware, not a final spec**. Anything undecided is marked **OPEN** and tracked in [open questions](../open-questions.md).

**v2 (2026-10-11)** applies the [build plan review](../reviews/2026-10-11-build-plan-review.md) and the owner's answers:
- **SEL** is one button: it toggles the LIVE layers and confirms everywhere.
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
| Buttons | FX, SEL, ENV, LFO, EDIT, GLO, HOME, SAVE, ARP, SEQ, PLAY, REC, OCT−, OCT+ | REC is red. PLAY can also light green. **SEL toggles the LIVE layers and confirms.** |
| Encoders | SELECT, ALGORITHM, PRESETS, K1–K4 | Endless, detented |
| Screen | 240×240 colour | Minimum text size is set at RC0 on hardware |

## One grammar, everywhere
- **SELECT** turns to move between rows. **SEL** confirms or opens. **ALGORITHM** moves the cursor across, or changes a value. **HOME tap** goes back. **HOME hold** is SHIFT and shows the cheat sheet.
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
- **SAVE tap saves now.** The header dot means "not yet written". If a save fails, SONGS keeps the unsaved song open and shows SAVE FAILED when you try to switch songs. **SAVE retries**, and a new edit also re-enables saving; the same rule covers a failed take write (a clear or a finished recording), which stays deferred for the next explicit SAVE or take edit rather than rewriting flash on a timer. If the flash keeps failing you are not trapped: on the refused switch the toast reads **"SAVE FAILED: SAVE RETRIES, HOLD TO DISCARD"**, and **holding SEL for 1.5 s** (the delete ring) discards the unsaved song and take edits and opens the other song. That discard **cannot be undone** — the ring text says so.
- **There is no separate saved copy** that can disagree with what you hear. Recovery comes from **undo**, which covers clears, copies, record passes and the take.
- **The song persists in slot 0 on the flash** (0xA0000–0xDBFFF, beside the project). Its record is written header-last, so a power cut during an autosave leaves the version before it. At boot the stored song *is* the current song; with an empty store (a first boot) the current song is the **Starter** template from `song_templates.c` (TCLS/ADR-0004), so a new device plays something at once. LIVE performs the current song — it is not rebuilt on every entry — and changes that belong to the song (T10 adds assign-keys, K1–K4 and the launch default) mark it dirty.

## Modes

| Mode | Enter | Leave | Purpose |
|---|---|---|---|
| **STUDIO** (after boot) | — | — | Build the song: roles, clips (SEQ), sounds (EDIT, ENV, LFO), songs (SAVE held) |
| **LIVE** | **Long-press PLAY** (~600 ms ring) | **Long-press PLAY** again | Perform: launch, play, FX, knobs, record the take |

- **PLAY tap** always starts or stops the transport. **A short press acts on release** (released before the ~600 ms long-press threshold); holding past the threshold enters or leaves LIVE and starts or stops neither transport.
- **HOME never exits LIVE.** A HOME tap closes overlays. A HOME hold shows the cheat sheet.
- Playing clips carry on across mode changes.

### STUDIO: the song home is the default view
The song is the edited object; STUDIO opens on its **song home** (STUDIO 1), not on X0X's pattern
views. Its header names the song, the transport, BPM and bar.beat; below it sit the song card and
the role × clip grid with its cursor, and the K1–K4 strip (the cursor clip's length, mode, launch
and swing). SELECT moves the cursor between roles, ALGORITHM along a row, SEL launches the clip
under the cursor; the keys mirror the grid. SEQ opens the cursor clip's settings (STUDIO 5),
creating an empty clip first if the slot is empty; a plain tap on a key selects and launches that
clip at its own quantize, and **HOME + tap launches it now** (immediate, as in LIVE); HOME + hold a
pad (or SEL on the cursor) clears with a ring (STUDIO 9); holding a loaded pad copies onto a blinking
empty same-role slot (STUDIO 14); HOME+REC undoes and HOME+PLAY redoes with a toast (STUDIO 10). **The
clip engine is the only audio source in every normal view** (the song home, the sound/FX/MIX/GLOBAL
pages and LIVE), so tweaking a sound never stops the clips.

### Classic patterns: removed
X0X's pattern editors, its pattern sequencer and its song arranger are **gone** (TCLS, owner
decision [ADR-0004](../decisions/0004-remove-classic-shorten-delay.md)). There is no GLOBAL →
Classic row, no CLASSIC header, no `GLO: BACK TO SONG` footer and no path into a pattern editor:
HOME and SEQ on a sound/FX/MIX/GLOBAL page return to the song home, and EDIT opens the cursor
role's sound page (the 909/303 pages, or an FM6/NOISE macro card). The clip engine is the only
thing that owns the audio, so there is no hand-over and no third context next to STUDIO and LIVE.
X0X's project patterns are no longer held in RAM or written to flash; the sound and kit settings
are still read at boot, and the inherited sound/FX/MIX/GLOBAL pages survive.

## LIVE mode

### Two layers, toggled by SEL
| Layer | Keys do | Screen |
|---|---|---|
| **PADS** (the default) | Launch clips, laid out by the pad map | Role × clip grid with clip states |
| **PLAY** | Play the **focused role** live, in the song's scale. OCT−/OCT+ transpose. | Role name, sound, scale, and a strip showing clip status |

- The SEL LED shows the layer: off means PADS, lit means PLAY. A header badge says the same. Switching layers never changes what's playing.
- **Focused role:** whichever role launched most recently. Turn ALGORITHM to step to another role.
- **SEL toggles the PADS/PLAY layers** in LIVE and confirms inside overlays. **Turning SELECT does nothing in LIVE**, so a stray bump can't change anything.
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
- EDIT opens the setup list. SELECT chooses a row; ALGORITHM changes its value; SEL opens Assign keys or Take. EDIT leaves setup; HOME goes back one level, remaining in LIVE.
- **Assign keys:** SEL opens the assignment screen. Hold a key and turn PRESETS through every clip of every role, five role stops, six punch FX, Live note and None (the key does nothing). Release commits and a toast names the key and target; a hold that ends on the starting target changes nothing and does not dirty the song. Assignments apply to LIVE PADS only: STUDIO's keys always mirror the grid (key k is clip k). Leaving before release cancels. Assigned FX use the same tap-to-latch and hold-for-momentary gestures as the FX layer; a mapped Live note plays the focused role at the key's pitch in PADS. Opening setup releases held panel and MIDI notes.
- **Launch on:** Next bar, Next beat or End of clip. New clips and MIDI Program Change use this default; existing clips keep their own launch setting.
- **Stage lock:** Off or On. While playing, clearing from launch mode is blocked. LIVE pad holds do not clear or copy clips; take clear remains available.
- **External keys:** Omni or channels 1–16. Changing channel releases held external notes.
- **Program change:** Off or On, enabling quantized clip launch on the external keys channel.
- **Clock in:** Auto or Internal, the same GLOBAL setting. Auto follows incoming clock and shows EXT with the followed tempo in the LIVE header. Internal restores the song tempo.
- **Take:** SEL opens the existing take card; SEL plays, PLAY or SEL stops playback, and holding REC 1.5 s clears it with the existing ring and undo.
- Assignments, launch default, stage lock and MIDI options belong to the song and mark it dirty. Autosave writes only while stopped and idle after leaving LIVE; Clock in belongs to GLOBAL settings.
- SEL toggles PADS/PLAY on the performance screen and confirms rows inside setup.

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

Role mutes and punch FX are performance state (decided 2026-10-11). They reset when you leave LIVE and are never saved with the song.

- **K1** is a bipolar filter: low-pass to the left, high-pass to the right.
- **K2** is Space, **K3** is Echo and **K4** is Drive.
- **FX held or double-tapped** turns the first six lower keys into punch FX: **repeat 1/8, repeat 1/16, tape stop, filter dive, echo throw, freeze**.
  - Tap to latch, hold for momentary.
  - Upper keys mute roles, through the clip engine: a role muted mid-note releases its notes, and unmuting rejoins the clip in time.
  - HOME+FX resets everything, and leaving LIVE releases every effect and clears every mute.
- **K1–K4 changes you didn't record revert** when you leave LIVE.

**SONGS is a stopped view (2026-10-11).** Opening SONGS stops the transport. PLAY, HOME+PLAY and REC do nothing there, so song files are only written while everything is silent. Clip undo belongs to one song: opening another song starts a fresh undo history, and HOME+REC in SONGS undoes the last song delete. A switch away from a song whose save failed is refused; **hold SEL for 1.5 s** on the open action to discard those unsaved edits and open anyway (the ring warns that undo cannot restore them).

## STUDIO tasks (R15)
| Task | Gesture |
|---|---|
| New song | SAVE held → SONGS → `+ New song` → Empty, Starter or Copy current |
| Launch a clip now | HOME + tap a pad: immediate, not quantized (a plain tap uses the clip's own launch setting) |
| Open, copy, rename or delete a song | SONGS → EDIT on the song → menu. Delete is hold-to-confirm and undoable. |
| New clip | Cursor on an empty slot. REC records from the next bar (pressing REC again closes the loop), or SEQ opens step entry. |
| Edit a clip | SEQ cycles NOTES (or the DRUM grid) → MOTION → SETTINGS |
| Copy a clip | Hold the source pad, then tap an empty `+` slot. To move a clip, copy it and then clear the original. |
| Clear a clip | HOME + hold the pad for 1.5 s. A ring names the clip. Undoable. |
| Shorten a melodic clip | Notes starting past the new end are removed; undo restores the length and notes together. New or moved notes stay within the clip length. |
| Record knob motion | Turn knobs while recording a clip. HOME + touch a knob clears that lane (on the sound pages and the clip MOTION page; on the song home the K1–K4 edit the clip, so HOME + a knob does nothing there). |
| Undo / redo | HOME+REC / HOME+PLAY. A toast names the action. |
| Save | SAVE tap. Autosave also runs while stopped. A failed save is retried by SAVE; holding SEL on a refused song switch discards the unsaved edits (not undoable). |
| Help | GLO + any button shows that button's card |

Deferred: HOME+SAVE capture, OCT± clip banks, and the Gate clip mode.

## External MIDI (R12)
- Use **TRS MIDI IN**. The FM-1 has no USB host.
- Notes on the live channel play the focused role.
- Program Change launches clips, quantized.
- Clock comes in over USB or TRS. Clock out over USB comes after v1 if it's easy.
- The live channel and Program Change are per song (`song.live_channel`, `song.pc_launch`), edited in LIVE SETUP. Defaults are **Omni** and **PC launch Off**.
- T11i: **CC 120 and CC 123** release everything external MIDI holds, plus the engine's all-off.
- T11i: **CLOCK IN**, a GLOBAL row, is **Auto** (follow the incoming clock when present) or
  **Internal** (ignore it, and keep the song's own tempo). With Auto, F8 ticks set the tempo, and
  a small **EXT** indicator shows while the clock is being followed; when the clock stops the
  transport keeps playing at the last tempo and EXT goes away. Switching to Internal restores the
  song's own `bpm_x10` (the clock never overwrites it).
- T11i: **Start** (0xFA) restarts at bar 1, step 0, relaunching what was playing; **Continue**
  (0xFB) resumes from the stop point; **Stop** (0xFC) stops. Song Position Pointer is not
  supported (the clip engine has no seek).
- T11i/TCLS: external MIDI notes, Program Change and the clock reach the app in **STUDIO and
  LIVE**; the clip engine is the only audio source, so there is no X0X per-part MIDI handling.

## Open
- Velocity.
- Whether drum clips share the melodic step model.
- The minimum text size, set at RC0.
