# Data model (frozen v1)

[`core/model/model.h`](../../core/model/model.h) is the contract between the core lanes ([build plan](build-plan.md#lanes-v3-2026-10-11)). Workers read it and never change it; changes go through the coordinator.

- **Song** (`song_t`, 14,352 B): tempo ×10, the key (`key` 0–11, C–B) and the scale (`scale`, `SCALE_*`), swing, the default launch quantize, the LIVE SETUP options (live MIDI channel, Program Change launch, stage lock), five roles, a 27-entry key → clip map, and 27 clips. Plain data with no pointers, so it copies by assignment and is stored as bytes, compressed with X0X's `lz.c`.
- **Roles** are fixed: DRUMS, BASS, LEAD, CHORDS, FX. Each has an engine, a preset, volume, mute, and a contiguous range of clips (`first_clip`, `nclips`).
- **Clips** (`clip_t`, 528 B) are indexed in key-zone order: drums 0–6, bass 7–11, lead 12–18, chords 19–23, FX 24–26 (`clip_role()`). A clip has a length in 16th steps (1–64), Loop or Once, and a launch quantize (Bar, Beat, End of clip). Its payload is a union:
  - drum clips use 11 lanes of step bits plus an accent row (the same packing as X0X's `dpart_t`);
  - melodic clips use up to 96 notes `{step, note, vel, len}`, so a step can hold a chord.
  Each clip also has 2 motion lanes: a parameter id plus one value per step, with `MOTION_NONE` meaning no value. T3 pins the id's encoding (the field was left free): `target << 12 | voice << 8 | index`, which `live_bridge.c` unpacks back into `engine_set(target, voice, index, value)`.
- **`key_clip[27]`** maps each key (pitch order F3–G5) to a clip. The default is identity. LIVE SETUP's "Assign keys" edits it.
- **Takes** are separate from the song: `take_hdr_t` plus up to 1,024 `take_event_t {sample, cmd}`, where `cmd` is X0X's 32-bit engine command word or a launch, mute or FX command from the same encoding space. A take is 8 KiB at most (cut from 2,048 events on 2026-10-11 to fit the RAM pool). A take records only user actions (launches, mutes, punch FX, parameter moves, notes played live); clip notes are regenerated from the launches. Parameter changes to the same parameter within one 16th step are coalesced (the last value wins), so a long knob sweep costs at most one event per step. The prelude also writes the current tempo and the latched/momentary punch FX, so a replay starts from the same state; a later tempo change (UI or followed clock) is recorded too. A take is recorded only while LIVE is active; leaving LIVE finalizes it at once. A MIDI Start or Continue during a recording ends the take cleanly (the transport resets under it). `song_t.has_take` says whether one exists.
- **Take saving is deferred.** A finished recording (or a clear) marks the take pending; the store write rides the normal autosave (stopped and idle, never while LIVE is active), beside the song. Undo of a not-yet-written replace/clear cancels the pending write and reloads the stored take; a clip edit made after it is undone first (chronological order).
- **Storage:** `SONG_SLOTS` is 5. Songs and takes live in 0xA0000–0xDBFFF (60 sectors, 240 KiB; the user-sample area BREAK freed). It is a shared pool of extents: 4 sectors per song and 5 per take, plus one spare extent of each kind. The record header is written last and carries slot, kind, sequence number and CRC. At boot the newest valid record per slot wins, so a power cut leaves the previous version. This guarantees 5 slots for worst-case data. 0x9F000 stays reserved for FM6 patches.
- **Time** is the audio sample counter only (`uint32_t`, wrapping after about 24 h at 48 kHz).

## Command words (frozen v1)
Takes record X0X's 32-bit engine command word, `type:4 | a:8 | b:8 | c:12` (`engine.c`). The command types are:

| Type | Meaning |
|---|---|
| 0–7 | Parameter targets (`T_*` in `engine.h`) |
| 8 | `C_DRUM` |
| 9 | `C_BON` |
| 10 | `C_BOFF` |
| 11 | `C_NOP` |
| 12 | `TAKE_LAUNCH`: a = clip, b = 0 queue / 1 now / 2 stop role, c = role |
| 13 | `TAKE_MUTE`: a = role, b = 0/1 |
| 14 | `TAKE_FX`: a = punch FX id, b = 0 off / 1 on / 2 latch |
| 15 | `TAKE_MARK`: a = 0 start / 1 end / 2 tempo, c = value (tempo ×10 split over b:c when needed) |

FM6 and NOISE (T5i) use the existing parameter-target and note-on/off types with new part numbers; they don't add command types.

**T5i: the FM6 and NOISE parts (T5i's scheme).** Two engine-only parts are added *after* X0X's four: `PART_FM6 = 4` and `PART_NOISE = 5` (`ENG_NPARTS = 6`), for the mixer, the dispatch and the render only. `NPARTS` stays 4, so no X0X part table, no sequencer (`NTRACKS = 4`) and no stored format changes.
- **Notes.** `C_BON`, `a = 4 | 5`, `b = the MIDI note`; the `c` field carries **velocity 0..127** (for `a < 4` it keeps X0X's `accent | slide`). `C_BOFF`, `a = 4 | 5`, `b = the note`. No new command type. `engine_note_on/off` queue them.
- **Parameters.** Target `T_RSV` (3, the removed BREAK part's target): **voice 0 = FM6, voice 1 = NOISE**; the index is the adapter's own 0..7. FM6: `0 ALG, 1 FB, 2 MLVL, 3 MRAT, 4 MEG, 5 VMOD, 6 DTUN, 7 SLOT` (SLOT is `engine_fm6_preset`, falling back to a ROM voice when a bank voice is not there). NOISE: `0 MODE, 1 COLR, 2 FREQ, 3 RES, 4 TRK, 5 DENS, 6 DRFT, 7 CRSH`. FM6's signed macros are stored as a pot offset (`pot = macro − lo`, so `0..hi−lo`); the rest are the adapter values.
- **Mix.** `T_MIX` voices 4 and 5 are FM6's and NOISE's strips (Level, Rev send, Dly send, Pan), like the others.
- **FM6 presets.** `engine_fm6_preset(n)` (SLOT numbering: 0..7 a ROM factory voice, 8 the own patch, 9..40 a bank voice). The bank is the 32-voice flash sector at `0x9F000` (`plat_fm6_vbank`), validated on read; an erased or corrupt sector falls back to the ROM voices. T5i never writes it.

## Undo limits (v1)
- **Depth:** the undo ring holds `UNDO_STACK_DEPTH` (11) entries, about 6 KiB (T9i cut it from 23 to free pool for the take); a new edit drops the oldest when full.
- **Take replace:** one level per song. Undo brings back the previous take; a second undo says "can't undo".
- **Pending take:** a record/clear that has not been written yet is undone by cancelling the pending write and reloading the stored take. A clip edit newer than the pending take is undone first (chronological order, T9i r1/S6); a take undo is refused while a take is armed, recording or playing.
- **Delete song:** undo works until another save reuses that storage. After that it says "can't undo" rather than restoring something wrong.
- **Deletes and take undos are durable:** they are written as new records, so a reboot keeps the result.
