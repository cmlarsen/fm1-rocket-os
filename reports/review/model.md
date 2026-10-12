# Model and persistence review

Reviewed commit `3c90e73299c96f83c97012410725206cca781b28`, 2026-10-11. Review only; no product, test, or documentation source changed. Findings: **11 BUG, 3 EDGE, 0 SIMPLIFY, 0 CLARITY, 0 MAINTAIN**; **9 high, 5 medium**. Ranked by impact below.

Read `CLAUDE.md`, the glossary, architecture invariants, interaction model, and engineering model contract. Inspected all of `core/model/*`, `song_io.c`, `song_templates.c`, `undo.c`, `project.c`, and `lz.c`. Traced save policy and model consumers in `ui.c`, `ui_live.c`, `ui_clip.c`, `ui_studio.c`, `live_bridge.c`, `engine.c`, `midi_live.c`, `plat_fm1.c`, and the storage driver only as needed to establish product misuse. Did not review inherited DSP/HAL/loader internals or flash hardware.

## Findings

### 1. BUG · high — New song exceeds the device's user stack

**Location:** `firmware/src/app/song_io.c:777`; `firmware/src/app/song_templates.c:167,186`.

`New > Empty` and `New > Starter` each allocate a full `song_t` in the caller and another in the template callee. Both are simultaneously live; each song is 14,352 bytes. The user stack is only `0x01C7A000 - 0x01C74100 = 24,320` bytes (`firmware/app.ld:32`). The firmware compiles these as separate O2 translation units without LTO (`tools/build.py:43-51`).

**Scenario:** SONGS → New → Empty/Starter → caller's song plus template's temporary need at least 28,704 bytes before other call frames → stack crosses the guard into adjacent memory; crash or memory corruption.

**Evidence ran:** compiled both files with host Clang `-O2 -fstack-usage`: `song_io_songs_create` 14,432 B, `song_template_empty` 14,432 B, `song_template_starter` 14,464 B. Nested totals are 28,864/28,896 B. These are host frame sizes, not a pi32v2 measurement; the two 14,352-byte objects alone establish the budget violation. No device repro run.

```c
/* song_io.c */ song_t s; song_template_starter(&s, bpm_x10, key);
/* song_templates.c */ song_t s; /* ... */ *out = s;
```

**Fix:** construct templates directly into `out` and use the guarded shared song scratch in the CRUD path. Check the actual target's maximum call-chain stack usage.

### 2. BUG · high — Recording motion makes the song unsavable

**Location:** `core/model/song_store.c:569`; `firmware/src/app/song_io.c:489`.

The validator still rejects every nonempty motion parameter `>= 0x0800`, while the implemented encoding is `target << 12 | voice << 8 | index`. The motion page writes MASTER (6) and FX (4) parameters, all above that limit. Validation therefore rejects legitimate UI-generated songs and existing legitimate motion on load.

**Scenario:** record FILTER/SPACE/ECHO/DRIVE on a clip → parameter such as `0x6001` stored → save returns `-2` → whole song stays unsaved and edits disappear on reboot. A valid-CRC record containing actual motion also cannot load.

**Repro ran, ASan/UBSan:** create melodic clip 7, `song_io_motion_lane(7, 0x6001, 1)`, set step 0 to 80 → `song_validate=-1`, `song_io_save=-2`. UI reachability is `ui_clip.c:642-648,679`.

```c
if (c->motion[j].param != PARAM_NONE && c->motion[j].param >= 0x0800u)
    return -1;
```

**Fix:** validate the actual target/voice/index encoding and value range against supported parameter metadata; use the same contract in lane creation and load validation. Do not merely increase the numerical limit and admit nonexistent targets.

### 3. BUG · high — Decoded fallback does not become the protected current record

**Location:** `core/model/song_store.c:626,749`; free-extent selection at `:412,449`.

Boot selects records using envelope CRC alone. If the newest payload has a valid CRC but fails decompression/migration/model validation, loading falls back to an older good extent without changing `slot.song_ext` or the directory metadata. The allocator still considers the good fallback free and protects the malformed extent instead.

**Scenario:** all five song slots used; slot 0 has good extent 0 and newer malformed extent 5 → load succeeds from extent 0 → next save erases extent 0, the only free extent → power loss/program failure → no valid version of slot 0 remains. This defeats the previous-version guarantee precisely during corrupt-record recovery.

