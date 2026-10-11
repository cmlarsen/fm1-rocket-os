# T2c review (Opus): merge after fixes. As shipped, the fuzzer can't catch hung notes.

The reviewer's files are in `/private/tmp/claude-502/-Users-caleb-CalebApps-fm1-rocket-os/10296272-f272-4eb6-9e65-5ca02d389a5d/scratchpad/`: `repro_hang.x0x`, `shrink2.py` and the `fuzzclone` variant "B". Coordinator decisions are in **bold**. **For item 4 you may change UI code** (`ui.c`), which overrides the spec's "don't fix UI".

1. **Remove `engine_all_off()` from the fuzz release tail** (`host/x0x_host.c:1188`). The tail is: release every key and button, stop both transports (`live_bridge_transport_stop` + `seq_stop`), `wait 3000`, `expect voices 0`. Never use the panic all-off in the tail.
2. **Pin the tail in the shrinker** (`tests/fuzz/shrink.py:56`). Shrink only the event body and always re-append the fixed tail. You can adopt the reviewer's `shrink2.py`.
3. **Make the generator stronger:**
   - add held-across-transition macros: key down → one of HOME/GLO/LFO/EDIT/SEQ/long PLAY/SCL/FX hold/SAVE hold → key up;
   - add REC-armed variants;
   - add **checks in the middle of a run**: after every view or mode change with no keys held, `expect voices` counts only voices that a held key or a playing clip justifies. Simpler: track each key's note-ons in the event log and fail when a note-on has no note-off within 3 s after its key went up while no clip on that part is playing. Choose the approach that is sound and document it.
4. **Fix the real hang the fuzzer found.** The legacy key-play paths `bass_kbd` (`ui.c:2072`, called from `:2292`) and `bass_step_key` (`ui.c:2097`) bypass the held-note table. **Route every path where a key plays a note through the held-note table,** or make those paths unreachable outside Classic, and inside Classic register their notes in the table. Add `repro_hang.x0x` as a scenario.
5. **Prove it with mutation tests.** Plant the reviewer's mutants (a) no note-off in `note_key_up`, (b) `set_view` keeps held notes, (c) no `engine_all_off` on the Classic handoff, (d) LIVE exit keeps held notes, one at a time in a scratch copy. Report which ones the 20×500 and the 200×2000 budgets catch. (a) must be caught at 20×500. (b)–(d) must be caught by the checks in the middle of a run, or each gets a written reason why it can't hang.
6. After the fixes, the unmutated build passes 200×2000. Report how long `make check` takes.

Commit, update reports/T2c.md with the mutation table, and send worker_done.

## Re-review: one regression to fix
- **A held legato 303 note is cut** (`ui.c:2100/2113` `bass_kbd`, `ui.c:2137/2147` `bass_step_key`). The `kbd_held_note[b] == n` guard was dropped, so `note_key_up` → `engine_bass_off(part)` closes the gate whatever is sounding. The reviewer's repro is `/private/tmp/claude-502/-Users-caleb-CalebApps-fm1-rocket-os/10296272-f272-4eb6-9e65-5ca02d389a5d/scratchpad/legato_cut.x0x`.
- **Coordinator rule for every monophonic part (303 A/B, and any mono role path in the held-note table):** releasing a key sends the part's note-off only if that key's note is the one the part is sounding. If another key on the same part is still held, keep sounding. Don't retrigger in v1.
  - Apply it once, in `note_key_up` / the held-note table, so LIVE PLAY, the sound pages, the X0X keyboard and MIDI all get it.
  - Polyphonic engines (FM6, NOISE, drums) release per note as now.
- Add `legato_cut.x0x` as a scenario, plus a LIVE PLAY-layer variant on BASS.
- Document in `reports/T2c.md` that `staleheld`/`heldnotes` only see notes in the held-note table; the end-of-run `voices 0` check covers the rest.

Run the full ladder plus `make fuzz-ui`, commit, and send worker_done.
