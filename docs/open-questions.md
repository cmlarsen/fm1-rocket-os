# Open questions

These questions are still unresolved. When one is answered, record the answer in the relevant doc or ADR, then delete it from this list.

## Resolved 2026-10-10
- **Licence.** GPL-3.0, open source. We fork; we don't build a base. See [ADR-0001](decisions/0001-base-platform.md).
- **FM sound design.** We keep the inherited engine and leave it out of the performance path. See [ADR-0003](decisions/0003-sound-design-scope.md).
- **Live playing.** A long press on PLAY enters LIVE, and SEL toggles between PADS and PLAY. See the [interaction model](product/interaction-model.md#live-mode).

## Resolved 2026-10-11 (T7a)
- **LIVE → STUDIO transport (was #6).** The song is the edited object and the clip engine is the
  only audio source in every normal view, so **playing clips carry on across mode changes**: LIVE
  entered from the song home keeps what is playing and leaving keeps it playing. The classic X0X
  pattern pages (GLOBAL → Classic patterns) hand the audio to X0X's sequencer instead; leaving
  them gives it back. See the [interaction model](product/interaction-model.md#classic-patterns).
  Extended by a coordinator decision for the owner to confirm.

## Resolved 2026-10-11 (plan review)
- **Build host.** deltron.local: x86_64, Ubuntu 26.04, glibc 2.43, 12 cores, 30 GB RAM, 49 GB free, with Docker, gcc, rsync and qemu. SSH works as `caleb`. KVM doesn't speed up the pi32v2 emulator, because QEMU has to emulate the instructions in software.
- **Classic patterns removed, delay 0.75 s** (owner, 2026-10-11): [ADR-0004](decisions/0004-remove-classic-shorten-delay.md). Frees about 39 KB of RAM pool; built in TCLS.
- **The LIVE layer toggle is the SEL button** (owner, 2026-10-11): use the panel's printed label and X0X's name (`B_SEL`) everywhere. SEL confirms in menus and toggles the PADS/PLAY layers in LIVE. Only a long press on PLAY leaves LIVE. The SCL wording in docs, help cards and the LIVE footer is renamed in TPOL.
- **One take per song** in v1.
- **Save model.** One state per song, autosaved while stopped, and undo is the safety net.
- **Scope cuts:** quantize is Bar, Beat or End; modes are Loop or Once; six punch FX; one pad map; no scenes. The full list is in [build-plan](engineering/build-plan.md#deferred-past-v1).

## Resolved 2026-10-11 (T8 review)
- **The song's key and scale.** `song_t` gained `key` (0–11) and `scale` (`SCALE_*`) in the former `rsv[2]`, validated on load; the New-song screen stores them and the Starter's chords follow progressions in them. See [model](engineering/model.md).

## Owner decisions
1. **Confirm the fork target: X0X.** It is recommended, and FM6, NOISE and the newer gfx would be ported from Felucca. See [fork evaluation](research/fork-evaluation.md).
2. **Default pad map.** The proposal is keyboard zones (see [screens](product/screens.md#design-decisions-made-while-drawing)). It is waiting for the owner's review on the design canvas.
3. **Committing LIVE tweaks.** Should LIVE offer "commit tweaks to song", or should LIVE changes always revert? The v1 default is revert.
5. **Name.** Is "rocket-os" the working name?

## Hardware facts to verify
1. **Key velocity.** The GrooveOS manual says yes; Felucca's HAL says the keys are switches only ([grooveos](research/grooveos.md#conflicting-facts-to-resolve)).
2. **Second CPU core.** Is it usable? Melodee claims both cores, while the stock firmware uses one.
3. **CPU headroom on the device** with 5 roles plus FM6.
4. **TRS MIDI IN adapter type.** Is it type A or type B?
5. **USB host.** Is it possible at all? Every source so far says no.
6. **Audio codec.** Is it the internal ADDA or an external codec?

## Tooling
1. **QEMU binary gate.** Does fm1-emulator run well enough on deltron.local to serve as the binary-level gate? T1 will show.
2. **CI.** Deferred to RC3.

## RC1 CPU headroom (2026-10-11)
- Full LIVE stress with 4 FM6 voices runs at about 77% CPU and trips the overload guard, which then holds FM6 to 2 voices. Is a lower FM6 voice count acceptable if optimisation can't win headroom back? See [RC1 results](reviews/2026-10-11-rc1-device-results.md).

## Core-1 data integrity with FM6 offloaded (2026-10-11, T5perf review)
X0X's core-1 check word covers the job hand-off, not the data core 1 reads while it renders FM6. On a unit with a bad core 1, FM6 could click or hold a stuck-loud voice before the hand-off check trips the single-core fallback. Memory isn't corrupted: no pointers are loaded from that data. The 808 has the same exposure. Only a healthy unit has been measured. If owners report FM6 glitches, offer a GLOBAL option to keep FM6 on core 0.
