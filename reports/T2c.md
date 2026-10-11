# T2c: UI fuzz for hung notes and crashes

Enforces the "no hung notes" invariant in
[architecture.md](../docs/engineering/architecture.md#invariant-no-hung-notes-2026-10-11).
Lane [T2c](../docs/engineering/tasks/T2c.md), from `integration` (includes T7a's round 2), then the
[review fixes](T2c-review.md). No hardware was flashed; `make fw` only builds (on `deltron.local`).

## What shipped

- **`host/x0x_host.c` — `fuzz SEED N`.** N deterministic random input events (a held-key model;
  button tap/press/release/hold; encoder turns and `spin`s; waits 0–300 ms) from the song home,
  with a fixed prologue that visits the sound pages, Classic and LIVE. A `stop` command and the
  `expect voices 0` / `expect heldnotes 0` keys back the release tail.
- **`tests/fuzz/`.** `run_fuzz.sh` runs seeds 1..S and minimises any failure; `shrink.py`
  (ddmin) deletes events from the body while the host still fails and re-appends the fixed tail.
- **`make fuzz-ui`** runs 200 × 2000; **`make check`** runs the short 20 × 500.
- The 909/808/303 and FM6/NOISE voice counts (`engine_voices_active`) and scenarios
  `voices.x0x`, `repro_hang.x0x`, `fuzz_crash.x0x`.

## Review follow-up ([reports/T2c-review.md](T2c-review.md))

**1. The tail never uses the panic all-off.** `host_all_stop()` (and so the `stop` command and the
fuzz release tail) releases every key and button, stops both transports
(`live_bridge_transport_stop` + `seq_stop`) and stops there. An `engine_all_off` would have masked
exactly the hung notes the fuzzer is for. The tail is `stop`, `wait 3000`, `expect voices 0`,
`expect heldnotes 0`.

**2. The shrinker pins the tail.** `tests/fuzz/shrink.py` shrinks only the event body and always
re-appends the fixed release tail (every key up, every button release, `stop`, `wait 3000`, the two
expects), so the minimiser cannot delete the thing that defines the failure.

**3. A stronger generator, with mid-run checks.**
- Held-across-transition macros: a key down → one of HOME / GLO / LFO / EDIT / SEQ / long PLAY /
  SCL / an FX hold / a SAVE hold → key up, with REC-armed variants (and a prologue one that also
  starts X0X's transport in Classic before leaving).
- **The held-note table must be empty of any note that started before a view or mode change.**
  The host records, per key, the view/mode its note started in (`hn_mode`, ui.c). After every event
  that changed `ui.view`, `ui.classic` or the LIVE owner, the fuzzer checks every key that was
  physically held *before* the change; a note that survived is a failure. The reproduction records
  `expect staleheldK 0` — `1` exactly when key K's note is still in the table and the mode has
  changed since it started, so it is *self-guarding*: deleting the transition in the shrinker makes
  it vacuous rather than a false positive. This catches (b) deterministically. (A key pressed in the
  same input frame as the change is not checked, so a legitimate post-change press cannot trip it.)

**4. The real hang is fixed.** `bass_kbd` and the 303 step preview in `ui.c` sent
`engine_bass_on` directly, bypassing the held-note table; a REC-armed key on a non-sound page (e.g.
GLOBAL) left its note sounding when the view changed. Both now register their note with the new
`note_key_track`, so a key up, a view/mode change or a `stop` releases it (accent and slide are
still sent by their own `engine_bass_on`). `tests/scenarios/repro_hang.x0x` is the minimised
reproduction; it fails before the fix and passes after.

**5–6.** See the mutation table and results below.

## A crash the fuzzer found (fixed)

With the tail no longer masking, the 200 × 2000 budget found a **segfault** the old fuzzer could
not reach: `pref_value_of` (ui.c) drew a switch's value as `d->names[v]` with `v` out of the
switch's range (a motion / p-lock value on an FX switch), reading past the names array.
AddressSanitizer pinned it to `draw_knobs → pref_value_of → put_s`. The value is now clamped to
`0..d->max` before the names lookup. The shrinker's crash-only reduction is committed as
`tests/scenarios/fuzz_crash.x0x` (it segfaulted before, passes now).

## Mutation proof (review item 5)

The reviewer's four mutants (`ui.c` note-off, `set_view`, `studio_set_classic`, `live_leave`) were
planted one at a time in a scratch copy and run against both budgets.

| # | mutant | 20 × 500 | 200 × 2000 | caught by |
|---|---|---|---|---|
| **a** | `note_key_up` sends no note-off | **11/20 seeds fail** | **72/200 fail** (first seed 4) | the end `expect voices 0` (the engine voice survives) |
| **b** | `set_view` keeps held notes | **20/20 seeds fail** | **200/200 fail** | the mid-run `expect staleheldK 0` after a view change |
| **c** | no `engine_all_off` on the Classic handoff | 0/20 | 0/200 | **cannot hang:** both calls are redundant — entering Classic calls `live_bridge_exit()` and leaving calls `live_bridge_enter()`, and both call `engine_all_off()` themselves |
| **d** | `live_leave` keeps held notes | 0/20 | 0/200 | **cannot hang:** `live_leave`'s `note_release_all()` is redundant with the `set_view()` both of its branches call, which releases the same table |

(a) is caught at 20 × 500 as required. (b) is caught by the mid-run check. (c) and (d) remove
calls that are already made by the functions they call, so they change no state; the reason is
written above rather than tested. (Note (b) and (d) also cannot leave a voice at the end, because
every key-up releases its note through the table before the view is consulted; the mid-run check
catches (b) as the release-on-change violation it is.)

## Results (item 6)

- **`tests/fuzz/run_fuzz.sh 200 2000`** (the `make fuzz-ui` budget): **all 200 seeds pass**, no
  crash and `voices 0, heldnotes 0` after every release tail (~4.5 min wall on this Mac).
- **`tests/fuzz/run_fuzz.sh 20 500`** (the `make check` budget): all pass.
- **`make check` takes ~15 s wall** end to end (the `fw` build on `deltron.local`, the lint checks,
  and the 20 × 500 fuzz); the fuzz part is ~6 s.
- **Regression (full ladder):** `make unit`, `make scenarios` (goldens unchanged),
  `make screens-lint` (0 findings), `make parity` (WASM matches native), `make check` and `make fw`
  all pass.
- **Device build** (`make fw`, JieLi clang 4.0.1, pi32v2): image **514,624 B** (the held-note mode
  byte and the mono-part release rule); RAM `.data+.bss` **95,824 B** of 98,304; POOL **292,132 B**
  of 344,064; package 609,680 B (identity FM-1_900).

## Round 2: the mono-part release rule

The review's round-2 regression: the first fix moved the 303 keyboard note-off into the table but
dropped `bass_kbd`'s per-part guard, so releasing one of two keys held on the same 303 part closed
the gate and cut the legato note. `note_key_up` now applies the rule once, for every table path
(LIVE's PLAY layer, the sound pages, the X0X keyboard and the 303 step preview): a monophonic part
(`LIVE_BASS` / `LIVE_LEAD`) sends its note-off only when the last key on that part is released;
while another key on the same part is held the part keeps sounding. v1 does not retrigger to the
other key; polyphonic roles (drums, FM6, NOISE) release per note as before. MIDI in already had the
equivalent guard (`bass_held[b] == note` in engine.c), so it was never affected.

Scenarios: `legato_cut.x0x` (the reviewer's classic-keyboard reproduction) and `legato_live.x0x`
(the BASS PLAY layer in LIVE); both assert `voices 1` after the first key is released and
`voices 0` after the last, and both fail without the rule.

**Scope of the mid-run checks.** `staleheldK` / `heldnotes` only see notes that a key put in the
held-note table. Anything else — a sequencer gate, a clip, a MIDI note, a drum one-shot — is not in
the table, so those checks would not notice it; the end-of-run `voices 0` check (every engine's
voices) is what covers the rest.

## Limits

- The fuzzer drives buttons, keys and encoders only; MIDI in, the USB path and the take recorder
  (T7b/T9) have their own tests.
- Only the 909, 808, 303 A/B, FM6 and NOISE voices are counted; a hung note in a future engine
  would need a counter there too.