**Repro ran, ASan/UBSan:** filled five slots, encoded a sixth slot-0 song with `key_clip[0]=39`, correct payload/header CRCs, sequence 6; initialized and loaded; injected next program failure; rebooted. Output: `fallback load=0 directory_extent=5 next save fail=-4 reboot_load=-1`.

```c
if (sst_try_song(st, order[best], (uint8_t)slot, out) == 0)
    return 0;  /* does not adopt order[best] into st->slot[slot] */
```

**Fix:** adopt the fully validated fallback's extent, sequence, flags, and tag before exposing it, or fully validate candidates before directory selection. Apply the equivalent repair to take fallback so its valid copy is also protected.

### 4. BUG · high — Flush reports success after a failed song save, allowing open to discard edits

**Location:** `firmware/src/app/song_io.c:134`; caller `firmware/src/app/ui.c:2309`.

`failed` disables the song write but `song_io_flush()` initializes its result to success. The SONGS open wrapper interprets this as successful preservation of the current song and opens another song. Avoiding automatic retries is reasonable; claiming the outstanding save succeeded is not.

**Scenario:** edit current song → save fails (bad flash or validation failure) → open a different song → flush skips dirty song and returns 0 → open replaces RAM → edits permanently lost. This also amplifies findings 2 and 6.

**Repro ran, ASan/UBSan:** save slot 0, change key assignment to 9, fail next program, call flush → `flush=0 dirty=1 failed=1`; subsequently opening another stored song replaces the edited assignment. UI wrapper was verified by inspection; the harness directly invoked the same model calls.

```c
int rc = 0;
if (dirty && !failed) rc = song_io_save();
if (rc == 0) rc = song_io_take_flush();
```

**Fix:** return failure whenever dirty state remains uncommitted, including the no-retry state. Keep song switching blocked until a successful explicit retry or an explicit discard decision.

### 5. BUG · high — Take undo/redo writes flash while LIVE or audio is active

**Location:** `firmware/src/app/song_io.c:561-567`; `core/model/song_store.c:1028`; CRUD guard at `song_io.c:741`.

Take undo only refuses an armed/recording/playing *take*. It does not refuse LIVE mode, the clip transport, or active voices. Restoring a durable take erases/programs flash immediately. HOME+REC/HOME+PLAY in LIVE invoke these paths (`ui_live.c:417,456`). Separately, CRUD's `store_quiet()` tests only transport, leaving external held notes unprotected.

**Scenario A:** save two recordings after leaving LIVE → re-enter LIVE and launch clips → HOME+REC → synchronous erase/program during performance → XIP stalls/audio glitches and violates “no flash while LIVE,” even if clips are stopped.

**Scenario B:** open SONGS, then hold a TRS MIDI note while transport is stopped → New/Copy/Rename/Delete passes `store_quiet()` and writes while a voice sounds. MIDI is polled unconditionally (`main_fm1.c:350`), and its note sink has no SONGS gate (`midi_live.c:49`). Entry into SONGS silences existing notes only; it cannot silence a later arrival.

**Proof:** traced complete call chains; no device flash/audio repro run. Existing host take undo tests establish that restore performs writes but stub transport/voice state, so they do not cover the policy.

```c
if (type == UNDO_REPLACE_TAKE &&
    (take_recording() || take_armed() || take_playing())) return UNDO_CANNOT;
/* core restore */ sst_write_take(...);
/* CRUD */ static int store_quiet(void) { return !live_bridge_transport_playing(); }
```

**Fix:** centralize stopped + idle + outside-LIVE admission for all product flash paths. Durable undo should swap safe RAM state and queue persistence, or refuse/defer its flash operation. SONGS must suppress new audio input or wait for confirmed silence before every mutation.

### 6. BUG · high — Shortening a melodic clip invalidates the entire song

**Location:** `firmware/src/app/song_io.c:279`; validator `core/model/song_store.c:560`.

Changing clip length leaves notes at now-outside steps. The validator rejects any note whose step is outside the new length, so an ordinary length edit makes the whole song impossible to save. `song_io_note_add`/`song_io_note_set` similarly accept steps below 64 but beyond `clip.len`.

**Scenario:** boot Starter, shorten BASS A from 16 to 8 → notes at 8/12/14 remain → validation/save fail → reboot loses edits. User can recover only by lengthening again or deleting the offending notes.

**Repro ran, ASan/UBSan:** `song_io_clip_set_len(7,8)` on Starter → `validate=-1 save=-2 note_last_step=14`.

