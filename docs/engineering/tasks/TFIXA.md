# TFIXA: audio and take fixes from the code review

Source: [reports/review/audio.md](../../../reports/review/audio.md), [engine.md](../../../reports/review/engine.md) and the verification [reports/review/verify-audio.md](../../../reports/review/verify-audio.md). Read them, plus the invariants in [architecture.md](../architecture.md).

## Scope, in order
1. **Command rings can drop Stop** (audio 1 / engine 1).
   - Reserve ring headroom so a Stop, all-off or note-off is always admitted. Non-safety pushes are refused first.
   - Return admission results to callers, and count drops.
   - Test: MIDI Start, 14 PC launches and Stop in one poll must stop.
2. **Take start ordering** (audio 3). Move `engine_all_off()` inside the critical section, before `g_eng.playing = 1`. Write a test that injects the audio IRQ in the old gap.
3. **Take replay keeps clip phase** (engine 2). The prelude records each playing role's clip position, and replay restarts each clip from it. Test: a 2-bar clip launched mid-bar, then a take; the replay's note stream must match the recording (eventscmp).
4. **External MIDI mono legato** (audio 2). Releasing an older note must not cut a newer held note on BASS/LEAD. Either pass the pitch in the note-off and check it, or use the panel's held-key rule. Test both the MIDI and the clip paths.
5. **The rest of the engine and audio report's medium findings, where the fix is small and testable.**
   - Start/Continue staging overwriting a pending launch.
   - Natural take end silencing the final window.
   - Rejected launches being recorded.
   - Stop then Start in one drain.
   - The UINT32_MAX sentinel.
   - Skip anything that needs a redesign, and list it in the report.
6. **No 64-bit division** (engine 10). Use 32-bit arithmetic in `set_bpm`, and add a lint to `make check` that fails on a 64-bit `/` or `%` under `core/` and the product files in `firmware/src/app`.

## Rules
- Write a failing test first for each item.
- Interrupts never wait, the ISR never reads `song->clip[]` or pushes onto main-loop rings, and no hung notes.
- Keep X0X's layout. Never flash hardware.

## Done when
- `make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` pass on the committed tree, and `tools/check_loader.sh` passes on both packages. Pool spare is at least 8,192 B on both builds.
- `reports/TFIXA.md` lists each finding, its test, and what was skipped and why.
