# GrooveOS as a design reference

GrooveOS 1.2.2 is a closed-source, $29 firmware for the FM-1. This page summarises it in our own words and keeps quotes short. It was researched on 2026-10-10 from the full manual, the site, press coverage and YouTube.

GrooveOS forbids decompiling and reverse engineering in its terms. **Use it only as a reference for how a user experiences it.** Do not copy its code, assets or manual text.

Related: [requirements](../product/requirements.md) · [firmware landscape](firmware-landscape.md) · [UX principles](../product/ux-principles.md) · [clip-launch UX](clip-launch-ux.md)

## Summary

- **Author and build.** Peter Gombos wrote it. It **patches stock V15** rather than replacing it.
- **Release history.** 1.0 shipped on 2026-10-02 and 1.2.2 on 2026-10-10.
- **What it is.** An 8-track loop groovebox with:
  - FM and VA engines, plus 8-pad kits;
  - a 64-step sequencer with p-locks and 16 stored patterns;
  - loop recording that also records knob moves;
  - a PERFORM view with mutes, punch-in FX, throws and 11 scenes;
  - 3 project slots plus autosave;
  - MIDI clock in and out, and a MIDI channel per track.
- **Fit for us.** It covers the "groovebox without a DAW" half of our goal well. It is weak on the **clip launcher** half:
  - no per-track clips on keys;
  - no loop/one-shot choice;
  - no phrase quantise;
  - no key remapping;
  - no linear performance recording.

## Features

### Engines
- **FM** is the stock 6-op engine with 12 voices.
- **VA** is new: saw/square/tri/sine, super, sub, noise; LP24/LP12/BP/HP filters; 8 voices.
- **Track types:** FM SOUND, VA SOUND, FM KIT, VA KIT, MIDI OUT. A track keeps both its FM and VA settings.
- **Kits** have 8 pads drawn from 12 drum models. Each pad has tune, decay, level, choke and send.
- **K1–K4 are macros** with sound-specific names.
- **Four preset banks** of 32. Each project stores its own copy of every sound it uses.
- **The sound browser filters by role** (BASS, KEYS, PAD, LEAD, PLUCK, DRUMS, FX), but tracks are typed by engine.

### Sequencer
- **Track sources.** Each track has a recorded loop, an optional step pattern and an optional arpeggiator.
- **Tracks.** Up to 8, each 1–8 bars long, so polymeter works.
- **Patterns.** 1–64 steps, rates from 1/4 to 1/32 (including triplets), swing, up to 4 notes per step.
- **Steps.** Each step has velocity, length and CHANCE. CHANCE also covers conditions: FILL, !FILL and A:B.
- **Stored patterns.** 16 per project (A–P). A track selects one through a menu row. CHAIN can link up to 16.
- **Transforms.** Transpose, direction, stretch, 2X and 1/2X, repeater, re-roll and DICE.
- **Kit tracks** use their own drum grid: 8 pads × 16 steps per bar. Each hit can have ratchet, flam and nudge. *Unconfirmed:* whether stored patterns apply to kit tracks.
- **Limits.**
  - 384 events per track and 1,536 per project.
  - 96 automation points per track.
  - A project is at most 11,968 bytes.

### PERFORM view
| Control | Function |
|---|---|
| White keys 1–8 | Track mutes, queued to the launch line. Hold to solo. SHIFT stages several. |
| White keys 9–16 | Punch-ins: DIVE, LIFT, ECHO, WASH, 1/4, 1/8 and 1/16 repeats, FILL. Hold for momentary, tap to latch. |
| Black keys | 11 scenes, A–K. Tap launches on the next bar; tap again to cancel. SHIFT+key captures the current state. |
| K1–K4 | Throws: FILTER, SPACE, ECHO, DRIVE. SHIFT+SEL glides them back. |
| SEL | Tap tempo |

- **A scene** is the pattern each track plays plus which tracks are muted.
- **Stage lock.** Clearing is blocked while music plays.
- **Feedback.** Lit means playing, dark means muted, blinking means queued.

### Quantisation
- **LAUNCH** can be OFF, BEAT or BAR (default BAR). It applies to REC, mute, solo and pattern changes.
- **Scenes** always launch on the next bar.
- **There is no phrase option.**

### Projects
- **Storage.** 3 slots plus an autosave that resumes after a power cut.
- **Saving stops playback.**
- **Export.** 2–4 KB `.groove` files over WebMIDI.

### MIDI
- **Clock.** INT, OUT (USB verified) or EXT.
- **Per-track input channels.** An external keyboard plays a track live and records into it while REC is on.
- **Other.** MIDI OUT tracks, Program Change, panel CCs 20–29, and FX CCs.
- **BLE-MIDI** comes from the stock firmware.
- **USB host is not mentioned anywhere.**

