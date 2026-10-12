#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Golden comparison for the host scenario runner (T2a).

    tests/golden.py OUTDIR GOLDENDIR          # diff OUTDIR against GOLDENDIR
    GOLDEN_UPDATE=1 tests/golden.py OUT GOLD  # write OUT's artefacts into GOLD

Every file in GOLDENDIR is checked against the same name in OUTDIR: *.png by decoded pixels,
*.json by parsed structure, anything else by text. Each *.wav named in GOLDENDIR/audio.json must
exist in OUTDIR; it is measured (frames,
peak, rms) and compared within loose bounds, so a DSP change does
not churn a golden by its exact bytes. The first mismatch of every file is printed; exit 1 on
any. A scenario without a golden directory is simply not compared (the caller skips it)."""
import json
import math
import os
import struct
import sys
import zlib

TXT_SKIP = {"midi_out.txt"}          # always written; not part of a golden (events*.txt is)
WAV_FRAMES_TOL = 0.01                # 1 % of the golden's frame count
WAV_DB_TOL = 1.5                     # peak / rms within 1.5 dB


def read_png(path):
    """decode a PNG to (w, h, bytes-per-pixel-channels(RGB), pixels); 8-bit RGB only"""
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")
    pos, idat, w, h, bd, ct = 8, b"", 0, 0, 0, 0
    while pos < len(data):
        ln = struct.unpack(">I", data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", body[:10])
        elif typ == b"IDAT":
            idat += body
        elif typ == b"IEND":
            break
    if bd != 8 or ct not in (2, 6):
        raise ValueError(f"unsupported PNG (bit depth {bd}, colour type {ct})")
    nch = 3 if ct == 2 else 4
    raw = zlib.decompress(idat)
    stride = w * nch
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _ in range(h):
        ft = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if ft == 1:
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                b = prev[i]
                c = prev[i - nch] if i >= nch else 0
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                pr = a if pa <= pb and pa <= pc else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out += line
        prev = line
    return w, h, nch, bytes(out)


def png_first_diff(golden, out):
    w0, h0, c0, p0 = read_png(golden)
    w1, h1, c1, p1 = read_png(out)
    if (w0, h0, c0) != (w1, h1, c1):
        return f"size {w1}x{h1}x{c1} != golden {w0}x{h0}x{c0}"
    bad = 0
    first = None
    for i in range(0, len(p0), c0):
        if p0[i:i + c0] != p1[i:i + c0]:
            bad += 1
            if first is None:
                px = i // c0
                first = (px % w0, px // w0, tuple(p1[i:i + c0]), tuple(p0[i:i + c0]))
    if bad:
        x, y, got, want = first
        return f"{bad} pixel(s) differ; first at ({x},{y}): got {got}, golden {want}"
    return None


def json_first_diff(golden, out, path=""):
    a = json.load(open(golden))
    b = json.load(open(out))
    return _jd(a, b, path)


def _jd(a, b, path):
    if type(a) is not type(b):
        return f"{path or '$'}: {type(a).__name__} vs {type(b).__name__}"
    if isinstance(a, dict):
        for k in sorted(set(a) | set(b)):
            if k not in a:
                return f"{path}.{k}: extra in output"
            if k not in b:
                return f"{path}.{k}: missing from output"
            r = _jd(a[k], b[k], f"{path}.{k}")
            if r:
                return r
        return None
    if isinstance(a, list):
        if len(a) != len(b):
            return f"{path}: {len(b)} items vs golden {len(a)}"
        for i, (x, y) in enumerate(zip(a, b)):
            r = _jd(x, y, f"{path}[{i}]")
            if r:
                return r
        return None
    if a != b:
        return f"{path}: {b!r} vs golden {a!r}"
    return None


def text_first_diff(golden, out):
    a = open(golden, errors="replace").read().splitlines()
    b = open(out, errors="replace").read().splitlines()
    for i in range(max(len(a), len(b))):
        x = a[i] if i < len(a) else "<end>"
        y = b[i] if i < len(b) else "<end>"
        if x != y:
            return f"line {i + 1}: got {y!r}, golden {x!r}"
    return None


def wav_metrics(path):
    """frames, peak (0..1) and rms (0..1) of a 16-bit WAV (any channel count)"""
    data = open(path, "rb").read()
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("not a WAV")
    pos, ch, bits, frames, peak, sumsq, total = 12, 1, 16, 0, 0.0, 0.0, 0
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        sz = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + sz]
        pos += 8 + sz + (sz & 1)
        if cid == b"fmt ":
            ch, bits = struct.unpack("<H", body[2:4])[0], struct.unpack("<H", body[14:16])[0]
        elif cid == b"data":
            n = len(body) // 2
            s = struct.unpack("<%dh" % n, body[:n * 2])
            for v in s:
                f = v / 32768.0
                if abs(f) > peak:
                    peak = abs(f)
                sumsq += f * f
                total += 1
    return {"frames": total // max(ch, 1), "peak": round(peak, 6),
            "rms": round(math.sqrt(sumsq / total), 6) if total else 0.0}


def cmp_audio(golden_json, outdir):
    """every WAV named in audio.json must exist in OUTDIR and match its golden metrics"""
    want = json.load(open(golden_json))
    problems = []
    actual = sorted(f for f in os.listdir(outdir) if f.endswith(".wav"))
    # T4 review: compare the expected and actual filename sets first. Iterating the WAVs on disk
    # let a missing expected file eliminate its own comparison (audio.json is skipped by the
    # ordinary file loop), so a scenario that stopped producing audio still passed.
    for name in sorted(want):
        if name not in actual:
            problems.append(f"{name}: MISSING from {outdir}")
    for name in actual:
        if name not in want:
            problems.append(f"{name}: no golden metric")
            continue
        got = wav_metrics(os.path.join(outdir, name))
        w = want[name]
        if abs(got["frames"] - w["frames"]) > max(64, w["frames"] * WAV_FRAMES_TOL):
            problems.append(f"{name}: {got['frames']} frames, golden {w['frames']}")
        for k in ("peak", "rms"):
            g, gd = max(got[k], 1e-9), max(w[k], 1e-9)
            db = 20.0 * math.log10(g / gd)
            if abs(db) > WAV_DB_TOL:
                problems.append(f"{name}: {k} {db:+.1f} dB (got {got[k]}, golden {w[k]})")
    return problems


def compare(outdir, golddir):
    fails = 0
    for name in sorted(os.listdir(golddir)):
        g = os.path.join(golddir, name)
        if name == "audio.json" or not os.path.isfile(g):
            continue
        o = os.path.join(outdir, name)
        if not os.path.exists(o):
            print(f"golden {name}: MISSING in {outdir}")
            fails += 1
            continue
        if name.endswith(".png"):
            r = png_first_diff(g, o)
        elif name.endswith(".json"):
            r = json_first_diff(g, o)
        else:
            r = text_first_diff(g, o)
        if r:
            print(f"golden {name}: {r}")
            fails += 1
    gaudio = os.path.join(golddir, "audio.json")
    if os.path.exists(gaudio):
        problems = cmp_audio(gaudio, outdir)
        if problems:
            print("golden audio: " + "; ".join(problems))
            fails += 1
    if fails:
        print(f"GOLDEN FAIL ({fails} file(s)) against {golddir}")
        return 1
    print(f"golden ok: {outdir} matches {golddir}")
    return 0


def update(outdir, golddir):
    os.makedirs(golddir, exist_ok=True)
    wavs = {}
    for name in sorted(os.listdir(outdir)):
        src = os.path.join(outdir, name)
        if not os.path.isfile(src) or name in TXT_SKIP or name == "audio.json":
            continue
        if name.endswith((".png", ".json", ".txt")):
            open(os.path.join(golddir, name), "wb").write(open(src, "rb").read())
        if name.endswith(".wav"):
            wavs[name] = wav_metrics(src)
    if wavs:
        with open(os.path.join(golddir, "audio.json"), "w") as f:
            json.dump(wavs, f, indent=2, sort_keys=True)
            f.write("\n")
    print(f"golden updated: {golddir}")
    return 0


def main(argv):
    if len(argv) != 2:
        print("usage: tests/golden.py OUTDIR GOLDENDIR", file=sys.stderr)
        return 2
    outdir, golddir = argv
    if os.environ.get("GOLDEN_UPDATE") == "1":
        return update(outdir, golddir)
    if not os.path.isdir(golddir):
        print(f"golden: no golden directory {golddir} (not compared)")
        return 0
    return compare(outdir, golddir)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
