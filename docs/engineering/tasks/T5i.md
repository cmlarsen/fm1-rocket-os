# T5i: FM6 and NOISE as X0X engine parts

Integration lane for T5 in [build-plan.md](../build-plan.md#lanes-v3-2026-10-11). It runs alongside T3, which builds LIVE in `ui_live.c`/`live_bridge.c` and adds a clip-engine render hook plus a shorter `dly_buf` in `engine.c`. **Touch only the engine dispatch and part tables, not the block-render hook or `dly_buf`,** so the two branches merge cleanly. **The loader CRC port is NOT part of this task.** It changes the loader, so it waits for the owner as a separate task, T5L.

Read first:
- `CLAUDE.md` and the worker rules in `docs/engineering/build-plan.md`;
- `reports/T5c.md` (the adapter APIs), `firmware/src/dsp/eng_fm6.h`, `eng_noise.h` and `fm6_vbank.h`;
- `firmware/src/app/engine.c/.h` (parts, the command queue, how a part renders into the mix, `T_MIX` per part, the core-1 split);
- `docs/engineering/architecture.md` (the RAM plan; FM6 and NOISE state may need the pool);
- `firmware/app.ld` and `tools/build.py` (the pool and size checks).

**Ownership:**
- `firmware/src/app/engine.c/.h`: new parts only — the part enum, the dispatch for note on/off and params, render calls, and the mix strips;
- the FM6 voice-bank flash read at 0x9F000 (`fm6_vbank`);
- build lists (`tools/build.py` and the host/WASM build scripts) so the new `.c` files compile everywhere;
- new tests and scenarios.

## Steps
1. **Two new parts**, after X0X's existing ones: `PART_FM6` (polyphonic) and `PART_NOISE`. Add them to `NPARTS` and the mixer (level, pan, reverb and delay sends, like the others). Notes arrive through the existing `C_BON`/`C_BOFF` commands with the new part numbers. Use velocity where the command carries it, or extend the command's `c` field (see `docs/engineering/model.md`, Command words) without adding a command type. Parameters go through `engine_set` targets. Use `T_RSV` (3, free since BREAK was stripped) for FM6 and NOISE parameters with a voice/index scheme, or ask the coordinator.
2. **FM6 presets:**
   - The 32 factory voices load from the voice bank. If no bank sits at 0x9F000 on this unit, use the built-in ROM voices: X0X never wrote there, so it may hold Felucca leftovers or erased flash. Validate before use, and fall back to ROM.
   - `engine_fm6_preset(n)` selects a voice.
   - Never write flash in this task.
3. **CPU and core:**
   - FM6 renders on core 0. Never require core 1.
   - Measure the host cost per block with X0X's profiler (the PERF screen counters or `plat_cpu_pct` hooks) for: 909 + 303A + 303B + FM6 with 4, 6 and 8 voices + NOISE.
   - Set `ENG_FM6_VOICES` to the largest count that keeps the host estimate under 60% of the block. Report the numbers; RC1 checks them on the device.
4. **RAM:** follow the RAM plan. Put FM6 and NOISE state in the pool if `.data+.bss` would exceed 98,304 − 512. Report POOL and `.bss` before and after.
5. **Tests:**
   - a scenario that plays a 4-note FM6 chord and a NOISE hit through the command path, with an events golden and `audio.json` bounds (not silent, no clipping);
   - a unit test that a corrupt or erased voice bank falls back to ROM voices;
   - `make unit-core scenarios check fw` and `tools/check_loader.sh build/x0x.fwsc` (the loader must stay identical).

## Done
- Small commits with the attribution lines.
- `reports/T5i.md` with the CPU table, RAM numbers and the param scheme.
- `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T5i.md`.
