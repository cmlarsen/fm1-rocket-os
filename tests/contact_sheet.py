#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Contact sheet for `make screens` (T2a): every shot at 1:1 in a grid, its file name under it.

    tests/contact_sheet.py OUT.png SHOT.png [SHOT.png ...]

The sheet exists so a reviewer can look at every screen state at once; the images are never
scaled (a 240x240 shot is 240x240 in the sheet), because a scaled mockup hides legibility
problems (CLAUDE.md)."""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

COLS = 6
PAD = 6
LABEL_H = 14


def main(argv):
    if len(argv) < 2:
        print("usage: tests/contact_sheet.py OUT.png SHOT.png ...", file=sys.stderr)
        return 2
    out, shots = argv[0], sorted(argv[1:])
    font = ImageFont.load_default()
    imgs = [(os.path.basename(p), Image.open(p).convert("RGB")) for p in shots]
    cw = max([im.size[0] for _, im in imgs] + [1])
    ch = max([im.size[1] for _, im in imgs] + [1])
    rows = math.ceil(len(imgs) / COLS)
    cell_w, cell_h = cw + 2 * PAD, ch + LABEL_H + 2 * PAD
    sheet = Image.new("RGB", (COLS * cell_w, rows * cell_h), (18, 18, 20))
    d = ImageDraw.Draw(sheet)
    for i, (name, im) in enumerate(imgs):
        x = (i % COLS) * cell_w + PAD
        y = (i // COLS) * cell_h + PAD
        sheet.paste(im, (x, y))
        d.text((x, y + im.size[1] + 2), name, fill=(200, 200, 205), font=font)
    sheet.save(out)
    print(f"contact sheet: {out} ({len(imgs)} shots at 1:1, {COLS} columns)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
