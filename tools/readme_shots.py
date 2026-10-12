#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Regenerate the README's screen images from the scenario runs (TDOC).

Every image in docs/img/readme/ is a real firmware screenshot: the readme_*.x0x scenarios
run the steps the README describes and take a `shot` at each one, and this script copies the
chosen shots up to 480x480 with nearest-neighbour scaling (the 240x240 panel at 2x, so the
pixels stay crisp).

    make scenarios                 # writes build/scenarios/readme_*/<shot>.png
    tools/readme_shots.py          # copies the chosen shots into docs/img/readme/

Run it with no arguments from the repo root. It fails (non-zero) if a source shot is missing,
so a scenario that stops taking a shot (or a stale checkout) cannot pass silently.
"""
import os
import sys

from PIL import Image

SRC = "build/scenarios"
DST = "docs/img/readme"
SCALE = 2                      # 240x240 -> 480x480, nearest-neighbour

# (output name, scenario, source shot). The README's sections name these files.
SHOTS = [
    # The two modes
    ("modes-studio-home.png",    "readme_modes",          "readme_studio_home.png"),
    ("modes-live-enter.png",     "readme_modes",          "readme_live_enter.png"),
    ("modes-live-pads.png",      "readme_modes",          "readme_live_pads.png"),
    # Record a clip
    ("record-empty.png",         "readme_record_clip",    "readme_record_empty.png"),
    ("record-recording.png",     "readme_record_clip",    "readme_record_rec.png"),
    ("record-clip.png",          "readme_record_clip",    "readme_record_clip.png"),
    ("record-notes.png",         "readme_record_clip",    "readme_record_notes.png"),
    # Erase a clip
    ("erase-ring.png",           "readme_erase_clip",     "readme_erase_ring.png"),
    ("erase-undo.png",           "readme_erase_clip",     "readme_erase_undo.png"),
    ("erase-locked.png",         "readme_erase_clip",     "readme_erase_locked.png"),
    # Play clips together, and in order
    ("together-roles.png",       "readme_play_together",  "readme_together_live.png"),
    ("together-take.png",        "readme_play_together",  "readme_together_take.png"),
    # LIVE as a launchpad
    ("live-play.png",            "readme_live",           "readme_live_play.png"),
    ("live-fx.png",              "readme_live",           "readme_live_fx.png"),
    ("live-setup.png",           "readme_live",           "readme_live_setup.png"),
    ("live-assign.png",          "readme_live",           "readme_live_assign.png"),
    ("live-take.png",            "readme_live",           "readme_live_take.png"),
    ("live-clear-take.png",      "readme_live",           "readme_live_clear_take.png"),
    # Cheat sheet and help
    ("help-sheet.png",           "readme_help",           "readme_help_sheet.png"),
    ("help-card.png",            "readme_help",           "readme_help_card.png"),
]


def main():
    missing = []
    os.makedirs(DST, exist_ok=True)
    for out_name, scenario, shot in SHOTS:
        src = os.path.join(SRC, scenario, shot)
        if not os.path.exists(src):
            missing.append(src)
            continue
        im = Image.open(src).convert("RGB")
        w, h = im.size
        im = im.resize((w * SCALE, h * SCALE), Image.NEAREST)
        im.save(os.path.join(DST, out_name))
        print(f"  {out_name}  <-  {src}  ({w*SCALE}x{h*SCALE})")
    if missing:
        print("\nmissing shots (run `make scenarios` first):", file=sys.stderr)
        for m in missing:
            print(f"  {m}", file=sys.stderr)
        return 1
    print(f"readme shots: {len(SHOTS)} images in {DST}/")
    return 0


if __name__ == "__main__":
    sys.exit(main())
