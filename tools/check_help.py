#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""T11h: every button the interaction model gives a gesture has a help card.

SYSTEM 2's cards live in one table in firmware/src/app/ui_help.c (HELP_CARD[context][button]).
This tool reads the button names out of the interaction model's gesture tables
(docs/product/interaction-model.md) and fails if any of them has no card. The model's
"GLO + any button" row means every panel button needs a card in every context.

  tools/check_help.py [--file UI_HELP_C] [--model INTERACTION_MODEL]

Exit 0 when every gesture-table button has a card, 1 otherwise.
"""
import argparse
import os
import re
import sys

# the panel's buttons, by the names the interaction model uses, mapped to the B_* enum in plat.h.
# The model prints the second button SEL in one place and SCL in another: both are B_SEL.
BUTTONS = {
    "FX": "B_FX",
    "SEL": "B_SEL",
    "SCL": "B_SEL",
    "ENV": "B_ENV",
    "LFO": "B_LFO",
    "EDIT": "B_EDIT",
    "GLO": "B_GLO",
    "HOME": "B_HOME",
    "SAVE": "B_SAVE",
    "ARP": "B_ARP",
    "SEQ": "B_SEQ",
    "PLAY": "B_PLAY",
    "REC": "B_REC",
    "OCT-": "B_OCTDN",
    "OCT\u2212": "B_OCTDN",   # the model uses U+2212 (minus sign)
    "OCT+": "B_OCTUP",
}
ALL_B = sorted(set(BUTTONS.values()))
LABEL = {v: k for k, v in BUTTONS.items()}
LABEL["B_SEL"] = "SEL/SCL"

# a markdown table is a "gesture table" when one of its header cells is one of these
HDR_KEYS = ("gesture", "task", "control", "enter", "leave", "layer", "keys do", "purpose", "shows")

TOKEN_RE = re.compile(r"(?<![A-Za-z0-9])(FX|SEL|SCL|ENV|LFO|EDIT|GLO|HOME|SAVE|ARP|SEQ|PLAY|REC|OCT[+\-\u2212])(?![A-Za-z0-9])")


def read(path):
    with open(path, "r", encoding="utf-8") as fp:
        return fp.read()


def model_buttons(text):
    """the buttons that appear in the interaction model's gesture tables"""
    found = set()
    table = None                       # None, "hdr" (collecting the header), or True/False
    for line in text.splitlines():
        s = line.strip()
        if s.startswith("|"):
            cells = [c.strip() for c in s.strip("|").split("|")]
            if table is None:
                table = cells
            elif set("".join(cells)) <= set("-: "):
                # the header separator: decide now whether this table is a gesture table
                hdr = " ".join(table).lower()
                is_gesture = any(k in hdr for k in HDR_KEYS)
                table = is_gesture
            elif table is True:
                for c in cells:
                    for m in TOKEN_RE.findall(c):
                        found.add(BUTTONS[m])
        else:
            table = None
    # "GLO + any button" means every button needs a card
    if re.search(r"GLO\s*\+\s*any button", text, re.IGNORECASE):
        found.update(ALL_B)
    return found


def card_buttons(text):
    """the (context, B_*) pairs the card table in ui_help.c initialises"""
    cards = {}
    ctx = None
    for line in text.splitlines():
        m = re.search(r"\[(HELP_CTX_[A-Z]+)\]\s*=\s*\{", line)
        if m:
            ctx = m.group(1)
            cards.setdefault(ctx, set())
            continue
        if ctx:
            for m in re.finditer(r"\[(B_[A-Z0-9]+)\]\s*=", line):
                cards[ctx].add(m.group(1))
    return cards


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--file", default=os.path.join(root, "firmware/src/app/ui_help.c"))
    ap.add_argument("--model", default=os.path.join(root, "docs/product/interaction-model.md"))
    args = ap.parse_args()

    need = model_buttons(read(args.model))
    cards = card_buttons(read(args.file))
    if not cards:
        print("check_help: no card table found in %s" % args.file)
        return 1

    # the task writes a card for every button in STUDIO, LIVE and SONGS, so each context that
    # the table declares must cover every gesture-table button.
    missing = []
    for b in sorted(need):
        gaps = [ctx for ctx, b_set in cards.items() if b not in b_set]
        if gaps:
            missing.append((b, gaps))
    if missing:
        print("check_help: %d gesture-table button(s) have no card:" % len(missing))
        for b, gaps in missing:
            print("    %s (%s): missing in %s" % (LABEL.get(b, b), b, ", ".join(sorted(gaps))))
        return 1

    ctxs = ", ".join("%s=%d" % (c, len(v)) for c, v in sorted(cards.items()))
    print("check_help: %d gesture-table button(s), all have cards (%s)" % (len(need), ctxs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
