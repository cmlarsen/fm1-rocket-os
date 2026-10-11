# Community sentiment: what "easy to use" means on the FM-1

What owners say about the stock firmware and the custom firmwares, with a focus on ease of use. Researched 2026-10-10.

Nearly all of the usable evidence comes from **r/MVaveFM1**: all 513 posts and 5,649 comments from 2026-07-25 to 2026-10-11, read through the Arctic Shift archive. That subreddit is dominated by enthusiasts who own several units, and all of it falls in a two-week hype window. Several "best UX" verdicts were formed in browser emulators, not on hardware.

Related: [firmware landscape](firmware-landscape.md) · [UX principles](../product/ux-principles.md) · [GrooveOS](grooveos.md)

## Evidence coverage

| Source | Coverage |
|---|---|
| r/MVaveFM1 | 513 posts, 5,649 comments; quotes checked against the archive |
| r/synthesizers | 6 of about 60 threads read |
| Elektronauts | 253 posts, mostly from the stock era; the forum disallowed custom-firmware discussion |
| Synth Anatomy comments, Synthtopia, Sonicstate, dreyandersson.com | Read; some quotes are fetch excerpts |
| Hacker News | Excerpts only |
| Gearspace, YouTube comments, Amazon, AliExpress | Unreachable |
| ModWiggler, llllllll, KVR, r/synthdiy | No threads found |

## Who prefers what

There is no consensus. People pick a firmware by use case.

