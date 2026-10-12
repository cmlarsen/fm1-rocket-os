# Verification: audio.md #1–4, engine.md #1, #2, #10 (Opus, 2026-10-11, on integration)

The repro is a host harness built from `tests/host/live_bridge_test.c`, run under ASan/UBSan. Finding 10 was compiled with JieLi clang on deltron.

- **Audio 1 / engine 1, a full clip ring drops Stop: CONFIRMED.**
  - `push_cmd` returns −1, and `clip_engine_transport_stop` ignores it.
  - With 15 usable slots, MIDI Start + 14 PCs + Stop in one poll leaves `playing=1`.
  - The trigger needs a MIDI burst of 15+ commands inside one 5.8 ms block. The engine queue (1,024 slots) half is plausible but remote.
  - Fix: reserve headroom so Stop always fits, return the admission result, and keep note-off headroom in `cq_put`.
- **Audio 2, MIDI legato on BASS/LEAD: CONFIRMED.**
  - `bass_on 60`, `bass_on 64`, then `bass_off(part)` with no pitch. `midi_live.c` lacks the panel's held-key guard.
  - Releasing the earlier key cuts the note still held.
  - Fix: pass the pitch in C_BOFF, or copy the panel's last-held-key check into `ml_note_off`.
- **Audio 3, take publish before all-off: CONFIRMED.**
  - The replay's first 303/FM6/NOISE note can be clipped to about 6 ms when the audio IRQ fires in the gap. Drums are unaffected.
  - Fix: move `engine_all_off()` inside the critical section, before `g_eng.playing = 1`.
- **Audio 4, take events applied at block start: REFUTED as high.**
  - Every recorded event is stamped at a block start, and the device block is fixed at 256. The real effect is a constant early shift of ≤ 5.8 ms. Low.
- **Engine 2, replay loses clip phase: CONFIRMED.**
  - The prelude stores only `LAUNCH(NOW)`, so the recording's first LEAD note is 76 (step 16) but the replay starts at 60, step 0.
  - Any multi-bar clip, or one launched off the bar, replays out of phase.
  - Fix: record each playing role's position in the prelude and restart from it.
- **Engine 10, 64-bit division: CONFIRMED in the source, low impact.**
  - `(uint64_t)phase * new_l / e->step_L` compiles to a native 64/32 divide with no `__udivdi3`, so `check_symbols.sh` can't see it. It runs only on a tempo change.
  - Fix: use 32-bit arithmetic (the product is ≤ 1.3e9), and add a source lint for `(uint64_t)` divisions under `core/`.

**Fix order:**
1. Ring headroom for Stop.
2. Take start ordering, then clip phase.
3. The MIDI legato guard.
4. 32-bit `set_bpm` and the lint.
