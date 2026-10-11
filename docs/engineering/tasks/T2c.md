# T2c: UI fuzz for hung notes and crashes

Enforces the "no hung notes" invariant in `docs/engineering/architecture.md`. Start it after T7a merges.

**Ownership:**
- `host/x0x_host.c`: a `fuzz SEED N` command, plus an `expect voices 0` key that counts active voices across the 909, 808, 303 A/B, FM6 and NOISE;
- a new `tests/fuzz/`;
- a `make fuzz-ui` target, which `make check` also runs with a short budget.

## Build
1. **`fuzz SEED N`** generates N random input events with a deterministic PRNG: key down/up (keeping a real held-key model), button tap/hold/release (including long PLAY, SCL, HOME, SEQ, EDIT, GLO, SAVE hold, FX hold, REC), encoder turns, and waits of 0–300 ms.
   - It starts from the song home, with transitions into LIVE, Classic and the sound pages.
   - **It never sends inputs that write flash outside the normal autosave path,** and it excludes factory reset.
2. **When the sequence ends:** release every key and button, stop the transport, wait 3 s, then `expect voices 0`. Also check that the runner didn't crash and that every `expect` invariant (no `cq_put` from the ISR trap, and so on) held.
3. **`make fuzz-ui`** runs 200 seeds × 2,000 events. `make check` runs 20 × 500. A failure prints the seed and a minimised reproduction (shrink by deleting events while it still fails) as a ready-to-run `.x0x` file in `build/fuzz/`.
4. Fix nothing yourself in UI code. **Report every failing seed with its minimised scenario** in `reports/T2c.md`; the coordinator routes the fixes. If a bug blocks the fuzzer itself (a host crash), you may fix the runner.

## Done
- Commit and send `worker_done`.
