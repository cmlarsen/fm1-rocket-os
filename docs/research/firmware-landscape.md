# FM-1 custom firmware landscape

A survey of every FM-1 firmware we found, as of 2026-10-10/11. The scene is about three weeks old and repos change every day, so recheck star counts and versions before relying on them. Most feature details come from each project's own README or manual.

Related: [hardware](fm1-hardware.md) · [GrooveOS deep dive](grooveos.md) · [community sentiment](community-sentiment.md) · [dev tooling](dev-tooling.md) · [ADR-0001 base platform](../decisions/0001-base-platform.md)

## Summary

- **More than 30 firmwares exist.** Most are GPL-3.0 forks of **Felucca** (the base platform) or **SLOOP** (the main groovebox fork). Two closed firmwares patch stock V15: **Baud Girl FM-1+VA** (free) and **GrooveOS** ($29).
- **Polymetric 4–8-track step sequencing is solved.** All the major firmwares have 64 steps, chords, p-locks, chance, ratchets, overdub, and song chaining.
- **Per-track alternate patterns that queue to the bar** exist in **Melodee** (8 banks per track), **X0X** (32 patterns per part) and **GrooveOS** (16 stored patterns plus scenes).
- **Gaps that match our requirements:**
  - Only **FM1 Move** has a true clip grid, and it has never run on hardware.
  - **No firmware splits the keys** between launching and live play. All of them reach launching through a held modifier.
  - **No firmware records one linear timeline** of launches, mutes, FX punches and knob moves.

## Platform baseline

