# T6c review (Opus): merge after these fixes

Coordinator decisions are in **bold**. The reviewer's scratch tests, which prove each defect, are in `/private/tmp/claude-502/-Users-caleb-CalebApps-fm1-rocket-os/10296272-f272-4eb6-9e65-5ca02d389a5d/scratchpad/` (`rev.c`, `cut.c`, `fz.c`). Turn them into real tests in `tests/host/`.

1. **Delete must survive reboot** (`song_store.c:641-655`).
   - **Delete re-commits the song as a new record, with a `deleted` flag in the record header and a new sequence number, into the spare extent.** The data stays loadable for undelete.
   - **Undelete re-commits it with the flag cleared.**
   - `song_store_list` reports deleted slots as free.
   - A save into a deleted slot simply supersedes it.
   - Test: save, delete, reboot, and the slot is free; then undelete, reboot, and the song is back.
2. **Take undo must survive reboot** (`song_store.c:785`). Replace the RAM pointer swap.
   - **Undo of replace-take loads the previous take into the caller's RAM take buffer, then commits it as a new take record with a new sequence number.** It may reuse the previous take's own extent, since the data is now in RAM and the newer take stays valid on flash until the commit. The replaced take becomes the new "previous", so redo works the same way.
   - A power cut during this leaves the newer take; only the undo is lost.
   - Test: takes 1 and 2, undo, reboot, and take 1 is current.
3. **Take undo depth is 1 per slot** (`undo_stack.c:91-94`, `song_store.c:364-376`).
   - **Only the newest REPLACE_TAKE entry per slot is undoable.** Pushing a new one for a slot invalidates older ones for that slot, so they fail cleanly.
   - Undo entries store the sequence number of the record they expect to restore. When that record is gone, for example its extent was given to another slot, undo returns a distinct "cannot undo" code (not success), so the UI can say so.
   - Test: takes 1, 2 and 3, then undo twice: the first undo gives take 2 and the second returns "cannot undo".
4. **Undo-delete after the slot was reused must return "cannot undo"** (`undo_stack.c:95-97`), using the same sequence-number check.
5. **Validator** (`song_store.c:407-450`). Reject:
   - a name without a NUL inside `SONG_NAME_LEN`;
   - `bpm_x10` outside 200–3000;
   - a note `step >= clip.len`;
   - a note length of 0 or above 64;
   - a note velocity of 0 or above 127;
   - motion values that aren't `MOTION_NONE` and are above 127;
   - a motion param that isn't `PARAM_NONE` and is out of range (ask T5i later; for now, reject anything at or above 0x0800);
   - a `key_clip` entry that isn't `CLIP_NONE` and is at or above `NCLIPS`.
6. **Make the fuzzer reach the validator** (`song_fuzz.c:107-131`). Mutate the decoded song fields, then re-encode with a valid CRC, or call `song_validate` directly on mutated structs. Count and print how many mutated loads reach the validator. That count must be above 90%.
7. Low:
   - wear-level song and take extents by choosing the free extent with the lowest last-used sequence number;
   - erase only the sectors a record needs;
   - if the post-commit check fails, the save returns an error and the next boot must not prefer that record. Invalidate it, or document why it can't win.
   - leave the sequence-number wrap alone.

Then run `make unit-store` (plain and sanitizer) and the device compile, commit, update reports/T6c.md, and send worker_done.
