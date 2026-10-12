# UX principles

These rules apply to every interaction, screen and LED in the firmware. Each rule comes from evidence in the research, and the source is linked after it. Check designs against this page. When a design breaks a rule, record the reason in an ADR.

Related: [requirements](requirements.md) · [interaction model](interaction-model.md) · [community sentiment](../research/community-sentiment.md) · [clip-launch UX](../research/clip-launch-ux.md) · [GrooveOS lessons](../research/grooveos.md#what-to-learn)

## 1. One mental model: Song → Roles → Clips
- **A song holds a few roles** (Drums, Bass, Lead, Noise/FX and so on). **Each role holds a few clips. One clip per role plays at a time.**
- This model is used by Launchpad iOS, Ableton, Deluge and Note ([clip-launch UX](../research/clip-launch-ux.md#1-novation-launchpad-for-ios-ampify)).
- **Never show the user a gap between in-memory state and saved state.** What they hear is the song. This was the most common complaint about SLOOP and stock ([sentiment §1](../research/community-sentiment.md#1-save-and-project-models-are-confusing-and-people-lose-work-the-strongest-theme)).

## 2. The default action is always safe
- In launch mode, an unmodified key press only launches or stops. It never edits.
- Destructive actions (clear, delete, overwrite) need **hold-to-confirm with a filling ring that names the target**. Releasing early cancels.
- Every destructive action can be undone, and a toast names what was undone ("UNDO: cleared Bass B").
- **Stage lock:** while playing, clearing from launch mode is blocked. These patterns come from GrooveOS, OP-Z and SLOOP.

## 3. Few combos, and no two-handed holds while performing
- There is no physical Shift key. **HOME acts as Shift** (hold) and as Back (tap), and the hold shows its own cheat sheet ([GrooveOS](../research/grooveos.md#control-model)).
- Any layer reached by holding a button can also be **latched** (double-tap), so performing never needs two hands held down ([sentiment §3](../research/community-sentiment.md#3-hidden-hold-combos-with-no-physical-shift-key)).
- **The same control does the same job on every screen.** Fixed roles: SELECT scrolls, ALGORITHM moves the cursor and sets values, SEL confirms, HOME goes back. This is Baud Girl's consistency rule, the most-praised UX trait. LIVE switches layers with SEL, so SEL keeps meaning "confirm".
- **Every gesture has one meaning.** HOME hold always means SHIFT and the cheat sheet. A long press on PLAY is the only way in or out of LIVE. Holding a pad means copy, or clear when combined with HOME.

## 4. The screen explains itself
- A **hint line** shows `CONTROL: VERB` pairs.
- **Long-press any button for a help card** (X0X, GrooveOS).
- Labels and colours match the knobs: K1–K4 are always drawn in the same four colours (SLOOP).
- Dimmed menu rows say why they are unavailable.

## 5. Readable at arm's length
- Use **large type**. The minimum text size will be set during the [screen design workflow](../engineering/screen-design.md) and enforced by lint.
- Prefer graphical views (grids, dots, bars) over dense text. Baud Girl's dot-matrix sequencer was loved, and her later tiny-text version drew backlash.
- **Every screen must be checked at 1:1 on 240×240 before it is accepted.** Mockups at 4× scale fool you, as the FM1 Quest developer found when "everything looked great in Figma".

## 6. Waiting looks intentional
- Launches and stops are **quantized to the next bar by default**. Beginners never have to choose a quantize value.
- Queued items **blink**. Playing items **pulse on the beat**. A **countdown** (bars, then beats) shows until a queued change lands.
- Tapping a queued item again cancels it.
- **Shift + launch = launch immediately.**

## 7. Recording is playing
- You start recording by playing, with no setup. An empty clip plus REC records from the next bar, and pressing again closes the loop.
- **Capture after the fact:** you can keep what you just played even if REC was off (GrooveOS, Melodee, Ableton Capture).
- Knob moves made while recording are stored. Each recorded knob lane can be seen and can be cleared on its own (modifier + touch the knob).

## 8. Saving is boring and reliable
- **One state per song.** The song on screen is the song. It autosaves while stopped and silent, because flash writes stall audio, and SAVE tap writes it immediately. There is no separate "saved copy" that can disagree with what you hear. Undo is the safety net.
- **An unsaved-changes dot is always visible.**
- Songs own their sounds, so editing a preset never changes an old song.
- The **save format is versioned**, and old songs always migrate forward.

## 9. The performance layer is non-destructive
- FX throws and knob tweaks in launch mode revert when you leave, unless they were recorded or committed (Polyend).
- Performance recordings are **event logs** of launches, mutes, FX and knob moves, never audio. In v1 each song keeps one take, and recording again replaces it (undoable).

## 10. Trust signals
- Use a one-click browser installer with a visible **"return to stock"** option.
- Never touch the stock data regions, so going back is clean.
- Run a browser emulator so people can try before they flash.
- Publish the test suite and a changelog. These answer the "vibe-coded" distrust ([sentiment §7](../research/community-sentiment.md#7-distrust-of-vibe-coded-firmware)).