### FX
- **The chain.** One shared, reorderable chain: FILTER, REVERB, DELAY, DRIVE, CHORUS, PHASER, followed by a soft limiter.
- **Sends.** Each track and each kit pad has a send into it.
- **MAIN strip.** K1–K4 control SPACE, ECHO, DRIVE and FILTER.

### Recording
- **REC overdubs a loop on one track,** after a one-bar count-in.
- **Recorded knob moves.** Macros, level, send and 7 global FX values are written into the loop. Touch mode replaces existing moves.
- **Capture.** SHIFT+SAVE keeps the last loop as a take.
- **PRINT** turns a pattern or arp into notes.
- **Undo and redo.** 16 levels.
- **Not recorded:** scenes, mutes or throws. **There is no linear arrangement or audio recording.**

## Control model

- **Navigation.**
  - **SELECT** cycles through five views: TRACKS, MIX, PERFORM, KEYS, PROJECT.
  - **SEQ, ARP, FX, ENV, LFO, EDIT and GLO** each open their own screen.
- **Shift layer.**
  - **HOME is both back and SHIFT.** A tap below an adjustable threshold (200–400 ms) is back. Holding past it lights the key LEDs to show current values. At 600 ms a cheat sheet appears.
  - **A long-press on most buttons shows a help card.**
- **Cursor and values.**
  - **ALGORITHM is the cursor everywhere.** SEL opens the context menu for whatever is under it.
  - **K1–K4** edit four values shown in an on-screen legend. SHIFT gives a second row of four.
- **Menu rows.**
  - Symbols mark submenus (▸) and actions (↵).
  - Dimmed rows explain why they are unavailable.
  - Destructive prompts default to CANCEL.
- **Keys while SHIFT is held:** F3–B3 set loop length (1/2/4/8). C4–E4 set LAUNCH. F4–G4 set QUANT. A4 is click, B4 is count-in, F#5 is tap tempo.

| Task | How |
|---|---|
| New song | PROJECT → `NEW` → `EMPTY`. You get 4 tracks (kit, bass, keys, lead), 120 BPM, A minor. |
| New track | TRACKS → `+ ADD TRACK` → choose the engine |
| Clear a track | Hold SHIFT+REC for 1.5 s while a filling ring names the target. Or use the menu → `CLEAR`. |
| Duplicate | Menu → `DUPLICATE` |
| Undo / redo | SHIFT+REC tap / SHIFT+PLAY. A toast names the action, e.g. `UNDO T3 DICE`. |
| Save | SAVE. Autosave runs every 2 minutes while stopped. |
| Rename | Menu → `RENAME`, then type with the keys |

**Feedback**
- **Header:** view, play/rec state, bar.beat, tempo, 4 beat squares, battery, and an **unsaved-changes dot**.
- **Hint line:** rotates `CONTROL: VERB` pairs.
- **Toasts** confirm actions.
- **Queued things blink.**
- **While recording, a red glow** shows at the screen edges.

## Install and licensing
- **Install.** WebMIDI SysEx from Chrome or Edge. It runs a checklist and a SHA-256 check, then uses M-VAVE's updater in two stages.
- **Revert.** Restoring V15 is possible but erases GrooveOS projects.
- **Safe mode.** Hold OCT-+OCT+ at boot to start the stock firmware.
- **Browser simulator.** At `?sim=1` and `/emu`, so people can try before buying.
- **Licence.**
  - Proprietary, with no reverse engineering allowed.
  - A personal licence covering 1.x updates.
  - Purchases are tied to an account.

## Our requirements vs GrooveOS

| Requirement | GrooveOS | Notes |
|---|---|---|
| Multiple saved sequences, no DAW | Has | Only 3 project slots |
| Drum/bass/lead/noise types | Partial | Tracks are typed by engine, not role. Noise is a parameter, not a track type. |
| Several sequences at once | Has | 8 tracks |
| Dedicated launch mode | Partial | PERFORM has scenes and mutes but no clip grid |
| Keys mapped to clips | Partial | Scenes only, no per-track clips |
| Loop vs one-shot | Missing | |
| Quantized beat/bar/phrase | Partial | No phrase option |
| Alternate clips per role, exclusive | Partial | Selected through a menu row or a scene |
| Reassign clips to keys | Missing | |
| Split keys: launch + live play | Missing | PERFORM takes all 27 keys |
| External MIDI keyboard | Has | Through MIDI channels. USB host not stated. |
| Save whole song | Has | |
| Record performance incl. knob moves | Partial | Loop lanes only. Scenes, mutes and throws are not recorded. |