```c
k->len = (uint8_t)clamped_length; /* does not reconcile k->d.notes */
```

**Fix:** choose and implement a consistent shortening policy: trim out-of-range notes with undo, or preserve hidden notes and safely permit them in serialization/playback. Note setters must enforce the chosen invariant as well.

### 7. BUG · high — Failed take clear is acknowledged and its pending state discarded

**Location:** `firmware/src/app/song_io.c:629,667-679`.

`take_store_empty()` returns void and drops `song_store_take_save()`'s error. Its caller clears the pending operation and adds undo even if the erase/program/commit failed. No failure is surfaced, and a subsequent save can leave `has_take=0` with the old nonempty take still on flash.

**Scenario:** clear take → next flash program fails → flush returns success and pending clears → old take remains durable, clear cannot be retried, and undo bookkeeping claims a store change that never happened.

**Repro ran, ASan/UBSan:** persist take, request clear, set backend `fail_prog_once=1`, flush → `rc=0 pending=0 failed=0`.

```c
static void take_store_empty(int slot) { /* ... */ song_store_take_save(...); }
if (take_pending == TAKE_CLEAR || live_bridge_take_nevents() == 0) take_store_empty(slot);
```

**Fix:** propagate the empty-take save result; retain pending state and show failure until the commit is verified. Push durable undo only after success.

### 8. EDGE · high — Song/take split commits lose the previous take across a power cut

**Location:** `firmware/src/app/song_io.c:135-137,637,647`.

The song's `has_take` is committed before its separate take record. Each extent is header-last, but the pair is not atomic. A clear persists `has_take=0` first, which hides the still-valid old take if power dies before the empty-take record commits. Conversely, a first recording can commit `has_take=1` without any readable take. Opening/reboot then silently clears the flag.

**Scenario:** existing song with take A → request clear → song save commits → cut power before take save → reboot shows no take, although A's record survived. The promised prior whole-song version is not recovered. For first recording, a cut in the same window loses the new recording while the song header claimed a take.

**Repro ran, ASan/UBSan:** first recording pending, call song save, fail next take program, reboot → `take_flush=-1 reboot_has_take=0 events=0`. Clear's inverse was traced through the same call order; no physical power-cut run.

```c
if (dirty && !failed) rc = song_io_save();
if (rc == 0) rc = song_io_take_flush();
```

**Fix:** commit a consistent song/take transaction or reference a specific take generation from the final song commit. An empty take can serve as a tombstone, but the song must only refer to committed take state. Merely reordering the independent writes is not enough for all replacement and slot-reuse cases.

### 9. BUG · high — Failed durable take undo corrupts the current RAM take

**Location:** `core/model/song_store.c:1023-1029`; `firmware/src/app/song_io.c:567-571`.

Restore loads the previous take straight into the live buffer before attempting its flash commit. If the write fails, the model returns `UNDO_CANNOT`, and the bridge's runtime event count/state is not updated. Its header/events have nevertheless been replaced. The undo cursor also advances past the failed operation (`undo_stack.c:166-169`).

**Scenario:** current take B has 3 events; previous A has 2 → undo reads A into B's buffer → flash write fails → UI says cannot undo, but runtime still expects 3 events with A's 2-event header and a stale B tail → replay is inconsistent and B's RAM data is gone.

**Repro ran, ASan/UBSan:** persisted 2-event A then 3-event B; injected program failure on undo → `rc=-1 runtime_count=3 header_count=2 runtime_length=100` (B had length 200).

```c
if (sst_try_take(st, pe, slot, hdr, ev, max_events) != 0) return -3;
if (sst_write_take(..., hdr, ev)) return -4;
```

**Fix:** preserve the current buffer until restore is accepted, or make the RAM swap a successful operation with deferred persistence and consistent runtime publication. Distinguish stale history from transient write failure and keep the cursor retryable for the latter.

### 10. BUG · medium — First recorded take cannot be undone after it flushes

**Location:** `core/model/undo_stack.c:100-102,144`; `core/model/song_store.c:1000-1002`.

A slot without a prior take record gets a replace-take undo entry with sequence 0. `undo_take_live()` rejects it. The logical previous state was “no take,” but that state is not represented durably.

**Scenario:** new song → record first take → leave LIVE and autosave → HOME+REC says cannot undo. Before flush the same operation can be undone, so functionality changes merely because autosave occurred.

