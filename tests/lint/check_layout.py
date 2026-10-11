#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""T2b screen layout lint.

Reads the log gfx.c writes in a host build with -DX0X_LINT (one record per line):
  B <w> <h>                              a canvas cv_begin just cleared
  T <x0> <y0> <x1> <y1> <px> <col> <s>   a text draw: its ink box in canvas coordinates,
                                          the face size in px, the colour and the string

A frame is taken to start at the header canvas (240 x 20); its canvases are the header, the
main area and the knob strip, and any overlay. The checker reports, per frame:
  * text whose ink box leaves its canvas (0..w / 0..h): clipped by its own region;
  * two text ink boxes that overlap (an identical duplicate draw of the same box is not an overlap);
  * a face smaller than MIN_PX (the provisional RC0 minimum, 12 px).

Findings that recur in every frame are reported once. Exit non-zero on any finding.

  tests/lint/check_layout.py [LOG]
"""
import sys

MIN_PX = 12
HDR_H = 20


def parse(path):
    groups, cur = [], None
    for ln in open(path, "r", errors="replace"):
        ln = ln.rstrip("\n")
        if ln.startswith("B "):
            p = ln.split()
            cur = {"w": int(p[1]), "h": int(p[2]), "events": []}
            groups.append(cur)
        elif ln.startswith("T ") and cur is not None:
            p = ln.split(" ", 7)
            if len(p) >= 8:
                x0, y0, x1, y1, px, col = (int(p[i]) for i in range(1, 7))
                if x1 > x0 and y1 > y0:
                    cur["events"].append(("T", x0, y0, x1, y1, px, col, p[7]))
        elif ln.startswith("R ") and cur is not None:
            p = ln.split()
            if len(p) >= 6:
                x, y, w, h, col = (int(p[i]) for i in range(1, 6))
                if w > 0 and h > 0:
                    cur["events"].append(("R", x, y, x + w, y + h, col))
    return groups


def visible_texts(g):
    """drop a text whose centre a later rectangle covers (a big readout drawn over a list strip)"""
    ev = g["events"]
    rects = [(i, e) for i, e in enumerate(ev) if e[0] == "R"]
    out = []
    for i, e in enumerate(ev):
        if e[0] != "T":
            continue
        _, x0, y0, x1, y1, px, col, s = e
        cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
        if any(r[1] <= cx < r[3] and r[2] <= cy < r[4] for j, r in rects if j > i):
            continue
        out.append((x0, y0, x1, y1, px, col, s))
    return out


def frames(groups):
    """split the canvas stream into frames at each header canvas (240 x 20)"""
    out, cur = [], []
    for g in groups:
        if g["w"] == 240 and g["h"] == HDR_H and cur:
            out.append(cur)
            cur = []
        cur.append(g)
    if cur:
        out.append(cur)
    return out


def check_frame(groups):
    clips, overlaps, tiny = [], [], []
    for g in groups:
        w, h = g["w"], g["h"]
        seen, texts = set(), []
        for t in visible_texts(g):
            if t[:6] in seen:
                continue                       # the same box drawn twice in one canvas is not an overlap
            seen.add(t[:6])
            texts.append(t)
            if t[0] < 0 or t[1] < 0 or t[2] > w or t[3] > h:
                clips.append((w, h, t))
            if t[4] < MIN_PX:
                tiny.append((w, h, t))
        for i in range(len(texts)):
            for j in range(i + 1, len(texts)):
                a, b = texts[i], texts[j]
                if a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]:
                    overlaps.append((w, h, a, b))
    return clips, overlaps, tiny


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build/screens/lint.log"
    groups = parse(path)
    fs = frames(groups)
    all_clips, all_overlaps, all_tiny = {}, {}, {}
    canvases = sum(len(f) for f in fs)
    for f in fs:
        c, o, t = check_frame(f)
        for it in c:
            all_clips.setdefault((it[0], it[1], it[2][:6], it[2][6]), it)
        for it in o:
            all_overlaps.setdefault((it[0], it[1], it[2][:6], it[3][:6], it[2][6], it[3][6]), it)
        for it in t:
            all_tiny.setdefault((it[0], it[1], it[2][:6], it[2][6]), it)
    clips, overlaps, tiny = list(all_clips.values()), list(all_overlaps.values()), list(all_tiny.values())

    def show(name, items, fmt):
        print(f"{name}: {len(items)}")
        for it in items[:60]:
            print("   ", fmt(it))
        if len(items) > 60:
            print(f"    ... and {len(items) - 60} more")

    show("clipped", clips, lambda c: f"canvas {c[0]}x{c[1]}: box {c[2][0]},{c[2][1]}..{c[2][2]},{c[2][3]} '{c[2][6]}'")
    show("overlap", overlaps, lambda o: f"canvas {o[0]}x{o[1]}: '{o[2][6]}' {o[2][:4]} vs '{o[3][6]}' {o[3][:4]}")
    show("tiny", tiny, lambda t: f"canvas {t[0]}x{t[1]}: {t[2][4]}px '{t[2][6]}'")
    n = len(clips) + len(overlaps) + len(tiny)
    print(f"layout lint: {len(fs)} frames, {canvases} canvases, {n} findings")
    return 1 if n else 0


if __name__ == "__main__":
    sys.exit(main())
