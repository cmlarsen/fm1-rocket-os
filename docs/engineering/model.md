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
- **Storage target:** `SONG_SLOTS` 8. The data area is 0x97000–0xDFFFF (292 KiB), shared with X0X's own objects, and each stored object is at most `PLAT_STORE_MAX` (3,840 B). T6 measures the compressed sizes and may lower `SONG_SLOTS` or split a song into several objects; it records the result here.
- **Time** is the audio sample counter only (`uint32_t`, wrapping after about 24 h at 48 kHz).
