# UI lane review

Reviewed 2026-10-11, review-only. Scope: product paths in `firmware/src/app/ui.c`, `ui_studio.c`, `ui_live.c`, `ui_live_setup.c`, `ui_clip.c`, `ui_songs.c`, `ui_name.c`, `ui_help.c`, `panel.c`, and `editor.c`. Read CLAUDE.md, the glossary, architecture invariants, and interaction-model.md first. No product code, tests, or docs changed; no hardware flashed.

**18 findings: BUG 10, EDGE 4, SIMPLIFY 1, CLARITY 2, MAINTAIN 1. Severity: high 2, medium 14, low 2.** Top three: a clear hold survives HOME/view/mode exits; REC help can erase the take; PLAY help starts transport or switches modes.

## Evidence and limits

Built the native host with `sh host/build_host.sh`. Ran throwaway scenarios under `/tmp/ui-review` using `build/host/x0x_host SCENARIO /tmp/ui-review`; their important input sequences and failed expectations are quoted below, so the report does not depend on those temporary files. All findings marked **ran** were reproduced against the unmodified app. Static findings are explicitly marked. Hardware timing and concurrent ISR interleavings were not exercised.

Passing checks: `make unit-name unit-songs` (including ASan/UBSan), `sh tests/fuzz/run_fuzz.sh 20 500`, and `make screens-lint` (1,585 frames / 3,224 canvases, zero findings). Existing scenarios `help_cards`, `studio_clear_abort`, `studio_cliprec_at`, `live_fx`, `songs_delete`, and `tlset_studio_keys` also passed. Additional LIVE SETUP layout lint passed. An extreme NAME case visibly clipped; the generic lint also flags off-band SONGS draws, which are deliberately banded, so those were excluded as false positives.

## Findings, ranked

### 1. BUG — high — clear continues after its modifier or context disappears

**Location:** `firmware/src/app/ui_studio.c:224`, `firmware/src/app/ui.c:1574`, `firmware/src/app/ui.c:4083`.

After the 250 ms transition, the clear checks only SEL or the pad: `int held = st.clear_sel ? ... : ((ui.keys >> st.clear_key) & 1u) != 0;`. It neither requires HOME for HOME+pad clear nor requires the STUDIO view. `set_view()` does not cancel `st.clear_clip`, and `studio_tick()` runs in every view, including LIVE. A destructive gesture therefore survives the user abandoning its modifier, opening another page, or entering LIVE.

**Failure / repro (ran):** `wait 300; press HOME; key 0 down; wait 350; release HOME; wait 1300; expect clipused0 1` fails with **0**. Replacing HOME release with `press PLAY; wait 700` enters LIVE, then another `wait 600` deletes the clip there. Independently, `press SEL; wait 350; tap EDIT; wait 1300` deletes the cursor clip on the sound page.

**Fix:** cancel clear/tap/copy state on every STUDIO exit; require both HOME and the source pad throughout a HOME clear; recheck stage lock before committing. Consume the initiating gesture until all its inputs are released.

### 2. BUG — high — showing REC help can clear the take

**Location:** `firmware/src/app/ui_live.c:125`, `firmware/src/app/ui.c:2416`, `firmware/src/app/ui_help.c:180`.

`live_rec_tick()` runs before `help_glo_press()` and ignores both GLO and `ui.btn_used`. The card suppresses the normal button handler, but does not cancel REC's independent timer. Looking up how to clear a take can itself clear it; a short help press also arms recording.

