# Product requirements

This is the source of truth for what the firmware must do. It is captured from the project owner's brief on 2026-10-10, and the wording is kept close to the original. Change it only when the owner changes the brief.

Related: [UX principles](ux-principles.md) · [interaction model](interaction-model.md) · [GrooveOS gap analysis](../research/grooveos.md#our-requirements-vs-grooveos) · [roadmap](roadmap.md)

## Goal

Custom firmware for the M-VAVE FM-1 that turns it into a standalone clip-launching performance instrument. **It must be very user friendly.** Creating songs, creating sequencer tracks and clearing tracks should all be obvious.

## Functional requirements

| ID | Requirement |
|---|---|
| R1 | Create and save multiple sequences on the FM-1 without needing a DAW. |
| R2 | Support several kinds of sequence: drum/beat patterns, bass lines, lead lines, noise/effect patterns, and other synth sequences. |
| R3 | Allow several sequences to run at the same time. |
| R4 | Provide a dedicated performance / launch mode. |
| R5 | Map sequences or clips directly to the FM-1's keys. Pressing a key triggers the assigned sequence. |
| R6 | Support looping clips, which continue until stopped or replaced. |
| R7 | Support one-shot clips, which play once and stop. |
| R8 | Quantize clip changes. A new loop can be selected early and still start cleanly on the next beat, bar or phrase boundary. |
| R9 | Allow several alternative clips for the same musical role (Beat A/B/C, Bass A/B, Lead A/B) and switch between them live while everything stays in sync. |
| R10 | Allow clips and sequences to be assigned or reassigned to specific keys. |
| R11 | Allow some keys to stay available for live note playing while other keys act as clip launchers. |
| R12 | Ideally, support an external MIDI keyboard for live playing while the FM-1's own keyboard handles clip launching and performance controls. |
| R13 | Save the whole setup as a song/project, so clip assignments, sequences, sounds and performance configuration can be recalled later. |
| R14 | Record a performance made in launch mode, including any live knob, filter or effect changes. |
| R15 | Make creating new songs, creating new sequencer tracks and clearing tracks easy. |

## Development requirements

| ID | Requirement |
|---|---|
| D1 | A browser and host emulator, so UI and UX can be tested and iterated without repeatedly flashing firmware. |
| D2 | An agentic development setup that lets Claude build and verify changes confidently with no human in the loop. |
| D3 | Design tooling for iterating on screens. Mockups should map 1:1 to the 240×240 display. |
| D4 | Documentation lives in this repo as linked markdown files that act as the project's brain and memory. |

## Hardware-driven notes

- **R12:** the FM-1 is a USB *device*, not a host. An external keyboard has to connect through the TRS MIDI IN jack, or through a computer or USB host box. See [hardware](../research/fm1-hardware.md#5-constraints-for-a-clip-launcher-firmware).
- **R14:** record the performance as an **event log** (launches plus parameter changes), not as audio. The device has only about 1 MiB of flash.
