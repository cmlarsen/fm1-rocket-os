# T7a review (Opus): merge after fixes. Not safe to install yet.

Coordinator decisions are in **bold**. The reviewer's probe scenarios are in `/private/tmp/claude-502/-Users-caleb-CalebApps-fm1-rocket-os/10296272-f272-4eb6-9e65-5ca02d389a5d/scratchpad/probe/`. Turn the relevant ones into real scenarios.

1. **D1: a hung note when leaving LIVE** (`ui_live.c:70`). `live_leave` clears the held keys without sending note-offs. Send a note-off for every held key through `live_bridge_play_key(..., 0)`, then all-off for safety. Scenario: enter LIVE from the song home, switch to PLAY, hold a key, long-press PLAY, and check the `boff` follows.
2. **D2: the X0X editors leak outside Classic.** **Outside Classic, EDIT on the song home opens the cursor role's sound page in sound-only mode:**
   - map the role to `ui.part` (DRUMS→909, BASS→303A, LEAD→303B; for CHORDS/FM6 and FX/NOISE, a simple sound card with the T_RSV macro knobs);
   - the knobs edit the sound;
   - the keys play the role, as LIVE's PLAY layer does, and don't edit X0X patterns;
   - the step grid and pattern gestures are hidden or inert;
   - **HOME and SEQ on any X0X page outside Classic return to `V_STUDIO`.**
   Inside Classic, X0X behaves exactly as before. Add scenarios: EDIT from each role and back with HOME, and with SEQ; and no path from the song home reaches an X0X pattern editor without going through Classic.
3. **D3: the toasts are off the band** (`ui_studio.c:476`, y=96 against `BAND_H` 69). Draw the toast inside a band that's actually blitted (follow T2b's banding), so undo, redo, clear, copy and "can't undo" all show. Re-seed those goldens.
4. **D4: a SEL tap flashes the clear dialog** (`ui_studio.c:176`). Show the clear ring only after a 250 ms hold. A tap is just a launch.
5. **D5: `live_entry_stop` and `live_hold` weren't migrated.** Add the Classic entry (`tap GLO; tap SEL` or your helper), so `live_entry_stop` again proves that entering LIVE stops X0X's 303. Restore its intent, re-seed, and check the old shot's clips appear again.
6. **D6: card text.**
   - Use the clip's real mode (Loop or Once).
   - Show lengths under 16 steps as steps ("12 steps"), never "0 bars". Same in the ring (`:537`).
   - The left line becomes "<key> <scale> · 5 roles", using `song_t.key` and `scale`.
   - Give the header name 2 px of clearance from the transport icon.
7. **D7:**
   - draw the copy targets (empty cells blink or show `+` while copying, matching the keys);
   - clearing a playing clip stops that role, which releases its notes;
   - fix the report: HOME leaves Classic only if you implement it. **Do implement it:** HOME from any top-level X0X page inside Classic returns to the song home and leaves Classic.

Then run `make unit-core scenarios screens-lint parity check fw` and `tools/check_loader.sh build/x0x.fwsc`, list the re-seeded goldens with the reason for each, commit, update reports/T7a.md, and send worker_done.

## Re-review (round 2): one remaining defect
- **A hung note from a sound page.** Hold a key on the BASS sound page (or the CHORDS card), press HOME, then release: the release goes to the song home's handler, so no note-off is sent. The reviewer's probe is `/private/tmp/claude-502/-Users-caleb-CalebApps-fm1-rocket-os/10296272-f272-4eb6-9e65-5ca02d389a5d/scratchpad/probe/r2/snd_hang.x0x`.
- **Fix this class of bug once, with a coordinator decision:** add a single held-note table in the key path, mapping each key to what it started (engine and part, note). It is shared by LIVE PLAY, the sound pages and any future key-plays-a-note path.
  - Releasing a key always releases what that key started, whatever the current view or layer.
  - `set_view`, mode changes (STUDIO/LIVE/Classic) and HOME/SEQ handoffs release everything still held in the table.
  - Remove the per-view held tracking that this replaces, where that is safe.
- Add `snd_hang` as a scenario, plus a variant for each view change (SCL, EDIT→HOME, EDIT→SEQ, entering LIVE with a key held).
- **Cosmetic:** hide X0X's "P1" pattern marker in sound-page headers outside Classic.

Then run the full ladder and `tools/check_loader.sh`, commit, update reports/T7a.md, and send worker_done.
