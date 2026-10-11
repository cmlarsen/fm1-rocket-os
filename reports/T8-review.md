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