- **Updates** arrive as `.fwsc` packages over USB-MIDI SysEx. They are checked only by CRC-16 and are unsigned.
- **Recovery options**, from least to most hands-on:
  - USB RESCUE: hold OCT− at power-on.
  - [MvaveFM1Unbricker](https://github.com/Quixotic7/MvaveFM1Unbricker).
  - The [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter) (RP2040) or [flipper-fm1-recovery](https://github.com/merthsoft/flipper-fm1-recovery).
- **Stock V15 sequencer** has long notes, voice binding, chain playback and custom step length ([release notes](https://yms-file-store.oss-cn-hongkong.aliyuncs.com/software/releaseNote/firmware/FM-1.txt)).
- **Directories:**
  - [FM-1 Firmware Hub](https://fm1.designburgapps.com) by zvenson: a one-click Web MIDI installer for 17 GPL firmwares, with SHA-256 checks and nightly pulls of the newest releases.
  - [fm1-editor.com/firmware](https://fm1-editor.com/firmware/): about 32 firmwares.
  - [awesome-fm-1](https://github.com/cicloid/awesome-fm-1).
- **Dev tooling:**
  - [Dinghy](https://github.com/zvenson/dinghy): a dev template.
  - [fm1-emulator](https://github.com/simonjohansson/fm1-emulator) (Rust).
  - [FM1VST](https://github.com/Quixotic7/FM1VST): runs Felucca-family firmware as a plugin.
  - See [dev tooling](dev-tooling.md).
- **Communities:**
  - [r/MVaveFM1](https://www.reddit.com/r/MVaveFM1/).
  - [KVR](https://www.kvraudio.com/forum/viewtopic.php?t=632127).
  - [Gearspace](https://gearspace.com/threads/m-vave-fm-1.1465371/).
  - [HN](https://news.ycombinator.com/item?id=49994065).

## Profiles

### Felucca (base platform)
- **Project.** [repo](https://github.com/hugelton/Felucca) · [site](https://hugelton.github.io/Felucca/) · GPL-3.0-only · 386★ · v1.5.1 (2026-10-10) · by Leo Kuroshita (@kurogedelic).
- **Sound.**
  - 13 engines: ANALOG, FM6, PHASE, LOFI, SAMPLE, VOICE, TRIO, WHEEL, GRAIN, PHYS, NOISE, SLICE, DRUM.
  - 8 voices shared by 4 tracks.
  - FX: distortion, chorus, delay, reverb, limiter.
  - A 4-slot modulation matrix per track.
- **Sequencer.**
  - 4 polymetric tracks, each with its own LEN, DIV, SWING and GATE.
  - 64 steps with 4-note chords, tie, accent, slide, chance, nudge and ratchet.
  - A piano roll, and a drum grid for drum tracks.
  - Loop recording with overdub, and step recording.
  - AUTOMATION stores 128 events per project.
  - SEQ TOOLS: clear, reverse, shift, random, cook.
- **Patterns and songs.**
  - **There is one pattern per track.** "Patterns" are the 4 project slots A–D.
  - SONG holds 16 sections. Each section names the slot that each track plays.
  - Saves: 4 projects, 32 user presets, and an autosave that restores the last session.
- **MIDI.**
  - Each track has its own channel, over TRS or USB.
  - Clock in from INT, USB or TRS.
  - MIDI Learn for 16 CCs.
  - USB stereo audio in.
- **Performance.**
  - Hold FX and the 16 white keys become momentary master FX: repeat, reverse, filter, tape stop, freeze, throws and more.
  - While FX is held, black keys 1–4 mute tracks 1–4.
  - Hold LFO and turn a knob for a tweak that springs back on release.
- **UX.**
  - Tap a page button to open it, and tap again for the next page. Double-tap latches a layer.
  - Hold a step and turn a knob to set a p-lock.
  - Hold SAVE to undo.
  - OCT+ confirms and OCT− goes back.
  - A HELP hints setting exists, but the layers are dense.
- **Code layout.**
  - About 90 flat C files under `firmware/src`, plus `firmware/hal`, `firmware/loader`, `tools/`, `web/` (installer, editor, WASM emulator) and `tests/` (host tests and CPU budgets).
  - Engines are `eng_*.c`.
  - `seq.c` runs inside the audio ISR once per control block. Its header says "engines never see where a note came from".
  - Other modules: `song_chain.c`, `motion.c` (p-locks and automation), `perform.c`, `ui_*.c`, `project.c` / `storage.c` / `storage_hw.c`, and `midi_*.c`.
- **Data model** (`core.h`).
  - `NPART 4`, `NSTEP 64`, `NVOICE 8`.
  - `step_t` is about 10 bytes: `note[4]`, n, time, flags, vel, drum `hit`/`acc` masks, and probability.
  - **`track_t` embeds `step_t step[NSTEP]`, so each track holds exactly one pattern.**
  - `motion_store_t` holds 128 three-byte events.
  - `chain_row_t { slot[NTRK], repeat }` × 16.
- **Song playback.** Before PLAY, all 4 slots' steps are copied into RAM. The ISR only switches indices, and every section starts all tracks at step 1 together.
- **Storage.**
  - Versioned "FUN1" to "FUN10" records of about 3.4 KB each.
  - Slots live in `.noinit` RAM and are mirrored to flash.
  - Old formats are converted on load. New formats don't load on older firmware.

### SLOOP
- **Project.** [repo](https://github.com/isod89/sloop-fm1) · [installer](https://isod89.github.io/sloop-fm1/webapp/installer/) · GPL-3.0 · 384★ · v2.6 beta · 59 open issues.
- **Concept.** 3 synth tracks plus 1 drum track, with 16 drum sounds on the white keys. *"No factory patterns, nothing to load: everything you hear, you play."*
- **Sound.**
  - 12 engines, 153 sounds, 37 kits, and 4 sample slots.
  - 27 punch-in FX on the keys.
  - DUST, DUCK and a DJ filter.
- **Sequencer.**
  - 64 steps, with a per-track length and a division from 1/32 to 2 bars.
  - Chords, ratchets and nudge.
  - 24 p-locks per track.
  - Fills.
  - **No automation lanes.**
- **Song.**
  - 4 sections A–D, and each section is a whole project. *"SAVE + key 5–8 save the loop into section A–D"*.
  - Sections play on the next bar.
  - Quick chain: hold SAVE and tap up to 8 sections.
  - SAVE + key 13 toggles song mode. SAVE + key 14 starts **SONG REC**, which records the order of sections.
- **Recording and mixing.**
  - REC overdubs immediately.
  - A free take on an empty project sets the loop length.
  - **Hold REC for 2 s to clear a track.**
  - GLO + keys 1–4 mute, and GLO + keys 5–8 solo.
- **UX.** *"Every function button is a layer. Hold a button, touch a key."* *"The screen always shows what the hands can do next."* The docs are good.
- **Forks.**
  - **sloopDX**: Dexed only. 128 steps from a shared pool of 256. Its README notes that a USB keyboard plugged directly into the FM-1 can't work.
  - **Others**: Floyd FM, DX7 Banks, ALG, FM1 Quest, ORBIT, Optimist, FM1 Move, NoteSorcery.

### X0X
- **Project.** [repo](https://github.com/charlesvestal/fm1-x0x) · [manual](https://charlesvestal.github.io/fm1-x0x/manual/) · GPL-3.0 · 115★ · v1.0.5.
- **Parts.** 909, 808, two 303s (Open303 with TB-3PO generators), and BREAK.
- **Patterns.**
  - **32 patterns per part, each part independent.** *"While playing, a new pattern starts when the current bar ends"*, and a queued pattern blinks.
  - PRESETS on HOME changes all parts. PRESETS on a part's own screen changes only that part.
- **Song.**
  - 192 bars. Each bar stores the pattern and mute state of each part.
  - **Song record**: in SONG mode with REC on, changes are written bar by bar.
- **Motion.**
  - Each pattern has its own motion, recorded with REC on.
  - Turning a knob with REC off overrides it for one pass.
  - SAVE held + knob clears it.
- **Controls.**
  - HOME + white key picks a pattern. HOME + black key mutes.
  - SAVE held + white key copies and pastes. SAVE + REC clears.
  - HOME + REC is undo, and HOME + PLAY is redo.
  - **Hold any button for about 1 s to see a help card for the current screen.** This is the best discoverability found in any firmware.
- **Saving.** The firmware never saves while playing. Autosave runs after 4 s of idle while stopped.
- **Test harness.** `host/x0x_host` replays `tests/scenarios/*.x0x` scripts on the host.

### Melodee
- **Project.** [repo](https://github.com/keremimo/melodee) · GPL-3.0 · 39★ · v1.1.0.
- **Engines.** PROPHET, FM6, CZ-1, SID, DRUM. **It uses both CPU cores.**
- **Banks.**
  - **8 banks per track.** SEQ held + white key 1–8 selects one.
  - Changes queue to each track's own loop end. Selecting the active bank cancels the queue.
  - Hold one bank key and press another to copy.
- **Song.**
  - 16 rows, each with a bank per track and repeats.
  - **Holding REC on SONG turns a logged jam into an arrangement.**
- **Motion.** A shared pool of 64 events.
- **Navigation.** "One navigation language": KNOB 1 moves across, KNOB 2 moves down, OCT+ enters and OCT− goes back.
- **Saving.** SAVE is undo.

### GrooveOS (paid, closed)
See [grooveos.md](grooveos.md).
- 8 tracks, 16 shared stored patterns, and 11 scenes on the black keys.
- LAUNCH can be BAR, BEAT or OFF.
- Retroactive takes.
- No timeline.
- PERFORM plays no notes.

### Baud Girl FM-1+VA (free, closed)
- **Project.** [site](https://baudgirl.com/work/FM-1+VA) · build 97 · source promised but not yet released.
- **Approach.** Patches V15. Adds a VA engine and an 8-bit engine.
- **Patterns.**
  - 16 patterns of 64 steps, with chords of up to 9 notes.
  - Each pattern has its own chain and repeat settings.
  - A live switch lands on the beat.
- **Controls.** HOME + knob reassigns that knob live.
- **MIDI.** Clock out over USB and BLE.

### Others

| Firmware | Link | Relevance |
|---|---|---|
| **FM1 Move** | [repo](https://github.com/Nicodesy06/FM1-MOVE) | **The only clip/scene grid.** 8 tracks × 6 clips. Hold SAVE: keys 1–6 select clips, keys 9–14 select scenes. Clips launch on the next bar. Key lights show playing, queued, filled and empty. No knob recording. **It has never run on hardware.** |
| **zp12** | [repo](https://github.com/zvenson/zp12) | SP-12 style sampler. **Loops 1–11 are on the black keys** and switch at loop end. **MIDI Program Change selects loops.** 4 songs × 32 steps. |
| **ORBIT** | [repo](https://github.com/dspaudio/orbit) | A **4-track event-tape timeline** with COPY, LIFT and DROP. It is edited, not recorded live. |
| **Hortator** | [repo](https://github.com/DeadActive/hortator) | 8-track drum machine with Grids and per-step MOTION. |
| **Optimist** | [repo](https://github.com/w0ts/optimist) | Features are chosen at build time against flash and RAM budgets. Per-track patterns, scenes, and a song chain A–P. |
| **NoteSorcery** | [repo](https://github.com/catacombius/NoteSorcery) | 8 tracks. Exports to an Ableton Live set or MIDI. Host only. |
| **WaveLoop** | [guide](https://eli7vh.github.io/Felucca/mod/) | Felucca driven from an Arturia MiniLab 3, with **setlist song switching**. |
| **SLICE64-FM** | [editor](https://jeymadcat-ops.github.io/SLICE64-FM-web/) | A 4-track tracker with an order list. |
| **Lunar Modulator** | [repo](https://github.com/ip2k/lunar-modulator) | MIT licence. 8 tracks. Simulator only. |
| **fm1-polyseq** | [repo](https://github.com/NOVALENTI/fm1-polyseq) | Strict C99 with no dynamic allocation, and its own HAL. |
| **Jangada** | [repo](https://github.com/zednaked/jangada) | A Felucca fork with drones and macros. |
| **April OS** | [site](https://beashoko.github.io/april-os/) | A sampling fork with 8 tracks. |

## Comparison

| Firmware | Open | Tracks | Clips per track | Quantized live switch | Song | Knob recording | Ease of use | Maturity |
|---|---|---|---|---|---|---|---|---|
| Stock V15 | Closed | 1 | 16 patterns | No | Chain | No | Simple | Stable |
| Felucca | GPL | 4 | 1 (4 project slots) | No | 16 sections | Per project (128 events) | Dense layers | High |
| SLOOP | GPL | 3+1 | 1 (4 sections) | Sections, next bar | Song record of section order | Locks only | Good docs | Beta |
| X0X | GPL | 5 | **32** | Bar, per part | 192 bars; **song record** | Per pattern | **Help cards** | 1.0 |
| Melodee | GPL | 4 | **8** | Each track's own loop end | Jam → song | 64 events | One navigation language | 1.1 |
| GrooveOS | $29 | 8 | 16 shared | BAR/BEAT/OFF | 11 scenes | Per loop | Polished | 1.2 |
| FM1 Move | GPL | 8 | **6 + scenes** | Next bar | Clips replace songs | None | Move-like | Never run on hardware |
| zp12 | GPL | 8 ch | 16 loops (global) | Loop end; **PC launch** | 4 songs | None | — | Test |

## Gap analysis vs our [requirements](../product/requirements.md)

| Req | State of the art | Gap we fill |
|---|---|---|
| R3 simultaneous sequences | Solved, 4–8 tracks | Pick a track count that fits RAM |
| R4/R5 launch mode, keys → clips | FM1 Move (untested), scenes in GrooveOS | **A hardware-tested clip grid on keys, showing state** |
| R6/R7 loop vs one-shot | None | **Per-clip launch mode** |
| R8 quantized switch | Bar (X0X, SLOOP), loop end (Melodee), BAR/BEAT (GrooveOS) | **Phrase quantize, and a choice of next bar or loop end** |
| R9 alternates per role | Melodee 8, X0X 32, Move 6 | **Exclusive per-role clips on keys** |
| R10 reassign keys | None | **A user key map** |
| R11 split keys | None | **A launch zone and a play zone at the same time** |
| R12 external keyboard | Notes per channel everywhere; PC launch in zp12 | **External keyboard plays live while the FM-1 keys launch** |
| R13 song save | Common; slots and formats are fragile | **More slots, stable format, file export** |
| R14 performance recording | X0X (bars + mutes), Melodee (jam → song) | **One timeline of launches, mutes, FX and knobs** |

## Implications

- Building on Felucca or SLOOP gets us a proven HAL, the OTA loader, a web installer, a WASM emulator, host tests, and a sequencer that runs in the ISR.
- **A clip system has to break the `track_t` ↔ `step[]` coupling** by adding a clip pool indexed by track. `song_chain.c` already does the RAM-copy-then-index-switch that queued clip switching needs.
- Patterns worth borrowing:
  - X0X: per-part cueing, song record, and help cards.
  - Melodee: queueing to each track's loop end.
  - GrooveOS: scene capture and the LAUNCH setting.
  - FM1 Move: key-light states.