**Repro ran, ASan/UBSan:** boot empty store, save song, save its first 2-event take through request+flush → `song_io_undo=-1`.

```c
uint32_t seq = 0u;
if (store->slot[slot].prev_take_ext >= 0) seq = store->slot[slot].prev_take_seq;
/* undo_take_live */ if (e->u.seq == 0u) return 0;
```

**Fix:** represent the absent previous take as an explicit undoable state, or create an empty take baseline before the first replacement.

### 11. BUG · medium — Pending take undo has no redo

**Location:** `firmware/src/app/song_io.c:596-606`.

Undo of an unflushed take cancels pending and overwrites RAM with the stored previous take without saving the cancelled take in any redo state. No ring entry is added, so redo cannot restore it. This violates the advertised take undo/redo behavior and makes the autosave window observable.

**Scenario:** stored take A → record B while LIVE → HOME+REC restores A → HOME+PLAY reports no redo and B is lost. The corresponding durable replace is redoable.

**Repro ran, ASan/UBSan:** A length 100, pending B length 200 → `undo=1 length=100`, then `redo=0 length=100`.

```c
take_pending = TAKE_NONE;
song_io_take_load_ram();
return UNDO_DONE; /* no redo state kept */
```

**Fix:** retain a bounded take swap/redo state for pending operations and insert it at the operation's chronological position. Preserve the no-flash-in-LIVE rule while doing so.

### 12. BUG · medium — Reusing a deleted slot retains the deleted song's take

**Location:** `core/model/song_store.c:791-795,883`; `firmware/src/app/song_io.c:789,819`.

Saving a new song into a deleted slot changes only its song record. `take_ext`, `take_seq`, and previous-take state still belong to the deleted song, including after reboot. The SONGS list reports a take even though the new song's `has_take=0`. Pending undo also forcibly sets `has_take=1` before loading, so it can attach the deleted song's take to the new song.

**Scenario:** delete song A with a take; fill all never-used slots; create Empty B in A's freed slot → B is advertised as having A's take. Record B's first take and undo before flush → A's recording is loaded into B.

**Repro ran, ASan/UBSan:** delete slot 0 with a persisted take, create four other songs, create another Empty → `reuse slot=0 has_take=1`. Cross-song attachment follows directly from `song_io_undo:597-599`; that second sequence was not separately executed.

```c
st->slot[slot].song_ext = ext; /* save does not reset/tombstone the old take */
out->has_take = st->slot[slot].take_ext >= 0 ? 1u : 0u;
```

**Fix:** associate take records with a song generation/identity, and atomically establish an empty take when reusing a slot. List the song's actual committed take presence, not merely existence of any historical take extent.

### 13. EDGE · medium — Loaded takes are not semantically validated

**Location:** `core/model/song_store.c:931-941,990`.

Only event count and adjacent timestamp ordering are checked. Tempo, length, first/last marker structure, command target/voice/index/value, role/clip/FX IDs, and event containment in the take length are unchecked. CRC proves bytes were committed, not that they are a valid take. Replay directly dispatches engine command words (`live_bridge.c:147`, `engine.c:541`).

**Scenario:** valid-CRC record with `bpm_x10=0`, length 1, one arbitrary parameter command (`type=0,a=255,b=255,c=4095`), no START/END → load succeeds and passes malformed data to replay. A first timestamp `0xFFFFFFFF` likewise passes the adjacent-only check and can be rendered immediately as a past event under signed wrap comparisons.

**Repro ran, ASan/UBSan:** `song_store_take_save/load` of the malformed record above both returned 0: `bpm=0 cmd=0fffffff`. No arbitrary command replay or hardware crash was attempted.

```c
if (hdr->nevents != nevents) return -1;
for (k = 1; k < nevents; k++)
    if ((int32_t)(ev[k].sample - ev[k-1].sample) < 0) return -1;
return 0;
```

**Fix:** share a take validator between save/load/restore; reject malformed timestamps, unsupported commands and field ranges, and require the recorder's marker/length invariants. Validate before publishing to the bridge.

### 14. EDGE · medium — Sound migration revision can commit before the migrated sound

**Location:** `firmware/src/app/project.c:239,266-269`; migration decision at `:237`.

Sound layout/revision lives in settings, which is committed before the separately stored sound. On loading old sound, RAM's revision is advanced to 5; a power cut after settings save but before sound save leaves old sound bytes paired with revision 5. On reboot the required migration is skipped permanently.

