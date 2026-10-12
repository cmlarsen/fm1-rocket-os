#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# fm1-rocket-os: the shared UFW package / approved-loader check.
"""Read a Felucca package's update loader and compare it with the approved one.

This is the one implementation of "select the loader the way the device does".
It is used by tools/check_loader.sh (the guard) and by tools/fm1_install.py (the
installer gate), so a package the installer accepts is exactly a package the
guard accepts.

The loader is the UFW entry of type 100, never a file merely named "ota.bin"
(firmware/src/ota.c, ota_ufw). The package's logical image is the .fwsc with its
20 marker bytes removed. ota_of() validates the header chip tag, entry count,
header CRC and entry-list CRC, then returns the single type-100 entry's bytes.

The function fails closed: any structural problem raises ValueError, and a
missing read raises OSError. Nothing here opens a device.
"""
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
    if len(hdr) < HDR:
        raise ValueError("package too short for a UFW header")
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
        if len(e) < ENT:
            raise ValueError("entry list runs past the package")
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


def loader_sha(loader):
    """the loader's sha256, lowercase hex"""
    return hashlib.sha256(loader).hexdigest()


def read_want(sha_file):
    """the approved loader's sha256: the first whitespace token of the sha file"""
    with open(sha_file) as f:
        return f.read().split()[0]


class LoaderMismatch(ValueError):
    """a structurally valid package whose type-100 loader is not the approved one"""

    def __init__(self, got):
        super().__init__("loader differs from the approved one")
        self.got = got


def verify(raw, want):
    """ota_of(raw) then compare its sha to `want`; -> the (matching) sha256.

    Raises ValueError (structurally bad package) or LoaderMismatch (a well-formed
    package whose loader is not approved)."""
    got = loader_sha(ota_of(raw))
    if got != want:
        raise LoaderMismatch(got)
    return got


def targets(arg):
    """the .fwsc files to check: one path, or every *.fwsc in a directory"""
    if os.path.isdir(arg):
        files = sorted(glob.glob(os.path.join(arg, "*.fwsc")))
        if not files:
            raise ValueError(f"no *.fwsc in {arg}")
        return files
    if arg.endswith(".fwsc"):
        return [arg]
    raise ValueError("expected a .fwsc package or a directory of them")


def main(argv):
    """tools/check_loader.sh's CLI: (sha_file, package.fwsc | directory) -> exit code.

    0 match, 1 a well-formed package with a different loader, 2 a structural
    failure, a missing package or bad arguments."""
    sha_file, arg = argv[1], argv[2]
    want = read_want(sha_file)
    try:
        files = targets(arg)
    except (OSError, ValueError) as e:
        print(f"check_loader: {arg}: {e}", file=sys.stderr)
        return 2
    rc = 0
    for path in files:
        try:
            with open(path, "rb") as f:
                data = f.read()
            loader = ota_of(data)
        except (OSError, ValueError, struct.error) as e:      # fail closed on any parse failure
            print(f"check_loader: FAIL {path}: {e}", file=sys.stderr)
            rc = 2
            continue
        got = loader_sha(loader)
        if got == want:
            print(f"check_loader: ok  {path} loader matches X0X 1.0.5 ({want[:16]}...)")
        else:
            print(f"check_loader: MISMATCH {path}\n  expected {want}\n  got      {got}", file=sys.stderr)
            rc = max(rc, 1)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
