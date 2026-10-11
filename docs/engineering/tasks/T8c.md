# T8c: name entry (port of Felucca's `ui_name.c`)

Core lane for T8 in [build-plan.md](../build-plan.md#lanes-v3-2026-10-11). **New files only**; T8 puts it into the songs UI later.

Read first:
- `CLAUDE.md` and the worker rules in `docs/engineering/build-plan.md`;
- Felucca's name entry: find it with `git ls-tree -r --name-only felucca/main | grep -i name`, then read it with `git show felucca/main:<path>`;
- `docs/product/screens.md` (STUDIO Rename) and `design/project/Studio-Rename.dc.html`, and `docs/product/interaction-model.md` (the one grammar: SELECT moves rows, SEL confirms, ALGORITHM moves the cursor or sets a value, HOME goes back);
- X0X's `firmware/src/gfx.*` and `firmware/src/ui_fonts.h` / `ui_tokens.h`, which T2b merged.

**Ownership (new files only):** `firmware/src/app/ui_name.{c,h}`, `tests/host/ui_name_test.c`, a `tests/scenarios/screens_name.x0x` shot if the runner can show the editor through a test hook (otherwise a host test renders it to PNG), and a `make unit-name` target appended to the Makefile.

## Behaviour
- **Model:** a name of up to `SONG_NAME_LEN - 1` characters (11) from the set `A–Z 0–9 space - _ . !`, plus a cursor.
- **Input:** ALGORITHM turns change the character under the cursor; SELECT turns move the cursor; the keys type letters directly when Felucca does that (port its mapping); SEL confirms; HOME cancels and restores the original; a long press on HOME clears the name.
- **API:** `name_edit_begin(e, initial)`, `name_edit_input(e, event)` returning DONE, CANCELLED or EDITING, `name_edit_result(e, out)`, and `name_edit_draw(e, canvas…)`, which uses the T2b fonts and tokens and follows the design at 1:1.
- Port Felucca's logic and keep its GPL header plus a "Ported from Felucca" line. Adapt it to X0X's input events and gfx.

## Tests
- Unit tests of the state machine: type, move, wrap, confirm, cancel restores, clear, maximum length, and the allowed characters only.
- A render to PNG: list the PNG in the report so the Opus reviewer can look at it.
- Run under `-fsanitize=address,undefined`.
- Compile on deltron.

## Done
- `make unit-name` passes.
- Small commits with the attribution lines.
- `reports/T8c.md`.
- `worker_done` with `--outcome`, `--files-modified`, `--report-path reports/T8c.md`.
