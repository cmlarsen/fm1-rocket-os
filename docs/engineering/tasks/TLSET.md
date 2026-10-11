# TLSET: LIVE SETUP (EDIT in LIVE)

Read first: [interaction-model.md § LIVE SETUP](../../product/interaction-model.md#live-setup-edit-in-live--configuring-keys-r10), the MIDI section of the same file, [screens.md](../../product/screens.md) (LIVE 7 Assign keys, LIVE 9 Setup), the designs `design/project/Live-Setup.dc.html` and `Live-Assign.dc.html`, [ux-principles.md](../../product/ux-principles.md), and the invariants in [architecture.md](../architecture.md).

## Scope
1. **EDIT in LIVE opens LIVE SETUP**, a list screen. EDIT or BACK leaves it. Today EDIT opens the take card (`ui_live.c`, `case B_EDIT`); the take card becomes the **Take** row of SETUP.
2. **Rows,** all saved with the song (they mark it dirty, and autosave writes it only while stopped and idle, after leaving LIVE):
   - **Assign keys.** Hold a key and turn PRESETS to pick what it triggers: a clip of any role, a role stop, a punch FX, or a live note. Release to commit. A toast confirms it, for example "A4 → CHORDS B". Use `song.key_clip[]`. If the encoding has to grow, bump the song version and keep loading older songs (add a round-trip test).
   - **Launch default** (`song.quant_default`).
   - **Stage lock** (`song.stage_lock`): while playing, clearing from launch mode is blocked.
   - **External keys channel** (`song.live_channel`): Omni or 1–16.
   - **Program Change launch** (`song.pc_launch`): Off or On.
   - **Clock in:** the existing GLOBAL CLOCK IN setting, Auto or Internal, shown here too. It is the same value, not a copy.
   - **Take:** play, stop and clear, keeping T9i's behaviour.
3. **Header EXT chip.** The LIVE header shows a small **EXT** chip while an external clock is being followed (`midi_live_ext()`), and the tempo readout uses `midi_live_bpm_x10()` then.
4. **Help.** Add or update the help card for LIVE SETUP and the cheat sheet entry. Use the SEL wording for the layer toggle (the owner chose SEL, the panel label).

## Rules
- Put new UI code in new files (`ui_live_setup.c`). Keep X0X's layout.
- No hung notes. Interrupts never wait. Settings reach the engine through the existing main-loop paths, never from an ISR.
- No flash writes while LIVE is active.
- Write a failing scenario or unit test first. Add goldens for LIVE 7 and LIVE 9 and the EXT chip, generated with `GOLDEN_UPDATE=1`. View them at 1:1 on 240×240 and list what changed in the commit.
- Docs: update interaction-model.md (drop the "LIVE SETUP rows are a follow-up" note in the MIDI section), screens.md and MANUAL.md.

## Done when
- `make unit unit-core scenarios screens-lint fuzz-ui check fw fw-debug` pass on the committed tree, and `tools/check_loader.sh` passes on both packages.
- Pool spare is at least 8,192 B on both builds. The report gives the numbers.
- `reports/TLSET.md` lists each row, the test that covers it, and the pool numbers.
- Never flash hardware.
