#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# fm1-rocket-os: the loader guard. Exit 0 only if the loader in a package matches the one
# recorded in tools/x0x_loader.sha256 (X0X 1.0.5's, the only loader the unattended-flashing
# rule in docs/engineering/build-plan.md permits).
#
#   tools/check_loader.sh build/x0x.fwsc     # one installable package
#   tools/check_loader.sh build               # every *.fwsc in the directory (all must pass)
#
# It reads the package the way the device's ota_ufw does (firmware/src/ota.c): the loader is the
# entry of type 100, never a file merely named "ota.bin". A package with no type-100 entry, more
# than one, a bad header or entry-list CRC, a wrong chip tag, a bad entry count or a truncated
# entry fails closed (exit 2); a well-formed package whose loader differs exits 1.
# Stripping the BREAK part must not change the loader: run this on the stripped build too.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
ARG=${1:-}

if [ -z "$ARG" ]; then
    echo "usage: tools/check_loader.sh <package.fwsc | directory>" >&2
    exit 2
fi

python3 - "$ROOT/tools/x0x_loader.sha256" "$ARG" <<'PY'
import glob
import hashlib
import os
import struct
import sys

BLOCKS, BLK, KEEP = 20, 0x30, 0x2F     # .fwsc marker bytes (tools/fm1pkg_make.py)
HDR = 0x40                             # UFW header
ENT = 0x50                             # UFW entry
CHIP = b"AC791N"
TYPE_OTA = 100


def crc16(data, crc=0):                # poly 0x1021, init 0, MSB-first (ota_crc16)
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def enc(buf, key=0xFFFF):              # UFW header/entry cipher (ota_jl_enc)
    for i in range(len(buf)):
        buf[i] ^= key & 0xFF
        key = ((key << 1) ^ (0x1021 if key & 0x8000 else 0)) & 0xFFFF


def logical_image(raw):
    """the package without its 20 marker bytes"""
    if len(raw) < BLOCKS * BLK:
        raise ValueError("not an FM-1 package (too short)")
    out = bytearray()
    for i in range(BLOCKS):
        out += raw[i * BLK:i * BLK + KEEP]
    return bytes(out + raw[BLOCKS * BLK:])


def ota_of(raw):
    """the type-100 entry's bytes, selected as ota_ufw selects them; ValueError on any problem"""
    logical = logical_image(raw)
    hdr = bytearray(logical[0:HDR])
    enc(hdr)
    nent = struct.unpack_from("<H", hdr, 8)[0]
    if hdr[16:22] != CHIP:
        raise ValueError("wrong chip tag (not AC791N)")
    if not 1 <= nent <= 11:
        raise ValueError(f"entry count {nent} out of range (1-11)")
    if crc16(hdr[2:HDR]) != struct.unpack_from("<H", hdr, 0)[0]:
        raise ValueError("bad header CRC")
    if crc16(logical[HDR:HDR + nent * ENT]) != struct.unpack_from("<H", hdr, 2)[0]:
        raise ValueError("bad entry-list CRC")
    found = []
    for i in range(nent):
        e = bytearray(logical[HDR + i * ENT:HDR + (i + 1) * ENT])
        enc(e)
        typ, _idx, _dcrc, _res, off, ln = struct.unpack_from("<HHHHII", e, 0)
        if typ == TYPE_OTA:
            if off + ln > len(logical) or ln == 0:
                raise ValueError("type-100 entry runs past the package")
            data = bytes(logical[off:off + ln])
            if len(data) != ln:
                raise ValueError("type-100 entry length mismatch")
            found.append(data)
    if not found:
        raise ValueError("no type-100 (loader) entry")
    if len(found) > 1:
        raise ValueError(f"{len(found)} type-100 entries (a package has one)")
    return found[0]


def targets(arg):
    if os.path.isdir(arg):
        files = sorted(glob.glob(os.path.join(arg, "*.fwsc")))
        if not files:
            raise ValueError(f"no *.fwsc in {arg}")
        return files
    if arg.endswith(".fwsc"):
        return [arg]
    raise ValueError("expected a .fwsc package or a directory of them")


def main():
    sha_file, arg = sys.argv[1], sys.argv[2]
    want = open(sha_file).read().split()[0]
    try:
        files = targets(arg)
    except (OSError, ValueError) as e:
        print(f"check_loader: {arg}: {e}", file=sys.stderr)
        return 2
    rc = 0
    for path in files:
        try:
            data = open(path, "rb").read()
            loader = ota_of(data)
        except (OSError, ValueError, struct.error) as e:      # fail closed on any parse failure
            print(f"check_loader: FAIL {path}: {e}", file=sys.stderr)
            rc = 2
            continue
        got = hashlib.sha256(loader).hexdigest()
        if got == want:
            print(f"check_loader: ok  {path} loader matches X0X 1.0.5 ({want[:16]}...)")
        else:
            print(f"check_loader: MISMATCH {path}\n  expected {want}\n  got      {got}", file=sys.stderr)
            rc = max(rc, 1)
    return rc


sys.exit(main())
PY