## Reception
It has been out for 8 days, and feedback so far is thin.

**Praise**
- Synth Anatomy called it the most comprehensive firmware yet.
- People liked the web demo.
- Time To House rated it the most feature-complete.

**Complaints**
- $29 is too much for a synth that costs about $60.
- It is closed source.
- People suspect it was "vibe coded".
- People worry about bricking their unit.
- Its own changelog lists problems:
  - key latency of about 160 ms before 1.2;
  - knob lag;
  - FM crackle after long uptime;
  - save formats that change between versions.

## What to learn
1. **A shift layer that documents itself.** HOME is both back and SHIFT. Holding it lights the key LEDs to show current values, then shows a cheat sheet.
2. **On-screen help.** A `CONTROL: VERB` hint line, plus a help card on long-press.
3. **One undo gesture with a named toast.** Clearing needs a hold with a filling ring that names the target. Confirm prompts default to CANCEL.
4. **Queued actions blink,** and a second tap cancels before the change lands. Several mutes can be staged to land together.
5. **Performance keys:** tap to latch, hold for momentary, hold to solo.
6. **Capture the last loop, and PRINT** patterns or arps into notes.
7. **Stage lock** blocks destructive actions while music plays.
8. **Projects own their sounds.** Saves are A/B safe, and autosave resumes after a power cut.
9. **The browser simulator doubles as a try-before-you-buy tutorial.**

## Where we differ (our differentiators)
1. **Per-role clips on keys** that switch exclusively within their role.
2. **A user-assignable key layout** with a launch zone and a live-play zone.
3. **A loop, one-shot or gate setting per clip,** plus phrase quantise (2, 4 or 8 bars).
4. **Linear performance recording.** Launches, mutes, throws and knob moves go into an arrangement.
5. **Role-based tracks, including noise,** and one sequencer model for drums and melody.
6. **More than 3 projects.**
7. **Save without stopping,** if flash timing allows it.
8. **Open source,** with a stable or migrated save format.

## Conflicting facts to resolve
- **Velocity.** The GrooveOS manual says the keys are velocity-sensitive. The [hardware research](fm1-hardware.md) (Felucca's HAL) found no velocity sensing. Check this on the device.
- **Engines.** Synth Anatomy reports "four engines including acid". The manual describes FM, VA and kits only.

## Sources
- GrooveOS: [home](https://www.groove-os.com/) · [features](https://www.groove-os.com/features) · [learn/perform](https://www.groove-os.com/learn/perform) · [releases](https://www.groove-os.com/releases) · [terms](https://www.groove-os.com/terms) · [credits](https://www.groove-os.com/credits)
- Manual: [controls](https://www.groove-os.com/manual/controls) · [tracks](https://www.groove-os.com/manual/tracks) · [sound](https://www.groove-os.com/manual/sound) · [kit](https://www.groove-os.com/manual/kit) · [seq](https://www.groove-os.com/manual/seq) · [mix](https://www.groove-os.com/manual/mix) · [perform](https://www.groove-os.com/manual/perform) · [recording](https://www.groove-os.com/manual/recording) · [project](https://www.groove-os.com/manual/project) · [midi](https://www.groove-os.com/manual/midi) · [firmware](https://www.groove-os.com/manual/firmware)
- Press:
  - [Synth Anatomy](https://synthanatomy.com/2026/10/groove-os-turns-the-m-vave-fm-1-into-an-8-track-groovebox.html)
  - [Time To House](https://timetohouse.com/en/articles/mvave-fm-1-firmware-wars-groove-os-sloop-felucca-x0x)
  - [dreyandersson.com](https://dreyandersson.com/blog/m-vave-fm-1-custom-firmware/)
  - [Synthtopia roundup](https://www.synthtopia.com/content/2026/10/04/alt-firmware-options-for-the-m-wave-fm-1-pocket-synthesizer/)
  - [MatrixSynth](https://www.matrixsynth.com/2026/10/m-vave-fm-1-now-is-pocket-daw-groove-os.html)
  - [Digital Mixing](https://digital-mixing.com/en/music/19554-m-vave-fm-1-custom-firmware-community-oses-turn-the-pocket-fm-synth-into-a-groovebox-05-10-2026/)
- Video:
  - [DECIMA1](https://www.youtube.com/watch?v=CyOitggc6eM)
  - [Grzegorz Bojanek](https://www.youtube.com/watch?v=0xG1tWFHPa4)
  - [NatLife Sounds](https://www.youtube.com/watch?v=7mvNyEvGmQI)
