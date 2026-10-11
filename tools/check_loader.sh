#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# fm1-rocket-os: the loader guard. Exit 0 only if the loader in a build matches the one
# recorded in tools/x0x_loader.sha256 (X0X 1.0.5's, the only loader the unattended-flashing
# rule in docs/engineering/build-plan.md permits).
#
#   tools/check_loader.sh build                 # a build directory (build/loader/ota.bin)
#   tools/check_loader.sh build/x0x.fwsc        # an installable package (ota.bin inside it)
#   tools/check_loader.sh path/to/loader/ota.bin
#
# Stripping the BREAK part must not change the loader: run this on the stripped build too.
# Exit: 0 match, 1 mismatch, 2 usage or unreadable input.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
ARG=${1:-}

if [ -z "$ARG" ]; then
    echo "usage: tools/check_loader.sh <build-dir|package.fwsc|loader/ota.bin>" >&2
    exit 2
fi

python3 - "$ROOT/tools/x0x_loader.sha256" "$ARG" <<'PY'
import hashlib
import struct
import sys
from pathlib import Path

BLOCKS, BLK, KEEP = 20, 0x30, 0x2F
ENC_KEY = 0xFFFF


def enc(buf, off, size, key):
    for i in range(size):
        buf[off + i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


def logical_image(raw):
    """the package without its 20 marker bytes (see tools/fm1_install.py)"""
    out = bytearray()
    for i in range(BLOCKS):
        out += raw[i * BLK:i * BLK + KEEP]
    return bytes(out + raw[BLOCKS * BLK:])


def ota_from_fwsc(path):
    raw = Path(path).read_bytes()
    if len(raw) < BLOCKS * BLK:
        raise ValueError("not an FM-1 package (too short)")
    logical = logical_image(raw)
    hdr = bytearray(logical[0:0x40])
    enc(hdr, 0, 0x40, ENC_KEY)
    nfiles = struct.unpack_from("<H", hdr, 8)[0]
    off = 0x40
    for _ in range(nfiles):
        e = bytearray(logical[off:off + 0x50])
        enc(e, 0, 0x50, ENC_KEY)
        name = bytes(e[0x40:0x40 + 16]).split(b"\0")[0].decode("latin-1")
        o, ln = struct.unpack_from("<II", e, 8)
        if name == "ota.bin":
            return logical[o:o + ln]
        off += 0x50
    raise ValueError("no ota.bin entry in the package")


def loader_bytes(arg):
    p = Path(arg)
    if p.is_dir():
        for rel in ("loader/ota.bin", "build/loader/ota.bin"):
            if (p / rel).is_file():
                return (p / rel).read_bytes()
        raise ValueError(f"no loader/ota.bin under {p}")
    if p.suffix == ".fwsc":
        return ota_from_fwsc(p)
    return p.read_bytes()


def main():
    sha_file, arg = sys.argv[1], sys.argv[2]
    want = Path(sha_file).read_text().split()[0]
    try:
        got = hashlib.sha256(loader_bytes(arg)).hexdigest()
    except (OSError, ValueError) as e:
        print(f"check_loader: {arg}: {e}", file=sys.stderr)
        return 2
    if got == want:
        print(f"check_loader: ok  {arg} loader matches X0X 1.0.5 ({want[:16]}...)")
        return 0
    print(f"check_loader: MISMATCH {arg}\n  expected {want}\n  got      {got}", file=sys.stderr)
    return 1


sys.exit(main())
PY
