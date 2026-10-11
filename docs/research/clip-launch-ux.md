# Clip launching and performance sequencer UX

Research into how clip launchers and grooveboxes handle launching, quantization, editing safety and performance recording, distilled into rules for the FM1. Gathered 2026-10-10 from web sources only.

Items marked **[K]** come from the researcher's background knowledge, not a fetched source, and should be verified before relying on them.

Related: [requirements](../product/requirements.md) · [UX principles](../product/ux-principles.md) · [FM1 hardware](fm1-hardware.md) · [GrooveOS](grooveos.md)

## 1. Novation Launchpad for iOS (Ampify)

Sources: [product page](https://us.novationmusic.com/software/launchpad-ios?setCurrencyId=2), [Bonedo review](https://www.bonedo.de/artikel/novation-launchpad-fuer-ios-test/3), [Cult of Mac](https://www.cultofmac.com/news/novation-launchpad-live-loops), [MusicTech guide](https://musictech.com/guides/buyers-guide/best-music-remixing-apps/), [v1.11 per-track FX](https://www.sonicstate.com/news/2015/03/12/novation-launchpad-for-ipad-update/)

### Grid and launching
- The grid has 48 pads in 8 groups of 6.
- **Each column is one instrument role**, colour-coded (drums green, bass blue, ...).
- **Rows group loops that work together**, so launching a row changes the song section.
- **Only one loop plays per column.** Tapping another loop in that column swaps it in. Tapping the playing loop stops it. A global Stop stops everything.
- Each pad has a play mode:
  - Loop repeats.
  - One-shot plays while held, which is effectively a gate.
  - Retrigger plays once through.
- **Every launch is quantized** to the next beat or bar. The user never sets a quantize value.
- Tempo comes from BPM +/-, tap tempo or Ableton Link. Key and tempo can change at any time through time-stretching.

### FX
- The FX are Stutter, Autofilter, Gater, Delay, Stopper and Flanger.
- **Autofilter is one bipolar slider**: centre is neutral, up is high-pass, down is low-pass.
- Gater has 4 timing modes, Stopper is a held tape-stop, and Delay has 8 beat-synced times.
- The iPad version can route FX per track.

### Recording and saving
- Performances can be recorded and shared to SoundCloud or YouTube.
- **[K]** The output is rendered audio, not an editable list of launch events.

### Why beginners do well with it
- The loops are curated, so they are always in key and in time.
- Every launch is quantized.
- Each column carries one voice.
- Each effect has one control.
- Nothing has to be configured before it sounds good.

## 2. Peer products

### Ableton Live Session View
Sources: [Session View](https://www.ableton.com/en/manual/session-view/), [Launching clips](https://www.ableton.com/en/manual/launching-clips/)

- **Layout.** Tracks are columns and scenes are rows. Each track plays one clip at a time. Launching a scene fires its whole row.
- **Launch modes:**

| Mode | Press | Release |
|---|---|---|
| Trigger | Start | Ignored |
| Gate | Start | Stop |
| Toggle | Start, and the next press stops | Ignored |
| Repeat | Retrigger at the quantize rate while held | Stop retriggering |

- **Legato.** The new clip picks up the previous clip's play position.
- **Quantization.** Each clip can follow the global setting or override it. **[K]** The global default is 1 bar, and a queued clip blinks until it starts.
- **Follow Actions.** Each clip has actions A and B with chances (stop, again, next, previous, any, other, jump), triggered at clip end or after a set time.
- **Arrangement Record** logs clip launches, clip property changes, automation and tempo as **events, not audio**.
- **Capture and Insert Scene** turns whatever is playing now into a new scene.

### Novation Launchpad X (hardware) Session mode
Source: [Launchpad X User Guide v2](https://fael-downloads-prod.focusrite.com/customer/prod/downloads/launchpad_x_user_guide_v2_en.pdf)

The LED vocabulary:
- A queued clip **flashes**.
- A playing clip **pulses**.
- A stopped clip shows its colour statically.
- An empty slot is unlit.
- On an armed track, empty slots show **dim red**.
- A clip queued to record **flashes red**, and a recording clip **pulses red**.

Other behaviour:
- Pressing an empty slot stops that track.
- Stop is quantized to the end of the phrase. Mute is immediate.
- Capture MIDI grabs what was played even when recording was off.
- Held modifiers are temporary: hold Volume, adjust, release, and the previous view comes back.

### Ableton Note (iOS)
Source: [AppleVis](https://applevis.com/comment/158821) (single source)

- The app holds up to 8 tracks × 8 clips × 8 scenes.
- When you tap in a loop, Note **detects the loop length and closes the loop**. Quantize is applied afterwards.
- Duplicating a clip is the way to make variations.

### Synthstrom Deluge (closest analogue: grid, tiny display)
Sources: [community features](https://delugecommunity.com/features/community_features/), [tips](https://delugecommunity.com/reference/tips_and_tricks/), [performance view](https://delugecommunity.com/features/performance_view/), [getting oriented](https://delugecommunity.com/manual/introduction/getting_oriented/), [forum](https://forums.synthstrom.com/discussion/3110/song-mode-arranger-mode-not-sure-i-m-quite-getting-it)

**Song structure**
- A song holds tracks, and each track holds clips. One clip per track plays at a time.
- Row view and an Ableton-style Grid view (up to 128 clips) both exist.
- Arranger mode is a separate, strict timeline.

**Launch timing**
- Launches are automatic and length-aware. An armed section launches at the end of the longest playing clip, or at the next multiple of its own length.
- **Shift + launch launches immediately.** Quantized versus immediate is chosen per gesture.

**Grid view modes**
- In green (launch) mode, the pads arm, mute or record.
- In blue (edit) mode, the pads create, delete or copy.
- **Hold a clip and press an empty slot to copy it.** Holding a mode pad switches modes temporarily.

**Clip behaviour and recording**
- Clip modes are Infinite, Fill (lands so it ends at the next loop start) and Once.
- Catch Notes plays notes that are already in progress when switching instantly.
- A **launch countdown** on the display counts down bars and then beats until the queued launch.
- Pressing an empty pad while recording creates a clip, and recording starts on the next bar.
- **REC + SONG records clip and section launches into the Arranger.**

**Performance View**
- The view has 16 FX columns × 8 values.
- **A short press latches, and a long press is momentary.**
- Undo and redo are available.
- **SAVE blinks while there are unsaved changes.**

**Weak points**
- **No autosave.**
- Sean Luke criticises its "hot mess" of button combinations ([page](https://people.cs.gmu.edu/~sean/projects/synth/deluge.html); seen only via a search snippet).

### Elektron (Digitakt, Digitone, Analog Rytm, Analog Four)
Sources: [changing patterns](https://www.elektronauts.com/t/changing-patterns/99140), [temp save](https://www.elektronauts.com/t/ptn-temp-save-question/223976), [reload](https://stage2.elektronauts.com/t/analog-four-mk2-reload/78408), [trigless locks](https://www.elektronauts.com/t/trigless-locks-why-how-to-use-them/41953)

**Pattern changes**
- Sequential (the default) switches at the end of the pattern.
- Direct Start switches now and starts at step 1.
- Direct Jump switches now and keeps the step position.
- Temp Jump plays the new pattern once, then returns.
- The queued pattern number **flashes until it takes over**.

**Parameter locks**
- Each step can store its own value for any parameter.
- In live record, **turning a knob writes per-step locks**.
- **NO + encoder erases that parameter's locks.**

**Saving**
- Pattern and kit state autosave to a working copy.
- **Fn+Yes saves a known-good snapshot, and Fn+No reloads it.**
- Autosave on pattern change means a reload must happen *before* switching, and that timing window catches people out.

### Teenage Engineering: OP-Z, Pocket Operators, OP-1 field
Sources: [OP-Z project](https://teenage.engineering/guides/op-z/project), [OP-Z quick guide](https://teenage.engineering/guides/op-z/quick-guide), [PO-14 manual](https://manualsnet.com/teenage-engineering/po-14), [OP-1 field 1.7](https://synthanatomy.com/2026/04/teenage-engineering-op-1-field-1-7-tape-undo-before-gta-6.html)

**OP-Z performance**
- **The piano keys double as punch-in FX.** Add REC and the punch-ins are recorded to a dedicated **performance track**, which is sequenced like any other track.
- Pattern select is instant and keeps the step position. Project changes queue to the end of the bar.
- **Clearing works by holding Project + Stop until the bar fills. Releasing early cancels.**
- **Autosave is on by default.** A manual-save mode exists for lending the unit.
- Snapshot save and restore is a one-level revert.

**Pocket Operators**
- Hold Pattern and tap patterns to chain them. Repeats are allowed.
- **Hold Write and turn knobs** to record parameter locks.

**OP-1 field**
- It had no undo for years. Firmware 1.7 added 7-level tape undo, which shows the demand.

### Polyend Play
Sources: [manual](https://polyend.com/manuals/play/), [Perfect Circuit review](https://www.perfectcircuit.com/signal/polyend-play-review)

- In Perform mode, pads are punch-in FX columns. **Press and release latches the effect.**
- **Knob changes made in Perform revert when you leave the mode.** The performance layer is non-destructive.
- Each track has its own variations: orange means active, yellow means available.

### Korg and Roland motion sequencing
Sources: [Korg minilogue](https://en.wikipedia.org/wiki/Korg_Minilogue), [MC-101 cheatsheet](https://github.com/NLxAROSA/mc101cheatsheet), [TR-8S reference](https://static.roland.com/assets/media/pdf/TR-8S_Reference_eng04_W.pdf)

- **Korg:** knob moves during record become per-step values in a limited number of motion lanes (4 on the minilogue). **[K]** Func + knob clears that knob's motion on volcas.
- **Roland MC-101 and TR-8S:** motion is recorded per parameter while the sequence plays, and clip recording waits for the next bar.

### Squarp Hapax
Source: [Elektronauts thread](https://www.elektronauts.com/t/squarp-instruments-hapax-polychronic-performance-sequencer/168655?page=81)

- Pattern-change quantize options: 1, 4 or 8 steps; 1, 2, 3, 4, 8, 16 or 32 bars; or end of pattern.
- Also has follow actions and autoloads the last saved project.

## 3. Principles for a constrained device

### Mapping launching onto keys
- **Columns are roles, rows are variations, and one clip plays per column.** Launchpad iOS, Live, Note and Deluge all use this model.
- Pressing a slot replaces whatever plays in its column. Pressing the playing slot stops it.
- **Shift + key launches immediately.** Gate mode is an optional per-clip setting.
- An unmodified key press in launch mode is never destructive. Create, clear and copy go behind an explicit held edit modifier.

### Create, duplicate, clear, save
- **Create:** press an empty slot while recording. Recording starts on the next bar, and the second press closes the loop.
- **Duplicate:** hold the source slot and tap the destination.
- **Clear:** hold to confirm, with a visible fill. Releasing early cancels.
- **Save:** autosave a working copy, blink a dirty indicator, and keep explicit snapshots separate.

### Undo and safety
- Keep at least one level of undo for destructive edits.
- Provide snapshot and revert, and make sure revert never depends on timing (avoid Elektron's pitfall).
- FX and knob tweaks made in perform mode revert unless they are recorded or committed (Polyend).

### Feedback vocabulary

| State | Feedback |
|---|---|
| Empty | off or dim |
| Loaded, stopped | solid dim |
| Queued to play | fast flash |
| Playing | pulse on the beat, or solid bright |
| Queued to stop | flash at a different rate |
| Queued to record | flash red |
| Recording | pulse red |

- If the keys can't show these states, draw a clip grid on the screen, along with a **bars and beats countdown** until the queued launch.
- Blink a beat indicator on downbeats so quantized waits read as intentional.

### Quantization
- **The default is next bar.** Shift launches immediately.
- Never ask a beginner to choose a quantize value.
- Consider legato (keep the play position) when switching variations within a column.

### Recording knob motion
There are two models:
1. **Motion sequencing per clip** (Elektron, Korg, Pocket Operators, OP-Z): hold REC and twist. Each knob needs a way to be cleared.
2. **Performance recording into an arrangement** (Ableton, Deluge): an **event log** of launches and parameter changes on a timeline. It stays editable, costs very little flash, and replays as a song.

### Autosave
- Autosave the working state on a debounce and at power-off.
- **[K]** Writes must survive a brown-out: double-buffer the slots and checksum them.
- Explicit saves are never overwritten by autosave.

## 4. Recommended patterns
1. One column per role, one clip playing per column, and tapping the playing clip stops it.
2. Launches and stops quantize to 1 bar by default, and Shift launches immediately.
3. Flash means queued, pulse means playing, dim means loaded, red means record. Show an on-screen countdown.
4. Launch mode and edit mode are separate and always visible.
5. Create by pressing an empty slot while recording, starting on the next bar, with the loop closed automatically.
6. Duplicate by holding the source and tapping the destination.
7. Clear by holding to confirm with a progress fill.
8. Undo the last destructive action, and offer snapshot and revert that doesn't depend on timing.
9. The performance layer is non-destructive unless the user commits it.
10. Punch-in FX on keys: tap latches, hold is momentary, values are beat-synced, and one action resets all FX.
11. One bipolar filter knob, low-pass to the left and high-pass to the right.
12. Motion recording is hold REC and twist, and each knob's lane can be cleared and shown.
13. A performance is recorded as an event log that replays as a song.
14. Autosave the working state, show a dirty indicator, and offer a manual-save or lend mode.
15. Clip modes beyond Loop (Once, Gate, Fill) stay off the main path.

## 5. Anti-patterns
- A single tap clears or overwrites a slot.
- Revert depends on timing, or autosave silently commits jam changes.
- There is no autosave, or autosave overwrites an explicit save.
- Core actions need deep combinations that the manual doesn't document. This is the main stock FM1 complaint.
- Launches are unquantized by default, or a quantize value must be chosen before anything works.
- Queued and playing clips look the same.
- Modes have no visible indicator.
- Performances are recorded only as audio.
- Effects have many parameters each, or FX overlays hide the launch state.
- Motion lanes are unlimited and can't be seen.