- **Easiest: stock and Baud Girl.**
  - "stock and baudgirl are simple… Once you move over to any of the other firmwares… things get a lot more complicated and a little overwhelming for a beginner." ([1x00cov](https://www.reddit.com/r/MVaveFM1/comments/1x00cov/))
  - Baud Girl v96: "The UI is just so well thought out and intuitive… The documentation is a thing of beauty." ([1x26n5q](https://www.reddit.com/r/MVaveFM1/comments/1x26n5q/))
  - Baud Girl is also seen as the safest to flash, because it patches V15.
- **The friendliest groovebox: SLOOP.**
  - "full song created in just 5 min… including figuring out how it works for the first time" ([1x0aoff](https://www.reddit.com/r/MVaveFM1/comments/1x0aoff/))
  - One user returned an Ableton Move in favour of it: "does 85% of what the Move did… You WILL need reading glasses." ([1wz61hw](https://www.reddit.com/r/MVaveFM1/comments/1wz61hw/))
- **GrooveOS** was rated "best UX… only one actually working for multi-track live" ([1wxo6i8](https://www.reddit.com/r/MVaveFM1/comments/1wxo6i8/)). That verdict came from 2 minutes in its emulator, and its price drew heavy backlash.
- **Most powerful: Felucca and its forks.** People call it "Powerful but broad". Some find it "quite unintuitive" ([1wycpkh](https://www.reddit.com/r/MVaveFM1/comments/1wycpkh/)).
- **The "mega firmware" wish** (highly upvoted): "a consistent, polished firmware like Baud Girl's, the multiple engines… of Felucca, and an intuitive, well-designed interface like Groove OS's." ([1wxo6i8](https://www.reddit.com/r/MVaveFM1/comments/1wxo6i8/))
- **Synth vs groovebox split.** "the goal for most cfw is to make it an unusably tiny groovebox instead of a capable FM synthesizer." ([1wy98wr](https://www.reddit.com/r/MVaveFM1/comments/1wy98wr/), 56 pts) Many users run a different firmware on each of several FM-1s.
- **[dreyandersson ranking](https://dreyandersson.com/blog/m-vave-fm-1-custom-firmware/):** Felucca 79, SLOOP 79, Baud Girl 77, GrooveOS 74, X0X 71.

## Complaints, ranked by how often they came up

### 1. Save and project models are confusing, and people lose work (the strongest theme)
- **Stock:** pressing SEQ throws away unsaved patch edits. "It took me ages to figure that out 🥲" ([1wi42yi](https://www.reddit.com/r/MVaveFM1/comments/1wi42yi/)). Transpose and sync settings don't persist.
- **SLOOP:** "it took me a while to figure out the 'in memory' loop vs saving said loop to various slots. That was an annoying several hours… missing the basic navigation tools makes all those features less useful." ([1x23rx0](https://www.reddit.com/r/MVaveFM1/comments/1x23rx0/)) There is also "When I try to launch a brand new project, hoping for a blank slate, I just can't do it."
- **SLOOP:** "u gotta press the save button while on a different menu" ([1x2i5b3](https://www.reddit.com/r/MVaveFM1/comments/1x2i5b3/)).
- **Melodee:** "i ended up losing that melody since it autosaved." ([1x2ikpu](https://www.reddit.com/r/MVaveFM1/comments/1x2ikpu/)) The developer is adding an autosave toggle.
- **Requests:** "I wish you could store projects." ([1wyp6tn](https://www.reddit.com/r/MVaveFM1/comments/1wyp6tn/))

### 2. Clearing is hard
- **SLOOP:** "I would like pattern D to read EMPTY… even the manual doesn't cover this." Users replying gave contradictory answers ([1x0ui56](https://www.reddit.com/r/MVaveFM1/comments/1x0ui56/)).
- **Felucca:** "I can't figure out how to delete steps or clear patterns." The answer: go to the 4/4 tab of SAVE, turn KNOB 1, then press OCT+ ([1wycpkh](https://www.reddit.com/r/MVaveFM1/comments/1wycpkh/)).
- **SLOOP:** nobody knows how to clear only the automation.
- **Baud Girl:** clearing a step takes 3 actions. A user asked: "can we hold REC to clear step."
- **FM1 Move:** clearing a track takes five steps (SAVE, SELECT, ALGORITHM, REC, REC) and "Can't be undone."

### 3. Hidden hold combos, with no physical Shift key
- "by holding Shift... what do you actually mean (as there is no Shift button on FM1)?" ([1x0d2nh](https://www.reddit.com/r/MVaveFM1/comments/1x0d2nh/))
- SLOOP p-locks need two buttons plus a knob at once.
- **Two-handed holds get in the way of live play.** Users asked for latching layers ([1wycpkh](https://www.reddit.com/r/MVaveFM1/comments/1wycpkh/), [1wz52eq](https://www.reddit.com/r/MVaveFM1/comments/1wz52eq/)). Felucca added double-tap to latch, and SLOOP added hold plus HOME to lock.
- **Inconsistent navigation:** "on some pages, use octave +/-; on others, use Select" ([1x0umc8](https://www.reddit.com/r/MVaveFM1/comments/1x0umc8/)).
- **Stuck modes:** after a transpose combo, the ALGORITHM knob does only one thing until a reboot.
- Deep menus make sound design fiddly.

### 4. Tiny text and legibility
- **Baud Girl v96 backlash:** "I cannot read the tiny text at all… the 64 dot matrix sequencer in v93 was such a brilliant way of displaying legible information." ([1wyuf6s](https://www.reddit.com/r/MVaveFM1/comments/1wyuf6s/)) Baud Girl brought the old view back as a "Flow sequencer".
- **A developer's warning:** "everything looked great in Figma and in the web simulator, but on the physical FM1 screen, a lot of stuff ended up too small." ([1wzlwlt](https://www.reddit.com/r/MVaveFM1/comments/1wzlwlt/))
- **Felucca:** value animations are unnecessary, knobs lack acceleration, and steps get lost when turning quickly. Its issue tracker has reports of LEDs flashing unexpectedly.
- **Stock:** the button labels can't be read when the buttons are unlit.

### 5. Documentation
- The stock manual is about 90% complete, and the community wrote its own.
- One person "put the manual into ChatGPT" to learn it.
- Users ask for on-device help: "Because I'm always getting lost."

### 6. Flashing and fear of bricking
- **"Brick" shows up in 9 posts and 107 comments.**
- Felucca 1.0–1.0.3 soft-locked units that were updated from 0.9 ([1wyg26v](https://www.reddit.com/r/MVaveFM1/comments/1wyg26v/), fixed in 1.0.3.1).
- **Folk wisdom:**
  - Use a good cable.
  - Go back to stock between custom firmwares.
  - Close other MIDI apps.
  - Windows fails most often.
- **Hard bricks** need an RP2040 or ESP32 dongle and a guide full of hex addresses.
- **Each firmware's data format wipes your work** when you switch firmwares.
- **Installers that need a terminal** put people off.

### 7. Distrust of "vibe-coded" firmware
- People say: "AFAIK, all of them are vibecoded" (HN).
- Users ask authors to disclose AI use, and some advise choosing firmware by "people who were already coding before AI" ([1x2jipv](https://www.reddit.com/r/MVaveFM1/comments/1x2jipv/)).
- Visible quality signals matter here: tests, a changelog, and few regressions.

## What made things feel intuitive

- **Consistency and few combos.** Baud Girl's philosophy: "less reliance on shortcut button and knob combos… Consistency is important to make something easy to use." SELECT always scrolls vertically and ALGORITHM always sets values. "hold SEL and turn PRESETS to change patterns, anywhere" ([1wyuf6s](https://www.reddit.com/r/MVaveFM1/comments/1wyuf6s/), 174 pts).
- **Every screen is a list**, six rows at a time, with all four knobs live everywhere (Synth Anatomy).
- **A glanceable, graphical sequencer.** The dot matrix "showed me how to read a step sequencer."
- **On-screen colour that matches each knob** (SLOOP).
- **Recording by playing, with no setup.** SLOOP promises "Your first beat in 60 seconds". Melodee's "hold REC and the last bars you played… become its pattern".
- **Undo makes clearing feel safe.** SLOOP: hold REC for 2 s to clear, and undo brings it back.
- **Hold to show a layer on the keys** (FM1 Move).
- **One-click browser installers with a "return to stock" button.** "The amount of friction that has been removed for non technical people should not be underestimated." (Elektronauts #252) Browser emulators let people try before flashing.
- **Web editors** handle the deep editing.
- **Single-purpose firmwares are praised for being simple.**

## Feature requests that touch our scope

| Area | Ask |
|---|---|
| Projects | Several projects on the device; clearing or emptying slots; full backup per firmware; an autosave toggle |
| Sequencing | Fast step delete; knob-based note length; unquantized recording; copy and paste between sections |
| Song mode | Woovebox-style song mode; backing parts for live guitar ([1x2btv5](https://www.reddit.com/r/MVaveFM1/comments/1x2btv5/)) |
| **Clip launching** | Ableton/Force-style clips per track, each with its own patch. The answer given: "There isn't that I know of at present but that would be super cool." ([1x2jbl3](https://www.reddit.com/r/MVaveFM1/comments/1x2jbl3/)) |
| Nano-DAW | Use the keys to select, draw, move and resize notes; a desktop companion ([1x2gk7o](https://www.reddit.com/r/MVaveFM1/comments/1x2gk7o/)) |
| MIDI keyboard | External keyboard drives the arpeggiator; BLE MIDI host; sustain pedal |
| Automation | Record knob moves (Felucca MOTION "simply incredible") and clear them |
| Live performance | Latching layers instead of held buttons |
| Legibility | A large-font option; a persistent preset indicator |
| Scales | Chord and scale mode |

## Comparisons with other gear

- **OP-1** is the aspiration: "a poor man's OP1".
- **Ableton Move** is easy, and "only move has lead to finished things."
- **Woovebox** has a great song mode, but its LED screen is cryptic. Lesson: **labelled screen > cryptic LEDs**.
- **Pocket Operators** are the model for focused design.
- **Baud Girl's caution:** "A tiny screen and limited, awkward controls are simply too limited for the amount of UI a good sequencer UX really requires." Her answer is chaining short patterns instead of building long views.

## "User friendly" on this device, as users define it
1. The screen can be read at arm's length while jamming. Use big fonts and graphical views.
2. There are few multi-button combos, and performing never needs two hands held down.
3. Each control does the same thing on every screen.
4. Recording happens by playing, with no setup.
5. Save and clear are obvious and can be undone, and there is **no hidden distinction between in-memory and saved slots**.
6. A web editor handles deep editing.
7. Installing takes one click, with an obvious way back to stock.

## Sources
r/MVaveFM1 threads:
- [1wxo6i8](https://www.reddit.com/r/MVaveFM1/comments/1wxo6i8/) · [1wyjy3r](https://www.reddit.com/r/MVaveFM1/comments/1wyjy3r/) · [1wyuf6s](https://www.reddit.com/r/MVaveFM1/comments/1wyuf6s/) · [1wycpkh](https://www.reddit.com/r/MVaveFM1/comments/1wycpkh/) · [1wz52eq](https://www.reddit.com/r/MVaveFM1/comments/1wz52eq/) · [1wy98wr](https://www.reddit.com/r/MVaveFM1/comments/1wy98wr/)
- [1x0umc8](https://www.reddit.com/r/MVaveFM1/comments/1x0umc8/) · [1x23rx0](https://www.reddit.com/r/MVaveFM1/comments/1x23rx0/) · [1x0ui56](https://www.reddit.com/r/MVaveFM1/comments/1x0ui56/) · [1wyp6tn](https://www.reddit.com/r/MVaveFM1/comments/1wyp6tn/) · [1x2i5b3](https://www.reddit.com/r/MVaveFM1/comments/1x2i5b3/) · [1x0d2nh](https://www.reddit.com/r/MVaveFM1/comments/1x0d2nh/)
- [1wz61hw](https://www.reddit.com/r/MVaveFM1/comments/1wz61hw/) · [1x2i3y1](https://www.reddit.com/r/MVaveFM1/comments/1x2i3y1/) · [1x2ikpu](https://www.reddit.com/r/MVaveFM1/comments/1x2ikpu/) · [1wz5amr](https://www.reddit.com/r/MVaveFM1/comments/1wz5amr/) · [1wy49qw](https://www.reddit.com/r/MVaveFM1/comments/1wy49qw/) · [1x00cov](https://www.reddit.com/r/MVaveFM1/comments/1x00cov/)
- [1wi42yi](https://www.reddit.com/r/MVaveFM1/comments/1wi42yi/) · [1wyg26v](https://www.reddit.com/r/MVaveFM1/comments/1wyg26v/) · [1wze8s7](https://www.reddit.com/r/MVaveFM1/comments/1wze8s7/) · [1wz48t7](https://www.reddit.com/r/MVaveFM1/comments/1wz48t7/) · [1x03afk](https://www.reddit.com/r/MVaveFM1/comments/1x03afk/) · [1x2jbl3](https://www.reddit.com/r/MVaveFM1/comments/1x2jbl3/)
- [1x2gk7o](https://www.reddit.com/r/MVaveFM1/comments/1x2gk7o/) · [1x2btv5](https://www.reddit.com/r/MVaveFM1/comments/1x2btv5/) · [1x0tg95](https://www.reddit.com/r/MVaveFM1/comments/1x0tg95/) · [1x29hky](https://www.reddit.com/r/MVaveFM1/comments/1x29hky/) · [1x2plyy](https://www.reddit.com/r/MVaveFM1/comments/1x2plyy/) · [1wzlwlt](https://www.reddit.com/r/MVaveFM1/comments/1wzlwlt/)
- [1x2jipv](https://www.reddit.com/r/MVaveFM1/comments/1x2jipv/) · [1wta4aa](https://www.reddit.com/r/MVaveFM1/comments/1wta4aa/) · [1wt8sbq](https://www.reddit.com/r/MVaveFM1/comments/1wt8sbq/) · [1wr4ycx](https://www.reddit.com/r/MVaveFM1/comments/1wr4ycx/)

Other sources:
- r/synthesizers: [1wydl0q](https://www.reddit.com/r/synthesizers/comments/1wydl0q/) · [1wso26s](https://www.reddit.com/r/synthesizers/comments/1wso26s/) · [1wsazbq](https://www.reddit.com/r/synthesizers/comments/1wsazbq/)
- Forums: [Elektronauts](https://www.elektronauts.com/t/m-vave-fm-1/252170) · [Loopy Pro](https://forum.loopypro.com/discussion/68892/m-vave-fm-1-synth) · [HN](https://news.ycombinator.com/item?id=49994065)
- Press and rankings: [Synth Anatomy Baud Girl](https://synthanatomy.com/2026/09/baud-girl-fm-1-va-custom-m-vave-fm-1-firmware.html) · [Synthtopia](https://www.synthtopia.com/?p=155069) · [dreyandersson](https://dreyandersson.com/blog/m-vave-fm-1-custom-firmware/) · [fm1-editor.com](https://fm1-editor.com/firmware/)
