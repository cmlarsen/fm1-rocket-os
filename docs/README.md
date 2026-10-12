# Project brain

This folder is the project's memory: linked markdown documents that are the source of truth for decisions and knowledge. Start here and follow the links.

## Product: what we're building
- [Requirements](product/requirements.md): the owner's brief as numbered requirements (R1–R15, D1–D4)
- [UX principles](product/ux-principles.md): rules every screen and gesture must follow
- [Interaction model (draft v1)](product/interaction-model.md): views, key layouts, gestures and LED states
- [Screen inventory (design v1)](product/screens.md): all screens on the review canvas, plus the design decisions made while drawing them
- [Roadmap](product/roadmap.md): milestones M0–M7

## Engineering: how we build it
- [Architecture](engineering/architecture.md): layers, the `plat.h` boundary, clip engine and memory budgets
- [Agentic dev loop](engineering/agentic-dev.md): test pyramid, scenario scripts and the per-change loop
- [Screen design workflow](engineering/screen-design.md): from mockup to golden PNG at 1:1 pixels
- [Build plan](engineering/build-plan.md): orchestrated child-agent waves W0–W4 with owner gates (run 2026-10-10 to 2026-10-12)

## Lessons
- [FM-1 firmware playbook](fm1-firmware-playbook.md): what this project learned, for the next FM-1 firmware on any base (platform facts, safety gates, ladder, orchestration, starter kit)

## Decisions
- [ADR-0001: Fork an existing GPL-3.0 firmware](decisions/0001-base-platform.md) (accepted; X0X proposed)
- [ADR-0002: device code in C99](decisions/0002-language-c.md) (proposed)
- [ADR-0003: Keep FM sound design, off the performance path](decisions/0003-sound-design-scope.md) (accepted)
- [ADR-0004: Remove Classic patterns, delay 0.75 s](decisions/0004-remove-classic-shorten-delay.md) (accepted)

## Research (snapshot as of 2026-10-10)
- [FM-1 hardware, toolchain and flashing](research/fm1-hardware.md)
- [Firmware landscape](research/firmware-landscape.md): more than 30 firmwares compared, with a gap analysis
- [GrooveOS deep dive](research/grooveos.md): the closest existing product, mapped against our requirements
- [Community sentiment](research/community-sentiment.md): what owners call easy or hard to use
- [Clip-launch UX](research/clip-launch-ux.md): Launchpad, Ableton, Deluge, Elektron, OP-Z and others
- [Dev tooling prior art](research/dev-tooling.md): emulators, host runners, screen and audio goldens
- [Fork evaluation](research/fork-evaluation.md): code-level ranking of the candidates; recommends forking X0X, with FM6 ported from Felucca

## Open questions
- [Open questions](open-questions.md): owner decisions, hardware facts to verify, tooling

## Glossary
- **STUDIO** — the build mode (after boot): make the song's roles, clips and sounds.
- **LIVE** — the performance mode (long-press PLAY): launch clips, play, punch FX, record the take.
- **layer (PADS / PLAY / FX)** — what the keys do in LIVE: PADS launches clips, PLAY plays the focused role, FX makes them punch effects and role mutes. SEL toggles PADS and PLAY; FX is held or double-tapped.
- **role** — one part of a song (DRUMS, BASS, LEAD, CHORDS, FX), with its own sound, clips and MIDI channel.
- **clip** — a short loop or one-shot of notes (or drum hits) plus knob motion, belonging to one role.
- **song** — the whole thing: its roles and their clips, one take, and its settings.
- **take** — the most recent LIVE recording, stored as an event log; a song keeps one.
- **launch** — start a clip; it replaces whatever that role was already playing.
- **quantize** — when a launch or stop lands: the next bar (default), the next beat, or the end of the clip.
- **stage lock** — a per-song option that blocks clearing a clip while the transport plays.
- **punch FX** — six performance effects (repeat 1/8, repeat 1/16, tape stop, filter dive, echo throw, freeze); tap to latch, hold for momentary.
- **motion lane** — a recorded knob move stored on a clip; HOME + that knob clears it on the pages that have lanes (the sound pages and the clip MOTION page).

## Conventions
- **One topic per file.** Link related files instead of duplicating content.
- **Research files are dated snapshots.** The FM-1 scene changes daily, so recheck facts before relying on them.
- **Decisions go in `decisions/NNNN-title.md`** (ADR format: status, decision, why, alternatives, consequences).
- **When a question is answered,** move the answer into the relevant doc and delete it from `open-questions.md`.
- **Cite sources inline.** Mark anything unverified.
