#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Tests for tools/check_loader.sh.

It must select the loader the way the device's ota_ufw does — the entry of type 100, not a file
merely named "ota.bin" — and fail closed on an ambiguous or corrupt package.

  python3 tests/check_loader_test.py [build/x0x.fwsc]

Builds the negative packages in memory from the given package (crafted: a decoy named ota.bin
beside a different type-100 entry; two type-100 entries; a broken header CRC) and runs the real
script on each. Exit 0 if every case behaves; 2 if there is no package to test.
"""
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SRC / "tools"))
import fm1pkg_make as F  # noqa: E402

CHECK = str(SRC / "tools" / "check_loader.sh")
BLOCKS, BLK, KEEP = 20, 0x30, 0x2F


def strip(raw):
    out = bytearray()
    for i in range(BLOCKS):
        out += raw[i * BLK:i * BLK + KEEP]
    return bytearray(out + raw[BLOCKS * BLK:])


def insert_markers(logical, markers):
    out = bytearray()
    for i in range(BLOCKS):
        out += logical[i * KEEP:(i + 1) * KEEP] + bytes([markers[i]])
    return bytes(out + logical[BLOCKS * KEEP:])


def build_logical(base, entries):
    """base: the package's logical image (data area kept); entries: decrypted 0x50-byte entries"""
    L = bytearray(base)
    enc_e = bytearray()
    for e in entries:
        x = bytearray(e)
        F.enc(x, 0, 0x50, 0xFFFF)
        enc_e += x
    L[0x40:0x40 + len(enc_e)] = enc_e
    hdr = bytearray(L[0:0x40])
    F.enc(hdr, 0, 0x40, 0xFFFF)
    struct.pack_into("<H", hdr, 8, len(entries))
    struct.pack_into("<H", hdr, 2, F.crc16(enc_e))
    struct.pack_into("<H", hdr, 0, F.crc16(hdr[2:0x40]))
    F.enc(hdr, 0, 0x40, 0xFFFF)
    L[0:0x40] = hdr
    return L


def run(path):
    return subprocess.run(["sh", CHECK, str(path)], capture_output=True, text=True).returncode


def main():
    pkg = Path(sys.argv[1]) if len(sys.argv) > 1 else SRC / "build" / "x0x.fwsc"
    if not pkg.exists():
        print(f"check_loader_test: no package at {pkg} (build first)")
        return 2

    fails = 0

    def want(what, rc, good):
        nonlocal fails
        ok = (rc == good) if good is not None else (rc != 0)
        print(f"  {'ok  ' if ok else 'FAIL'} {what} (exit {rc})")
        if not ok:
            fails += 1

    want("the package's loader passes", run(pkg), 0)

    raw = bytearray(pkg.read_bytes())
    markers = [raw[i * BLK + KEEP] for i in range(BLOCKS)]
    L = strip(raw)
    hdr = bytearray(L[0:0x40])
    F.enc(hdr, 0, 0x40, 0xFFFF)
    nent = struct.unpack_from("<H", hdr, 8)[0]
    entries = []
    for i in range(nent):
        e = bytearray(L[0x40 + i * 0x50:0x40 + (i + 1) * 0x50])
        F.enc(e, 0, 0x50, 0xFFFF)
        entries.append(e)
    flash = next(e for e in entries if struct.unpack_from("<H", e, 0)[0] == 0)
    ota = next(e for e in entries if struct.unpack_from("<H", e, 0)[0] == 100)

    with tempfile.TemporaryDirectory() as d:
        # a decoy named ota.bin (type 0, the real loader) beside a different type-100 entry: a
        # script that picked by name would pass; the type-100 entry must be used (and mismatch)
        decoy = bytearray(ota)
        struct.pack_into("<H", decoy, 0, 0)
        decoy[0x40:0x50] = b"ota.bin" + b"\0" * 9
        wrong = bytearray(ota)
        wrong[0x40:0x50] = b"real.bin" + b"\0" * 8
        struct.pack_into("<II", wrong, 8, 0x400, 64)          # flash.bin's head, not the loader
        p = Path(d) / "decoy.fwsc"
        p.write_bytes(insert_markers(build_logical(L, [flash, wrong, decoy]), markers))
        want("an ota.bin-named decoy is ignored (type 100 wins)", run(p), 1)

        p = Path(d) / "two.fwsc"
        p.write_bytes(insert_markers(build_logical(L, [flash, ota, bytearray(ota)]), markers))
        want("two type-100 entries are rejected", run(p), 2)

        bad = bytearray(raw)
        bad[0] ^= 0x01                                        # a broken header CRC
        p = Path(d) / "crc.fwsc"
        p.write_bytes(bad)
        want("a broken header CRC is rejected", run(p), 2)

    print("check_loader_test: " + ("FAILED" if fails else "passed"))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
