# T6i review (Opus): merge after these fixes

Safe to install: every write stays inside 0xA0000–0xDBFFF, behind four guards. Coordinator decisions are in **bold**. RC1p has merged, so `editor.c` is yours for item 1.

1. **[High] A second writer in the song region.** USB sample upload (`slots.c:17`: 3 × 80 KiB at 0xA0000; `editor.c:57-71`, `:178-197` ED_SMP_ERASE/ED_SMP_WRITE; `smp_user_scan` at `main_fm1.c:280`) erases and programs the song region at any time.
   - **Remove user sample upload completely:** the ED_SMP_* commands, `smp_user_scan`, and the user-slot reads in `slots.c`. It existed for the BREAK engine T1 stripped. Built-in samples stay.
   - **Remove any host or web tool that uploads samples,** if one is left.
   - Then grep the whole tree for any other erase or program that can reach 0xA0000–0xDBFFF and list the results in the report.
   - Test: a host test that the ED_SMP commands are gone or refused, and the flash guard test refuses non-song writers in the region.
2. **[Medium, UX] STUDIO edits stop reaching LIVE after a reboot** (`song_io.c:47`, `live_bridge.c:291`).
   - **Until T7/T8 make the song the edited object, keep a 32-bit hash of the X0X project's patterns with the stored song.** Put it in the song record header or a small side record in the same store. Don't change `model.h`.
   - **At boot and on entering LIVE, if the project's hash differs, rebuild the clips from the project, keeping the song's key assignments (`key_clip`), LIVE SETUP options and tempo,** and mark the song dirty.
   - Test: edit a pattern in STUDIO, reboot, enter LIVE, and the change plays; a key assignment survives the same sequence.
3. **[Medium] Silent autosave failure retries on the same extent.**
   - **After a failed song save, show the existing save-error indication.** Stop autosaving the song until the next edit to it. Mark the failed extent as bad in RAM for this session, so the next save picks a different one.
   - Never retry on a timer.
   - Test: inject a program failure in the host flash simulator; the error shows, no further erase happens without a new edit, and the next save after an edit succeeds on another extent.
4. **[Low] The 14 KB stack frame** in `sst_recommit_song` (`song_store.c:745`): move the buffer to the pool, sharing the store's existing scratch buffer if possible.
5. **[Low] The `ota_req` path re-runs `song_io_boot()`** (`main_fm1.c:393`). Don't remap `g_song` while audio runs. Flush any dirty song first only if the save conditions hold; otherwise keep the RAM state. Never re-init storage there.
6. **RAM:** move `song_store.c`'s second 4 KiB `lz_head` to the pool, or share it with `project.c`'s, to get `.bss` headroom back. Report the numbers.

Then run `make unit-core scenarios screens-lint parity check fw` and `tools/check_loader.sh build/x0x.fwsc`, commit, update reports/T6i.md, and send worker_done.
