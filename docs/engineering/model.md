# Data model (frozen v1)

[`core/model/model.h`](../../core/model/model.h) is the contract between the core lanes ([build plan](build-plan.md#lanes-v3-2026-10-11)). Workers read it and never change it; changes go through the coordinator.

- **Song** (`song_t`, 14,352 B): tempo ×10, swing, the default launch quantize, the LIVE SETUP options (live MIDI channel, Program Change launch, stage lock), five roles, a 27-entry key → clip map, and 27 clips. Plain data with no pointers, so it copies by assignment and is stored as bytes, compressed with X0X's `lz.c`.
- **Roles** are fixed: DRUMS, BASS, LEAD, CHORDS, FX. Each has an engine, a preset, volume, mute, and a contiguous range of clips (`first_clip`, `nclips`).
- **Clips** (`clip_t`, 528 B) are indexed in key-zone order: drums 0–6, bass 7–11, lead 12–18, chords 19–23, FX 24–26 (`clip_role()`). A clip has a length in 16th steps (1–64), Loop or Once, and a launch quantize (Bar, Beat, End of clip). Its payload is a union:
  - drum clips use 11 lanes of step bits plus an accent row (the same packing as X0X's `dpart_t`);
  - melodic clips use up to 96 notes `{step, note, vel, len}`, so a step can hold a chord.
  Each clip also has 2 motion lanes: a parameter id plus one value per step, with `MOTION_NONE` meaning no value.
- **`key_clip[27]`** maps each key (pitch order F3–G5) to a clip. The default is identity. LIVE SETUP's "Assign keys" edits it.
- **Takes** are separate from the song: `take_hdr_t` plus up to 2,048 `take_event_t {sample, cmd}`, where `cmd` is X0X's 32-bit engine command word or a launch, mute or FX command from the same encoding space. A take is 16 KiB at most. `song_t.has_take` says whether one exists.
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
