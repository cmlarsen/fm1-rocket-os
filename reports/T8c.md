# T8c: name entry (port of Felucca's `ui_name.c`)

Core lane for T8 ([build-plan.md](../docs/engineering/build-plan.md#lanes-v3-2026-10-11)). New
files only, plus the `make unit-name` target: `firmware/src/app/ui_name.{c,h}`,
`tests/host/ui_name_test.c`, `Makefile`, this report. Nothing shared was touched, and nothing was
flashed.

The port is of Felucca's `firmware/src/ui_name.c` (GPL-3.0-only). Its SPDX header and copyright
are kept and a **"Ported from Felucca"** line was added, adapted to X0X's input events and gfx.

## What it does

A `name_edit_t` holds a name of up to `SONG_NAME_LEN - 1` (**11**) characters from the set
**`A-Z 0-9 space - _ . !`**, plus a cursor (`0 .. len`). `name_edit_begin` seeds it (dropping
disallowed characters); `name_edit_input` consumes one X0X input frame and returns
`NAME_EDITING`, `NAME_DONE` or `NAME_CANCELLED`; `name_edit_result` writes the name trimmed of
leading/trailing spaces; `name_edit_draw` renders it.

The one grammar (`docs/product/interaction-model.md`), mapped from Felucca:

| X0X | Felucca | does |
|---|---|---|
| F3..G5 **white keys** | same | phone-style multi-tap: a tap types the group's first character, another tap of the same key within 800 ms the next (cycling), another key or the timeout keeps it |
| **F# / G# / A# / C#** black keys | same | cursor left / SPACE / cursor right / DELETE-before-cursor (held: repeats) |
| **D#** black key | same | ABC ↔ 123 (in 123 mode each white key is one character) |
| **SELECT** turns | KNOB 1 | move the cursor |
| **ALGORITHM** turns | KNOB 2 | change the character under the cursor (wraps through the set) |
| **K3** turns | KNOB 3 | a random name (adjective + noun; PROJECT in Felucca) |
| **SEL** press | OCT+ | confirm (`NAME_DONE`) |
| **HOME** tap | OCT− | cancel, restoring the original |
| **HOME** hold ≥ 800 ms | — | clear the name (the release does not cancel) |
| **OCT+ / OCT−** | same | kept as aliases so the port stays a port |

Adaptations from Felucca, and why:
- **Allowed set.** Felucca's set is ` ABC…Z 0-9 - . _ / # +`; the task's is `A-Z 0-9 space - _ . !`.
  `/ # +` were dropped and `!` added, in both the multi-tap groups' targets and the ALGORITHM
  set. `name_insert` and `name_edit_begin` gate every character through `name_char_allowed`, so
  nothing outside the set can enter the model. The 123-mode row is 16 allowed characters.
- **Input.** Felucca read a `notes` bitmask, `fm1_in.notes` and `panel_enc`/`fm1_ms`; here a
  `name_event_t { btn, keys, enc[NE], ms }` carries exactly what X0X's `input()` reads
  (`plat_buttons`, `plat_keys`, `plat_enc`, `plat_ms`). Edges and held repeats are derived in the
  module from the previous frame, so the caller only samples.
- **One random seed that is fixed** (`0x92D68CA2`), not `fm1_ms`, so a repeated host run is
  byte-identical.
- **Inclusion model.** Like Felucca's `ui_name.c` (included by `ui_input.c`), this file draws
  through the `cv_*` calls and the `AF_S/AF_M/AF_L` faces that **gfx.c defines as statics**, so it
  is #included into the one translation unit that holds gfx.c (X0X's `ui.c`, once T8i wires it
  in). The host test does the same.

## Drawing

`name_edit_draw(e, ms)` renders `design/project/Studio-Rename.dc.html` at 1:1 in two canvas bands
(the gfx canvas holds 240×144 rows, so 0–109 then 110–239, each blitted in one transfer):

- a 24 px header: a `STUDIO` chip, `NAME`, and the battery;
- the name card (`4,30 232×76`, `TOK_SURF`): the caption `RENAME SONG <len> OF 12` and the name in
  `AF_L`, the character at the cursor on a `TOK_RAISE` box in `TOK_ACCENT`, and a blinking
  cursor bar;
- four hint cards (`WHITE KEYS / TAP AGAIN / BLACK KEYS / K3`), 2×2;
- a footer with `SEL save` and `HOME cancel` keycap chips.

Deviations from the artboard, all deliberate:
- the footer shows **SEL / HOME** (the task's grammar) rather than the artboard's OCT+/OCT−, which
  still work as aliases;
- the header's transport block (`▶ / ■` + BPM) is omitted: the name editor carries no transport
  state (the caller owns it), so only the mode chip, the layer and the battery are drawn;
- the black-keys hint reads **`left space right del`**: the T2b Inter Tight faces have no `←`/`→`
  glyph (the extras are the ellipsis and a few symbols), and `AF_M` is too wide for the card.

## Tests (`make unit-name`)

The scenario runner cannot show this screen yet — nothing includes `ui_name.c` until T8i wires it
into `ui.c`, and T8c may not touch the inherited `host/x0x_host.c` — so, as the task allows, the
render is done by the host test instead of a `tests/scenarios/screens_name.x0x` shot.

Built with `-Werror`, run, then rebuilt under `-fsanitize=address,undefined` and run again. Both
pass on the Mac. `tests/host/ui_name_test.c` covers:

- begin / length / cursor / `name_edit_result`, and the leading/trailing-space trim;
- `name_edit_begin` filters disallowed characters (`A/B#C+` → `ABC`);
- typing: a tap, a second tap replacing the letter, a third key committing the cycled letter and
  moving on, cycling back around a group, the 800 ms timeout committing and starting fresh, SPACE,
  DELETE, and the ABC→123 toggle with one-tap-one-character;
- cursor move and clamping (below 0, past `len`), and ALGORITHM changing the character and wrapping
  (`A` → SPACE → `!` → SPACE); 90 steps assert the result is always in the allowed set;
- SEL confirms; HOME taps back and restores the original; a HOME hold clears and its release does
  not cancel; the 11-character maximum refuses one more;
- a held DEL repeats (one delete at the press, another at `NAME_REP_MS`, then every `NAME_RATE_MS`);
- K3's random name is non-empty, ≤ 11, allowed, an adjective + a space + a noun, and never the name
  already in the field.

### Render to PNG (for the Opus reviewer, at 1:1)

Written by the test into `build/host/ui_name/`:

| PNG | shows |
|---|---|
| `editing_mid.png` | `SUNDAY DUB`, the cursor on the final **B** (a raise box + accent bar), caption `RENAME SONG 10 OF 12` |
| `empty.png` | an empty name, the cursor bar at the left, caption `0 OF 12` |
| `full.png` | `909 4 THE F` — the input `909 4 THE FLOOR` truncated to 11 of 12, cursor bar at the end |
| `random.png` | K3's random name (`SOLAR ECHO`), ≤ 11, caption follows |

## Device build (deltron.local, JieLi clang 4.0.1, pi32v2)

Compiled the way `tools/build.py` compiles X0X's units, in a throwaway translation unit that holds
gfx.c + ui_name.c + `lcd_sync`/`lcd_blit` stubs:

```
clang -target pi32v2 -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function \
      -mcpu=r3 -mfprev1 -ffp-contract=off -Ifirmware/hal -Ifirmware/src -I. -c t8c_dev.c
=> DEVICE COMPILE OK
```

`make fw` also passes unchanged (image 464120 B; RAM 95048/98304 B; pool 328052/344064 B). Nothing
in the firmware includes `ui_name.c` yet — T8i does that — so `make fw` is a regression check, and
the compile above is the proof that T8c's own code builds for the pi32v2.

## What T8i needs

- **Include `ui_name.c`** into the one translation unit that holds gfx.c (after it), in `ui.c`.
  The state lives in a caller-owned `name_edit_t`.
- **Build `name_event_t`** from `plat_buttons()`, `plat_keys()`, `plat_enc(EN_*)` and `plat_ms()`,
  and route SEL / HOME before the rest of `input()` while the editor is open.
- **Heads-up for T8i:** `core/model/model.h` and `firmware/src/app/plat.h` both
  `#define NKEYS` (one `27u`, one `27`), so a single TU that includes both fails with
  `-Werror`. T8c keeps the length in `ui_name.h` (`NAME_MAX`, asserted equal to `SONG_NAME_LEN - 1`
  only in the host test) to avoid it. T8i will hit this when it includes model.h in `ui.c`;
  it needs one `#undef NKEYS` (or the coordinator moves one definition).
- **Felucca's category (KNOB 3)** has no v1 equivalent (song names have no category), so K3 is the
  random name as the artboard shows.

## Determinism

No heap, no `double`, no 64-bit division, no wall clock (time is the frame's `ms`). The ALGORITHM
set, the multi-tap groups and the random seed are fixed, so a repeated host run renders identical
PNGs (verified by running `make unit-name` twice).
