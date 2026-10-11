# T7a: STUDIO song home and clip CRUD (the song becomes the edited object)

Integration lane. T8, the songs UI, runs in parallel in new files (`ui_songs.c`). Coordinate through your `ask` command if you need anything in its area.

Read first:
- `CLAUDE.md`;
- `docs/product/interaction-model.md` (the one grammar, Modes, STUDIO tasks, Saving);
- `docs/product/ux-principles.md`;
- `docs/product/screens.md` (STUDIO 1, 2, 5, 9, 10 and 14);
- the design in `design/project/Studio-Song.dc.html`, `Studio-EmptyClip`, `Studio-ClipSettings`, `Studio-ClearClip`, `Studio-Undo`, `Studio-CopyClip`, plus the `Header`, `Knobs`, `PadGrid` and `Keys` components and `fm1.css`;
- `docs/engineering/model.md`, `core/model/model.h`, `firmware/src/app/song_io.*`, `live_bridge.*`, `ui_live.c`, `core/model/undo_stack.h`, and `reports/T3.md` / `T6i.md`.

**Ownership:** new `firmware/src/app/ui_studio.c/.h`; `song_io.*`; `live_bridge.*` (to retire the project mapping); small hooks in `ui.c` (view dispatch, the default view, button routing); new tests and scenarios.

## Decisions (coordinator)
- **The song is the edited object from now on.** STUDIO opens on the new song home, not on X0X's pattern views.
- **The project-to-song mapping runs only once,** when no stored song exists, to seed the song from the X0X project. The pattern-hash remap from T6i is removed.
- **X0X's views stay reachable** for sound design (sound pages, FX, MIX, GLOBAL, SONG, PERF) through their existing buttons. The 909/303 pattern editors are reachable only as "classic" pages, until T7b replaces clip editing.
- **The song's `bpm_x10` is the tempo** in both modes. X0X's project tempo follows the song.

## Build
1. **STUDIO 1, the song home.** The header shows STUDIO, the song name, the transport, BPM and bar.beat. Below it: a song card, then the role × clip grid (5 rows in role colours; cells show empty, loaded or playing, and the cursor), then the knob strip.
   - SELECT moves the cursor between rows and roles; ALGORITHM moves the cursor along a row.
   - SEL launches the clip under the cursor, with its quantize; transport rules as in LIVE.
   - The keys mirror the grid: a key press selects and launches that clip.
   - Follow the design at 1:1.
2. **STUDIO 2, the empty slot.** The cursor on an empty slot shows the empty-slot card ("REC records · SEQ steps it in"). SEQ creates an empty clip (`used=1`, length 16, the role's default) and opens it. T7b provides the editor; until then SEQ creates the clip and shows STUDIO 5. REC is T7b too; show the hint now.
3. **STUDIO 5, clip settings.** Length (1–64), Mode (Loop/Once) and Launch (beat, bar, end of clip) on K1–K3, swing on K4, following the design. Changes mark the song dirty.
4. **STUDIO 9, clear clip.** HOME + hold a pad (or SEL on the cursor) for 1.5 s. A ring fills and names the clip; on completion the clip clears, with an undo entry. Releasing early cancels.
5. **STUDIO 14, copy clip.** Hold a loaded pad, and the empty keys blink slowly as targets. Tap a target to copy the source clip there (same role only in v1), with an undo entry.
6. **STUDIO 10, undo and redo.** HOME+REC undoes and HOME+PLAY redoes. A toast names the action. Use `song_io`'s undo (T6c ring). "Can't undo" shows its own toast.
7. **LIVE uses the same song,** with no remap. Entering LIVE from STUDIO keeps what's playing (the clip engine keeps running). This settles the open question in `docs/open-questions.md`: clips carry on across mode changes. Update the open question as resolved by this coordinator decision, flagged for the owner to confirm.

## Tests (write first)
Scenarios with goldens (PNG at 1:1, dump and events) for:
- boot to the song home;
- moving the cursor and launching;
- an empty slot, then SEQ, then a clip created with settings shown;
- changing the length and mode;
- clear with the ring, then undo, then redo;
- copy to an empty slot, then undo;
- entering LIVE with clips still playing, then leaving with them still playing;
- a song autosaved, rebooted, and the same song back.

Run `make unit-core scenarios screens-lint parity check fw`, plus `tools/check_loader.sh build/x0x.fwsc`. List every new PNG for the reviewer.

## Done
- Small commits with the attribution lines.
- `reports/T7a.md`.
- `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T7a.md`.
