# T8 review (Opus): merge after these fixes

Coordinator decisions are in **bold**. First merge `integration` into your branch: `song_t` now has `key` (0–11) and `scale` (`SCALE_*`) in place of `rsv[2]`, and the validator checks them.

1. **Storage meter** (`ui_songs.c:461-464`): the bar runs through the "STORAGE" label. Follow the design: the bar and "N of 5" on one row, with no label.
2. **The templates must set `song_t.key` and `song_t.scale`** (`song_templates.c:124`, `:138`). Test: an A-minor Starter is stored as key 9 and `SCALE_MINOR`.
3. **The Starter must be musical, not flat.**
   - Chord clips use real progressions in the chosen key and scale: for example I–V–vi–IV in major, i–VI–III–VII in minor, one chord per bar, with a different progression or voicing per clip.
   - Drums have accents (the accent row) and some ghost hats.
   - The fill's crash lands on step 0 of the following bar, not on step 15. Put it on step 0 of the fill clip, so the crash marks the downbeat when the fill launches.
   - Keep it deterministic. Extend the unit tests to check that the chord roots follow the progression.
4. **Copy current with no current song:** show the row dimmed and make SEL do nothing (no "5 SONGS MAX").
5. **The delete ring:** 5 px stroke as in the design; no dimmed-row fragments at the right edge (clip, or blank the area behind it); the footer reads "release: keep".
6. **Delete is a single gesture:** in the song menu, holding SEL on Delete for 1.5 s fills the ring and deletes. Releasing early keeps the song. There's no separate confirm screen to enter first.
7. **Handle return codes:** show "DELETED" or "RENAMED" only on success. On failure, show "SAVE FAILED" and leave the state unchanged (`ui_songs.c:295`, `:171`).
8. **Slot choice and undo:** **"First free slot" (create, copy, the automatic Empty song after the last delete) prefers slots that have never been used. A deleted slot is reused only when no never-used slot is left, and then the undo entry for that delete must report "can't undo"** (`song_store`'s sequence check already gives this; make the seam expose it). Add this to `songs_ops_t`, and test it with the host fake.
9. On empty slots, show "empty" once, not in both columns. Give the `+ New song` row the design's dashed border.

Then re-render every PNG, run `make unit-songs unit-core scenarios screens-lint check fw` and `tools/check_loader.sh build/x0x.fwsc`, commit, update reports/T8.md, and send worker_done. T8i (wiring) comes after T7a merges.

## T8i (now): wire the songs view
T7a has merged.
1. Merge `integration` into your branch.
2. Implement `songs_ops_t` over `song_io`. Add `song_io` functions as needed: you own them for this step. T7b is touching `song_io` in parallel, so keep your additions in a clearly separate block.
3. Add the `ui.c` hook: `V_SONGS`, opened by holding SAVE for about 600 ms, plus routing, draw, footer and LEDs. A short SAVE tap stays "save now".
4. Opening a song loads it into the song home, with the clip engine stopped first and all-off.
5. Use T7a's held-note table: `note_release_all` on entering and leaving the songs view.
6. Move `tests/scenarios/pending/t8_*.x0x` into `tests/scenarios` with goldens. Add: open a song while clips are playing (silence, then the new song), delete the current song, and reboot persistence.
7. Run the full ladder and `tools/check_loader.sh`, commit, update reports/T8.md, and send worker_done.

## T8i review (Opus): merge after fixes. Not safe to install yet.
The reviewer's repro scenarios are in `/private/tmp/claude-502/-Users-caleb-CalebApps-fm1-rocket-os/10296272-f272-4eb6-9e65-5ca02d389a5d/scratchpad/t8rev/x*.x0x`. Turn each into a real scenario. Coordinator decisions are in **bold**.

1. **Undo crosses songs** (`song_io.c:343`). **Clip undo is per song: opening, creating or copying a song clears the clip undo stack.** The undo for deleting a song lives in SONGS only, and its HOME+REC uses `can_undelete` first (`ui_songs.c:229`). Outside SONGS, HOME+REC only undoes clip edits in the current song.
2. **Flash writes while playing** (`ui.c:2681`). **SONGS is a stopped, quiet view.** Entering SONGS stops the transport (with all-off), and while it's open PLAY, HOME+PLAY and REC do nothing. `live_play_tick` must not run while SONGS has focus. Every store write in SONGS also asserts that the transport is stopped, and refuses with "SAVE FAILED" if not.
3. **A failed open blanks the current song** (`song_io.c:348`). Load into a scratch `song_t` in the pool and copy it into `g_song` only on success; `cur_slot` changes only on success. Check `song_io_flush()`'s result (`ui.c:2579`): if the flush fails, show "SAVE FAILED", don't open the other song, and keep the edits.
4. **Stale strip after closing SONGS:** `songs_close` (`ui.c:2639`) forces a full redraw of every band.
5. **The audio race on open.** **Before `g_song` changes: request the transport stop, then wait (bounded, about 50 ms) for the audio ISR to acknowledge that the clip engine is stopped and quiet. Only then change `g_song` and re-init.** If it times out, refuse the open with an error rather than swapping under the ISR.
6. **SAVE held shows old X0X feedback** (`ui.c:1303`, `:1325`). In STUDIO (outside Classic), suppress `copy_tick` and `help_tick` while SAVE is held, so the only feedback is SONGS opening at 600 ms.
7. Cache `clip_count` per open menu or ring (`ui_songs.c:613`, `:661`). Don't read flash on every draw.
8. Nits: "0 takes" plural; move the undo hint off the dimmed row.

Then run the full ladder and `tools/check_loader.sh`, commit, update reports/T8.md, and send worker_done.
