# Pending T8 scenarios

These `.x0x` scripts are the scenarios T8 owes, written against the songs UI and the
X0X host runner. They are **not wired yet** and are deliberately kept out of
`tests/scenarios/` so `make scenarios` does not pick them up: the runner has no way to
reach the SONGS view until T8i adds the `ui.c` hook (`V_SONGS`, a SAVE long-hold, the
routing/draw/footer/leds) and the `song_io` slot ops (`songs_ops_t` in
`firmware/src/app/ui_songs.h`). Nothing here runs until then.

When T8i lands, `git mv tests/scenarios/pending/t8_*.x0x tests/scenarios/`, add the
`tests/golden/<name>/` directories with `GOLDEN_UPDATE=1`, view every PNG at 1:1, and
record the shots in the report.

## What the runner needs first

- **SAVE long-hold** opens SONGS (the `ui.c` hook). The scripts `press SAVE`, `wait 700`,
  `release SAVE` to enter, and `tap HOME` to leave.
- **New `expect` keys** the songs screens need (names for the reviewer, not final):
  `songslot N NAME|EMPTY`, `songcurrent N`, `songused N`, `songscreen NAME`,
  and `readkey`/`songclips N` for the menu and the delete ring.
- **The shots** are the ones the state machine already renders in
  `tests/host/ui_songs_test.c`; the scenario versions should show the same screens with
  real flash behind them.

## Files

| Scenario | Covers | Shots it must produce |
|---|---|---|
| `t8_songs.x0x` | the list, the storage meter, New > Empty and Starter, the 5-slot limit | `s01_list.png`, `s02_new.png`, `s03_empty_created.png`, `s04_starter_created.png`, `s05_five_max.png` |
| `t8_edit.x0x` | the song menu, rename (T8c's editor), copy | `s06_menu.png`, `s07_rename.png`, `s08_renamed.png`, `s09_copied.png` |
| `t8_delete.x0x` | delete with the hold ring, undo, reboot persistence, open + reboot | `s10_delete_ring.png`, `s11_deleted.png`, `s12_restored.png`, `s13_after_reboot.png` |
