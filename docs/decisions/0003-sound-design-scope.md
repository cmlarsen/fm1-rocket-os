# ADR-0003: Keep FM sound design from day one, but leave it out of the performance path

- **Status:** Accepted, 2026-10-10.
- **Context:** The owner wants to keep FM sound design as long as it doesn't make the firmware harder to use. Community research shows a real split: many owners want the FM synth kept, and many complaints are about menu-deep sound editing ([sentiment](../research/community-sentiment.md#who-prefers-what)).

## Decision
- **We get it for free.** The fork already ships the FM6 (msfa/DX7) engine, its editing pages and its presets ([ADR-0001](0001-base-platform.md)). We keep all of it as is. Removing it would cost work, and so would adding it back later.
- **Simple by default.** A role's sound is chosen by **browsing presets**, filtered by role type. Everyday use never requires opening an editor.
- **Deep editing stays one step away.** EDIT, ENV and LFO open the existing sound pages for the focused role. LIVE never opens them, and nothing in the performance flow leads into them.
- **Songs own their sounds.** Editing a sound changes only that song's copy.
- **No new sound-design work until after M5 (takes).** That deferred work covers things like new engines, a sound-design UX overhaul, or a web patch editor.

## Why not defer FM entirely?
Removing an engine that the fork already integrates would mean extra work now, and adding it back later would mean more extra work. The complexity cost comes from the *UI paths*, not from the engine. We keep the engine and fence off the UI paths.

## Consequences
- CPU and flash budgets have to cover the inherited engines. If the CPU budget can't fit N roles plus FM6, we may drop or lazy-load engines we don't use.
- Screen work in M2–M5 concentrates on LIVE, CLIP and SONG. Sound pages keep the fork's look until the polish milestone.