**Scenario:** load pre-T5i revision-4 sound whose new FM6/NOISE mix slots are zero/old reserved bytes → `sound_rev5()` establishes valid defaults → autosave commits settings `snd_rev=5` → power loss before OBJ_SOUND commit → next boot reads old sound but skips `sound_rev5`, leaving new parts at old invalid defaults/zero levels. Older drum-layout migrations have the same metadata coupling.

**Proof:** traced the two independent A/B object commits and revision predicates; no project harness or hardware power-cut repro run. Header-last protects each object individually, not their revision relationship.

```c
proj.set.snd_rev = PROJ_SND_REV;
/* save */ save_obj(OBJ_SET, ...); save_obj(OBJ_SOUND, ...);
```

**Fix:** store the revision with the sound object itself and migrate according to that record's revision, or atomically bind settings and sound generations. A simple save-order reversal still needs protection against double migration after a cut.

## Checked without additional findings

- V1 → v2 key assignment migration keeps 0–26 and 255 unchanged, rejects legacy non-clip bytes 27–38, and preserves the song layout; fixture and round-trip tests pass. V2 target holes 39–254 are rejected.
- Song envelope bounds, fixed role/clip ranges, name termination, note count/value limits, song tempo/key/scale bounds, flags, and CRC checks prevent the covered out-of-bounds loads. Take count and byte-length bounds are enforced; semantic validation is the exception in finding 13.
- Ordinary individual song record writes use separate extents, bounded page programs, readback, and header-last commits; byte-cut and operation-cut tests preserve old/new valid records. Fallback allocation and cross-record consistency are the exceptions above.
- Song/take flash addressing stays in `0xA0000..0xDBFFF` with sector/page guards; region assertions avoid protected flash, FM6 bank, and PANEL/MOTION. Product save no longer writes the removed X0X pattern/song/motion objects.
- `lz.c` literal/match decoder checks truncation, output capacity, and back-reference distance; compressor/decoder round trips and malformed-stream tests pass under ASan/UBSan. No heap, double, wall-clock read, or 64-bit division introduced in reviewed model code.
- Clip undo/redo swaps and normal branch invalidation are sound in the exercised tests; deferred take insertion preserves the newer clip-edit order and redo tail, including a full ring. Take-specific failures are listed above.
- Ordinary autosave checks LIVE/entry-hold, transport, voice activity, recording, confirmation overlay, quiet interval and minimum save gap (`ui.c:4049-4076`). This gate is sound by inspection; it is bypassed by the distinct mutation paths in finding 5.
- Clip mutation and clip undo call the main-loop staging hook, rather than reinitializing playback; no direct model ISR reads, main-loop-ring ISR push, or interrupt spin was added in the scoped files. Adjacent sequencer concurrency remains the engine review lane's responsibility.
- Starter generation is deterministic, clamps BPM/key, respects clip/note limits and initializes motion sentinels; all-key template tests pass. Its nested allocation is the exception in finding 1.
- Inherited diff undo's normal edits, overflow eviction, shadow swaps and redo branch behavior pass the existing host test; no additional issue found there.

## Verification and limitations

Ran `make unit-store unit-songio` and `make unit-songs`: native and ASan/UBSan variants passed. Store tests include all song byte cuts, operation cuts, migration fixtures, and 100,000 mutations per fuzz phase. Ran existing `tests/host/lz_test.c` and `undo_test.c` with ASan/UBSan: passed.

Ran two throwaway `/tmp` harnesses using the existing simulated NOR/live-bridge stubs, real `song_io`, templates, store, and undo implementations; all observations quoted above reproduced under ASan/UBSan. Harnesses are not committed. Selected independent evidence:

```text
motion param=0x6001 validate=-1 save=-2
shorten Starter bass to 8 validate=-1 save=-2 note_last_step=14
fallback load=0 directory_extent=5 next save fail=-4 reboot_load=-1
failed clear rc=0 pending=0 failed=0
failed durable undo rc=-1 runtime_count=3 header_count=2 runtime_length=100
pending undo=1 events=2 length=100 redo=0 length=100
first take durable undo=-1
reuse slot=0 has_take=1
invalid take save=0 load=0 bpm=0 cmd=0fffffff
```

No hardware was flashed; no target build, audio glitch measurement, UI automation or full project migration repro was run. Host stubs do not model concurrent IRQs, so source-traced audio gating findings are explicitly distinguished from executed tests. Tests passing does not cover the new integration/failure cases above.