**Failure / repro (ran):** enter LIVE (`press PLAY; wait 700; release PLAY`), `tap REC; tap PLAY; wait-bar 1; tapkey 7; wait-bar 1; tap REC; wait-bar 1`. `expect takehas 1` passes. Then `press GLO; wait 60; press REC; wait 1600; expect takehas 1` fails with **0**. A short GLO+REC press/release instead makes `expect takearmed 0` fail with **1** (the bridge's arm-at-start flag is outside `takestate`).

**Fix:** resolve help ownership before feeding REC edges; cancel its pending gesture when the card claims REC, including a previously started press. Keep it suppressed through release.

### 3. BUG — medium — PLAY and FX help still perform their actions

**Location:** `firmware/src/app/ui_live.c:245`, `firmware/src/app/ui_live.c:476`, `firmware/src/app/ui.c:2414`.

PLAY's timer precedes help interception and ignores consumed-button state. FX's edge detector later reads the physical held button, likewise ignoring help ownership. The card is shown while the requested button also controls the instrument.

**Failure / repro (ran):** from boot, `wait 300; press GLO; wait 60; tap PLAY; expect liveplaying 0` fails with **1**. Holding PLAY 700 ms instead makes `expect mode 0` fail with **1**. In LIVE, `press GLO; wait 60; press FX; wait 60; expect fxlayer 0` fails with **1**.

**Fix:** give each press a single owner before all timers/edge detectors, and make the help claim suppress both timer completion and release actions.

### 4. BUG — medium — FX keys use chromatic indexes instead of physical lower/upper keys

**Location:** `firmware/src/app/ui_live.c:347`, `firmware/src/app/ui_live.c:537`.

The documented six lower keys are tested as raw keys `0..5`, and upper role-mute keys as raw `16..20`. Raw key indexes are chromatic: the existing `WHITE_KEY[]` and `BLACK_KEY[]` tables show lower indexes 0,2,4,6,7,9... and upper indexes 1,3,5,8,10.... Consequently lower G3 triggers tape stop rather than repeat 1/16, and upper F#3 punches repeat 1/16 rather than muting DRUMS. LEDs repeat the incorrect mapping.

**Failure / repro (ran):** enter LIVE, hold FX, `tapkey w1; expect fxlatch1 1` fails with **0**; `tapkey b0; expect fxmute0 1` fails with **0**.

**Fix:** translate physical lower and upper positions with WHITE_KEY/BLACK_KEY in both input and LED paths; use named physical keys in scenarios.

### 5. BUG — medium — HOME+FX does not reset role mutes

**Location:** `firmware/src/app/ui_live.c:483`.

The interaction model says HOME+FX resets everything, including role mutes. The handler calls only `engine_perform_reset()`; `live_bridge_mute_all(0)` is present on mode exit but absent here. A supposedly reset instrument can keep a role silent.

**Failure / repro (ran):** enter LIVE, `press FX; wait 60; tapkey 16; release FX; press HOME; press FX; wait 60; expect fxmute0 0` fails with **1** (raw key 16 is the current implementation's DRUMS mute).

**Fix:** reset mutes with the effects and cancel held FX ownership. Track the reset press as down until release: setting `fx_held = 0` while FX remains physically down currently repeats the reset every input frame.

### 6. BUG — medium — entering the FX layer retains PLAY notes

**Location:** `firmware/src/app/ui_live.c:480`, `firmware/src/app/ui_live.c:497`.

Opening or latching the FX layer changes what the keys do without calling `note_release_all()`. Held panel and MIDI notes remain sounding across a layer change, contrary to the architecture invariant. This is not an indefinite-note claim: the eventual physical release still clears the panel table.

**Failure / repro (ran):** enter LIVE, `tap SEL; key 7 down; expect heldnotes 1; press FX; wait 60; expect heldnotes 0` fails with **1**. Releasing FX also leaves that note held.

**Fix:** release panel and MIDI notes on every FX layer transition, just as SEL and setup entry already do.

### 7. BUG — medium — cancelling a SEL clear launches the clip

**Location:** `firmware/src/app/ui_studio.c:180`, `firmware/src/app/ui_studio.c:227`, `firmware/src/app/ui_studio.c:356`.

Crossing the tap threshold starts a SEL clear but does not mark SEL used. Releasing before 1.5 s executes the ordinary SEL tap handler before the clear timer notices release. Cancelling the destructive hold thus queues a musical action.

**Failure / repro (ran):** `wait 300; press SEL; wait 500; release SEL; wait 60; expect liveque0 -1` fails with **0**. The clip is kept but queued to launch.

**Fix:** consume SEL as soon as its hold crosses 250 ms, whether the clear completes, is refused, or is cancelled.

### 8. BUG — medium — tempo gesture on sound/FX/MIX pages changes the dead sequencer

**Location:** `firmware/src/app/ui.c:2503`.

HOME+SELECT writes `seq.bpm` and marks the project dirty, while the clip engine owns the only active transport. It neither changes the song's tempo nor invokes the bridge tempo setter. The same file's R_TEMPO setter already uses the correct song/bridge path.

**Failure / repro (ran):** `wait 300; expect bpm 125; tap EDIT; press HOME; turn SELECT 5; expect bpm 130` fails with **125**. The gesture is consumed but the music's tempo is unchanged.

**Fix:** route through a shared song-tempo helper, matching STUDIO's HOME+PRESETS gesture and R_TEMPO; remove writes to inactive sequencer tempo from product paths.

### 9. BUG — medium — LIVE PLAY ignores the song key and scale

**Location:** `firmware/src/app/ui_live.c:359`, `firmware/src/app/ui_live.c:382`, `firmware/src/app/ui_live.c:754`.

PLAY uses `41 + k + 12 * live.octv`, and mapped Live note uses the same chromatic calculation. Neither reads `song_io_key()` or `song_io_scale()`, despite the model requiring playing in the song's scale. The PLAY card also omits the scale.

**Failure scenario (static, not run):** choose C major, enter LIVE PLAY, press raw key 1; it emits MIDI note 42 (F#2), outside C major. Changing to another key/scale has no effect on panel pitch mapping.

**Fix:** define and apply one key/scale mapping for PLAY and mapped Live note, and display its key/scale on the PLAY card. If chromatic playback is intended, resolve the product contract first.

### 10. EDGE — medium — recordings longer than four bars are shortened to two

**Location:** `firmware/src/app/ui_clip.c:367`.

The rounding chain handles 3–4 bars as four but handles every value above four with `else if (bars > 1) bars = 2;`. The following clamp `if (bars > 4)` can never catch those values. The longest recordings shrink to 32 steps instead of the 64-step cap, hiding/truncating their latter half in playback.

**Failure / repro (ran):** `wait 300; newsong 0; cursor 1 0; tap REC; wait-bar 5; tap REC; wait-bar 1; expect cliplen7 64` fails with **32**.

**Fix:** clamp to four before rounding to 1/2/4, or use a simple ordered threshold expression. Define wrap/overdub behavior when a pass exceeds the maximum.

### 11. EDGE — medium — notes started during record-close retain one-step lengths

**Location:** `firmware/src/app/ui_clip.c:380`, `firmware/src/app/ui_clip.c:461`, `firmware/src/app/ui_clip.c:510`.

REC close finalizes keys held at the instant REC is pressed. New keys are still recorded during the closing interval, but `cl_rec_finish()` does not finalize them. After the boundary it clears `rec.active`, so their later key-up no longer calls `cl_rec_note_off()`. The newly recorded notes retain the initial length of one.

**Failure / repro (ran):** `wait 300; newsong 0; cursor 1 0; tap REC; wait 300; tap REC; key 7 down; wait 2200; key 7 up; expect notelen7.0 >1` fails with **1**. The note was held through multiple steps and the close boundary.

**Fix:** finalize all outstanding recorded notes at the actual boundary before ending the session, using the quantized boundary as note-off. Preserve normal release handling throughout the closing interval.

### 12. EDGE — medium — SONGS help fabricates HOME release and exits the view

**Location:** `firmware/src/app/ui.c:2428`, `firmware/src/app/ui_songs.c:278`.

When help becomes active the shell passes `ev.btn = 0` into the songs reducer. If HOME was already down, the reducer sees an artificial HOME-up and closes SONGS. Suppression also produces artificial down edges when help closes; freezing actions must not invent physical edges.

**Failure / repro (ran):** `wait 300; press SAVE; wait 700; release SAVE; press HOME; wait 60; press GLO; wait 60; expect songson 1` fails with **0**. GLO+HOME is explicitly supported in either order by help, but asking for its card performs Back.

**Fix:** maintain real physical edge bookkeeping while modal, suppress actions rather than zeroing the button mask, and consume the help chord through release. Apply the same rule to the nested NAME reducer.

### 13. EDGE — medium — valid wide names exceed the 240 px screen

**Location:** `firmware/src/app/ui_name.c:404`, `firmware/src/app/ui_name.c:419`.

NAME accepts eleven characters but draws the whole field in AF_L starting at x=14, with no scrolling or width-based font selection. Eleven Ws extend far past the right edge, hiding the final characters and cursor.

**Failure / repro (ran and visually inspected):** create/open a song, enter SONGS → EDIT → Rename, hold HOME 1.6 s to clear, then repeat `turn ALGO 23; turn SELECT 1` eleven times. `wide_name.png` shows the name cut at the right edge; the log records AF_L `WWWWWWWWW` reaching x=260 already at nine characters (and later characters farther out). Ordinary NAME unit tests and the screen tour do not catch this.

**Fix:** scroll the text to keep the editing cursor visible, or select a smaller face when the full name exceeds the field width. Add a maximum-width-name render case, not only a maximum-character-count case.

### 14. BUG — medium — LIGHTS Off also disables LIVE transport/layer button LEDs

**Location:** `firmware/src/app/ui.c:3938`, `firmware/src/app/ui_live.c:531`.

`live_leds()` computes PLAY and SEL button bits, but is called inside `if (proj.set.keyled)`. With LIGHTS Off, the general path checks inactive `seq.playing`, so PLAY goes dark while clips play and SEL goes dark in the PLAY layer. The panel layer indication is lost with key lighting.

**Failure / repro (ran):** `wait 300; tap GLO; turn SELECT 8; turn ALGO -2; tap HOME; press PLAY; wait 700; release PLAY; tap SEL; tap PLAY; expect button SEL 1; expect button PLAY 1` fails with **0** for both.

**Fix:** compute transport/layer button state independently of the key-light preference; gate only the key/dim decoration on LIGHTS.

### 15. CLARITY — medium — HOME cheat sheet claims sound-page lanes that cannot be cleared

**Location:** `firmware/src/app/ui_help.c:102`, `firmware/src/app/ui.c:1608`.

The sheet exposes “K1-K4 touch clear knob lane” on V_PART/V_SOUND, but `motion_forget()` searches the removed Classic `proj.arr.lane` path. Its own comment says TCLS leaves no allocatable X0X lane. `motion_touch()` also returns immediately because `seq.playing` is always false on the product path. Sound-page REC instead toggles legacy `ui.rec`; it does not record a clip's sound lane.

**Failure scenario (static, not run):** open a role's sound page, hold HOME for the cheat sheet, follow its clear-lane instruction; the handler answers “NO MOTION ON ...” and cannot reach a song clip's lane. This is a help/handler mismatch, not evidence of deleting the wrong lane.

**Fix:** hide that sheet row on unsupported sound pages, or implement and document the intended clip-owned sound-motion path. Keep it on V_CLIPMOTION, where the handler actually clears the clip lane.

### 16. SIMPLIFY — medium — remove unreachable Classic UI machinery from ui.c

**Location:** `firmware/src/app/ui.c:961`, `firmware/src/app/ui.c:1208`, `firmware/src/app/ui.c:1645`, `firmware/src/app/ui.c:2171`.

The 4,111-line file still builds Classic page variants, defines pattern clear/copy and generator actions, retains p-lock and step-entry helpers, draws old HOME/SONG/SEQ/GEN screens, and includes a second B_ARP switch case after the unconditional early B_ARP return. These states are intentionally unreachable after TCLS, but obscure which mutations are still legal and make audits/search results misleading.

**Maintenance scenario (static):** a developer adjusts “SEQ” or “HOME” behavior in the old switch/page branch and sees no product change, or accidentally reconnects it to pattern writes. This is not a current crash claim.

**Fix:** delete unreachable product UI branches in small verified steps without moving inherited DSP/HAL files. Separate reachable inherited sound/FX/MIX/GLOBAL page adaptation, transport/save coordination, and rendering helpers into include-style modules or explicit APIs; preserve existing host scenario coverage during the split.

### 17. CLARITY — low — NAME promises twelve characters but accepts eleven

**Location:** `firmware/src/app/ui_name.c:390`.

The displayed suffix is hardcoded `OF 12`, while NAME_MAX is eleven (`SONG_NAME_LEN - 1`). A full name displays “11 OF 12” and the user cannot type the promised last character.

**Failure scenario (static, visible in the wide-name repro):** fill eleven characters; the twelfth key does nothing while the counter implies capacity remains.

**Fix:** generate the counter from NAME_MAX rather than a separate literal; keep the model's terminator outside the displayed character capacity.

### 18. MAINTAIN — low — cross-module gesture ownership and exits have no common contract

**Location:** `firmware/src/app/ui.c:95`, `firmware/src/app/ui.c:1574`, `firmware/src/app/ui.c:2410`, `firmware/src/app/ui_live.c:506`.

Include-style modules share `ui`, `btn_used`, physical prior-state masks, and independent timers. The shared dispatcher reads physical input, several gesture machines run before it, and setup consumes some encoders before LIVE consumes the rest. Only held notes have a universal exit rule; gesture cleanup is scattered among individual callers. Findings 1–3 and 12 demonstrate the resulting gaps.

**Maintenance scenario (static):** add an overlay or a new view exit; notes are released by set_view, but an independent timer or synthetic release still commits an action in the next context.

**Fix:** introduce a small main-loop gesture ownership/cancellation contract and per-view leave hook, not a heap-backed framework. Feed each encoder once per frame into a snapshot; centralize “cancel pending tap/hold, keep release consumed” behavior. Have overlays and view transitions use that contract.

## Areas checked without additional findings

- Shared panel held-note release path, tracked role ownership, normal SEL/setup/view/mode releases, and the existing mono last-key release behavior. The short fuzz and existing note-related paths passed; FX layer entry is the explicit exception above.
- LIVE SETUP assignment enumeration, wrap over None, release-to-commit, no-change dirty suppression, and cancellation when leaving assignment; existing STUDIO key-map isolation scenario passed.
- Normal LIVE entry/exit keeps the clip transport, resets effects/mutes on exit, and defers take/song autosaves while LIVE or its mode ring is active. No in-LIVE flash write was found in the reviewed UI paths.
- SONGS normal transport suppression, delete hold cancellation, full-slot handling, operation failure UI, selection-release bookkeeping on normal exit, and NAME bounded insert/delete/multitap/repeat behavior. Native and sanitizer suites passed. This does not independently certify storage power-loss correctness, which belongs to the storage lane.
- Panel calibration bounds, timeout rollback, default loading, and main-loop watchdog servicing; editor frame minimum-length/header checks, bounded reply construction, unknown-command rejection, and removal of sample writers. No additional product-misuse finding; inherited HAL/loader internals were not reviewed.
- Standard 240×240 screen tour and LIVE SETUP text fitting. No additional overflow established outside the extreme NAME case; no hardware legibility claim.
- UI duration comparisons generally use unsigned subtraction or signed short-deadline differences, which tolerate 32-bit millisecond wrap for these short holds. No separate UI timer-wrap bug established. No ISR waiting, heap allocation, 64-bit division, or new double use found in the reviewed product paths; host-only reporting code is excluded.
