#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Ported into fm1-rocket-os from Felucca (tools/gen_aa_font.py) for T2b: the preset
# "x0x" matches the X0X UI's faces (S labels, M values/headers, L big numerals).
"""Pre-rasterise the Inter Tight TTF into anti-aliased 4-bit alpha glyph tables for the UI.

  gen_aa_font.py OUT.h --preset x0x

Output (a C header; the glyph / font types live in the renderer, firmware/src/gfx.c):
  AF_<N>_DATA   4-bit alpha, trimmed to the ink box, rows back to back (high nibble first)
  AF_<N>_G      aag_t {off, adv (1/16 px), bx, by, bw, bh} per glyph and phase
  AF_<N>_KERN / _KD  kerning pairs (key = a<<8|b, sorted) and their deltas in 1/16 px
  AF_<N>_EX     the codes of the glyphs past the ASCII range
  AF_<N>        aafont_t
  AF_<N>_CAP_Y / AF_<N>_CAP_B   the first / last row of 'H' (or '0'): the band text is centred by
"""
import argparse
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aa_raster as ar  # noqa: E402

FONTS = Path(__file__).resolve().parents[1] / "assets" / "fonts"
EXTRAS = [0xA9, 0xB0, 0xB7, 0xC4, 0xD6, 0xDC, 0xE4, 0xF6, 0xFC, 0x2026]  # (c) deg . A: O: U: a: o: u: (Latin-1), ellipsis
CODE = {0x2026: 0x85}                  # characters outside Latin-1 -> their one-byte code in the firmware
PHASES, PHASES_L = 4, 2                # horizontal phases per glyph (aa_raster.raster_font): S M, L
L_CHARS = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:.-+/%#"   # the big face draws numbers and the odd word


def preset(name):
    if name == "x0x":
        v = str(FONTS / "InterTight[wght].ttf")   # SIL OFL 1.1, Google Fonts
        return [("S", v + "@400", 12, (32, 126), EXTRAS, 0.0, PHASES),
                ("M", v + "@500", 15, (32, 126), EXTRAS, 0.0, PHASES),
                ("L", v + "@600", 28, (32, 32), [ord(c) for c in L_CHARS[1:]], 0.0, PHASES_L)]
    raise SystemExit(f"unknown preset {name}")


def parse_face(s):
    name, _, rest = s.partition("=")
    font, px, rng = rest.rsplit(":", 2)
    extras = []
    if "+" in rng:
        rng, ex = rng.split("+", 1)
        extras = [int(x, 0) for x in ex.split(",") if x]
    a, b = rng.split("-")
    return (name, font, int(px), (int(a), int(b)), extras, None, PHASES)


def emit(face, tracking, gamma, kern_min):
    name, font, px, (first, last), extras, ftrack, phases = face
    chars = [chr(c) for c in range(first, last + 1)] + [chr(c) for c in extras]
    r = ar.raster_font(font, px, chars, tracking if ftrack is None else ftrack, gamma, True, kern_min, phases)
    data, table, lines = bytearray(), [], []
    for ch in chars:
        adv16, ph = r["glyphs"][ch]
        off = len(data)
        for bx, by, bw, bh, b in ph:
            table.append((off, adv16, bx, by, bw, bh))
            data += b
        table += [(off,) + table[-1][1:]] * (phases - len(ph))   # one phase (aa_raster.FIXED): the same bitmap
    assert len(data) < 65536, "glyph data over 64 KiB: split the face"
    kern = sorted(r["kern"].items())
    keys = [(ord(a) << 8) | ord(b) for (a, b), _ in kern if ord(a) < 256 and ord(b) < 256]
    deltas = [d for (a, b), d in kern if ord(a) < 256 and ord(b) < 256]
    order = sorted(range(len(keys)), key=lambda i: keys[i])
    keys, deltas = [keys[i] for i in order], [deltas[i] for i in order]
    psh = phases.bit_length() - 1
    lines.append(f"/* {name}: {os.path.basename(font.split('#')[0])} {px}px, h {r['h']}, baseline {r['asc']}, "
                 f"{len(chars)} glyphs x {phases} phases, {len(data)} B data */")
    lines.append(ar.c_array(f"AF_{name}_DATA", "uint8_t", list(data), 24, "0x{:02x}"))
    lines.append(f"static const aag_t AF_{name}_G[{len(table)}] = {{")
    for off, adv, bx, by, bw, bh in table:
        lines.append(f"    {{{off}, {adv}, {bx}, {by}, {bw}, {bh}}},")
    lines.append("};")
    if keys:
        lines.append(ar.c_array(f"AF_{name}_KERN", "uint16_t", keys, 12))
        lines.append(ar.c_array(f"AF_{name}_KD", "int8_t", deltas, 24))
    ex = ", ".join(str(CODE.get(c, c)) for c in extras) or "0"
    lines.append(f"static const uint8_t AF_{name}_EX[{max(1, len(extras))}] = {{{ex}}};")
    lines.append(f"static const aafont_t AF_{name} = {{{r['h']}, {r['asc']}, {px}, {first}, {last}, {len(extras)}, {psh}, "
                 f"{len(keys)}, AF_{name}_G, AF_{name}_DATA, {'AF_%s_KERN' % name if keys else '0'}, "
                 f"{'AF_%s_KD' % name if keys else '0'}, AF_{name}_EX, 0}};")
    cap = "H" if "H" in chars else ("0" if "0" in chars else None)
    if cap:
        c = table[chars.index(cap) * phases]
        lines.append(f"#define AF_{name}_CAP_Y {c[3]}   /* the ink of {cap}: rows {c[3]} .. {c[3] + c[5] - 1} of the line */")
        lines.append(f"#define AF_{name}_CAP_B {c[3] + c[5] - 1}")
    cost = {"data": len(data), "table": 8 * len(table), "kern": 3 * len(keys), "extras": len(extras),
            "glyphs": len(chars), "phases": phases, "px": px}
    cost["total"] = cost["data"] + cost["table"] + cost["kern"] + cost["extras"]
    return "\n".join(lines) + "\n", cost


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--preset", default="x0x")
    ap.add_argument("--face", action="append", default=[])
    ap.add_argument("--tracking", type=float, default=0.0, help="extra advance per glyph in em")
    ap.add_argument("--gamma", type=float, default=1.0, help="coverage curve; <1 thickens light-on-dark text")
    ap.add_argument("--kern-min", type=int, default=1, help="smallest kerning pair kept, in 1/16 px (0 = no kerning)")
    a = ap.parse_args()
    faces = ([parse_face(s) for s in a.face] if a.face else []) or preset(a.preset)
    out = ["/* generated by tools/gen_aa_font.py: 4-bit alpha glyphs (Inter Tight, SIL OFL 1.1, "
           "Copyright 2022 The Inter Project Authors; see LICENSES/OFL-InterTight.txt) */",
           "#pragma once", "#include <stdint.h>", ""]
    total = 0
    for f in faces:
        text, c = emit(f, a.tracking, a.gamma, a.kern_min)
        out.append(text)
        total += c["total"]
        print(f"face {f[0]}: {c['px']}px {c['glyphs']} glyphs x {c['phases']}  "
              f"data {c['data']} B + table {c['table']} B + kern {c['kern']} B = {c['total']} B")
    Path(a.out).write_text("\n".join(out))
    print(f"fonts total {total} B  -> {a.out}")


if __name__ == "__main__":
    main()
