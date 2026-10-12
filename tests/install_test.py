#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
"""tools/fm1_install.py against a simulated FM-1 (no hardware, no mido).
The fake device is the one in web/test_web.mjs: identity on the handshake,
then "device asks, host answers" reads of the logical image. Run from the repo root:
  python3 tests/install_test.py
Also checks logical_image/product_of against web/fm1pkg.js (needs node and
build/x0x.fwsc, skipped otherwise)."""
import hashlib
import io
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import fm1_install as I       # noqa: E402
import fm1pkg_make as F       # noqa: E402

I.DELAY.update(open=0, start=0.01, reply=0, loader=0.01, reboot=0.01, retry=0.02, poll=0.05, hs=0.1,
               idle_check=0.4, idle_write=0.4, wait_loader=2, wait_reboot=2)
failed = 0


def ok(cond, what):
    global failed
    print(f"{what:<64} {'ok' if cond else 'FAIL'}")
    failed += not cond


# ---------------------------------------------------------- simulated FM-1 ---

class FakeLink:
    def __init__(self, dev, gen):
        self.dev, self.gen, self.q, self.closed = dev, gen, queue.Queue(), False

    @property
    def lost(self):
        return self.gen != self.dev.gen or not self.dev.connected

    def send(self, pkt):
        if self.lost:
            return False
        self.dev.sent.append(bytes(pkt))
        self.dev.rx(bytes(pkt))
        return True

    def read(self, timeout):
        try:
            return self.q.get(timeout=max(timeout, 0))
        except queue.Empty:
            return None

    def drain(self):
        while not self.q.empty():
            self.q.get_nowait()

    def close(self):
        self.closed = True


class FakeFM1:
    """a MIDI backend with one FM-1 on it"""

    def __init__(self, image, identity="FM-1_015", name="FM-1", unplug_after=None, after_write="FM-1_900",
                 stall_after=None, bad_addr=None):
        self.image, self.unplug_after, self.after_write = image, unplug_after, after_write
        self.stall_after, self.bad_addr = stall_after, bad_addr
        self.served = self.bad = self.upgrades = 0
        self.sent, self.links, self.gen, self.lock = [], [], 0, threading.Lock()
        self.boot(identity, name)

    def boot(self, identity, name):
        with self.lock:
            self.gen += 1
            self.identity, self.name, self.connected = identity, name, True
            self.waiting, self.queue = None, []

    def input_names(self):
        return [self.name] if self.connected else []

    output_names = input_names

    def open(self, in_name, out_name):
        if in_name != self.name or not self.connected:
            raise IOError("no such port")
        link = FakeLink(self, self.gen)
        self.links.append(link)
        return link

    def tx(self, pkt):
        for link in self.links:
            if link.gen == self.gen and not link.closed:
                link.q.put(bytes(pkt))

    def rx(self, d):
        if d == I.HS_QUERY:
            t = self.identity.encode()
            self.tx(b"\xF0" + I.pack7(bytes([0, 0x59, 0x11, 0, 0, 0]) + t + bytes(28 - len(t))) + b"\xF7")
        elif d == I.UPGRADE:
            self.upgrades += 1
            first = self.bad_addr if self.bad_addr is not None else 0
            self.queue = ([(first + k * 512, 512) for k in range(6)] + [(I.FINISH_WRITE, 8)]
                          if self.identity.startswith("ota-")
                          else [(0, 64), (0x40, 160), (0x1000, 512), (I.FINISH_CHECK, 8)])
            self.next()
        elif self.waiting:
            u = I.unpack7(d[1:-1])
            addr, n = self.waiting
            fin = addr >= I.FINISH_CHECK
            want = b"success\0" if fin else self.image[addr:addr + n]
            if u[14:14 + len(want)] != want or len(u) != 15 + len(want):
                self.bad += 1
            self.waiting = None
            self.served += 1
            if self.unplug_after and self.served >= self.unplug_after:
                self.connected = False
                return
            if self.stall_after and self.served >= self.stall_after:
                return
            if addr == I.FINISH_CHECK:
                threading.Timer(0.05, self.boot, ("ota-FM-1_900", "Felucca Update")).start()
            elif addr == I.FINISH_WRITE:
                threading.Timer(0.05, self.boot, (self.after_write, "Felucca")).start()
            else:
                self.next()

    def next(self):
        if not self.queue:
            return
        self.waiting = addr, n = self.queue.pop(0)
        u = bytearray([0, 0x59, 0x30, 0, 0, 0, 0]) + addr.to_bytes(4, "little") + bytes([n & 0xFF, n >> 8, 0])
        u.append(~sum(u[6:14]) & 0xFF)
        self.tx(b"\xF0" + I.pack7(u) + b"\xF7")


# ----------------------------------------------------------------- helpers ---

def package(product="FM-1_900", loader=None, flash=None):
    """a genuine UFW .fwsc (tools/fm1pkg_make.py): a real type-100 loader entry, not just
    FM-1 identity markers, so the approved-loader gate has something to parse"""
    if loader is None:
        loader = APPROVED_LOADER
    if flash is None:
        flash = bytes((i * 3) & 0xFF for i in range(0x1000))
    return F.ufw(flash, loader, product)


TMP = Path(tempfile.mkdtemp(prefix="felucca-install-"))

# The approved loader (any bytes; these tests own the sha file) and a same-length tamper that
# keeps the FELUCCA-LOADER-1 marker, so only the strict type-100 sha check tells them apart.
APPROVED_LOADER = I.LOADER_MARK + bytes(range(64)) + b"\xF7"
TAMPERED_LOADER = APPROVED_LOADER[:-1] + b"\xF6"
SHA = TMP / "x0x_loader.sha256"
SHA.write_text(hashlib.sha256(APPROVED_LOADER).hexdigest() + "  loader/ota.bin\n")
I.X0X_LOADER_SHA = SHA          # point the installer gate at the temporary approved loader


def pkgfile(name, raw):
    p = TMP / name
    p.write_bytes(raw)
    return str(p)


def cli(args, dev, answer=True):
    out, err = io.StringIO(), io.StringIO()
    old, sys.stderr = sys.stderr, err
    try:
        rc = I.main(args, backend=dev, out=out, ask=lambda _p: answer)
    finally:
        sys.stderr = old
    return rc, out.getvalue(), err.getvalue()


# ------------------------------------------------------------------- tests ---

def wire():
    data = bytes(range(256)) * 3
    ok(I.unpack7(I.pack7(data))[:len(data)] == data, "pack7 / unpack7 round trip")
    pkt = I.response(0x12345, b"\x01\x02\x03", fl=5)
    u = I.unpack7(pkt[1:-1])
    ok(u[:3] == b"\x00\x59\x30" and int.from_bytes(u[7:11], "little") == 0x12345 and u[14:17] == b"\x01\x02\x03"
       and ~sum(u[6:17]) & 0xFF == u[17], "response: header, address, data, checksum")
    raw = package("FM-1_906")
    ok(I.product_of(raw) == "FM-1_906" and len(I.logical_image(raw)) == len(raw) - 20, "product_of / logical_image (UFW)")
    raw = package("FM-1_9001000")                  # X0X from 0.10: major, minor, patch, two digits each
    ok(I.product_of(raw) == "FM-1_9001000", "product_of: a 12-character identity")


def installs():
    raw = package()
    image = I.logical_image(raw)
    p = pkgfile("ok.fwsc", raw)

    dev = FakeFM1(image)
    rc, out, err = cli([p, "--yes"], dev)
    ok(rc == 0 and dev.bad == 0 and dev.upgrades == 2 and dev.served == 11 and "done: the FM-1 runs FM-1_900" in out
       and "100%" in out, f"install: running -> loader -> Felucca ({dev.served} reads)")

    raw10 = package("FM-1_9001000")
    dev = FakeFM1(I.logical_image(raw10), identity="FM-1_909", after_write="FM-1_9001000")
    rc, out, err = cli([pkgfile("x10.fwsc", raw10), "--yes"], dev)
    ok(rc == 0 and dev.bad == 0 and "done: the FM-1 runs FM-1_9001000" in out,
       "install: 0.9 (FM-1_909) -> 0.10 (FM-1_9001000), the longer identity verified after")

    dev = FakeFM1(image, identity="ota-FM-1_900", name="Felucca Update")
    rc, out, err = cli([p, "--yes"], dev)
    ok(rc == 0 and dev.bad == 0 and dev.upgrades == 1 and "update mode" in out, "install: device already in update mode -> finishes the write")

    dev = FakeFM1(image)
    rc, out, err = cli([p], dev, answer=False)
    ok(rc == 1 and dev.upgrades == 0 and "cancelled" in out, "install: answer no -> nothing sent but the handshake")

    dev = FakeFM1(image, identity="FM-1_905", name="Felucca")
    rc, out, err = cli(["--info"], dev)
    ok(rc == 0 and "FM-1_905" in out and "running" in out and dev.upgrades == 0, "--info: identity of the connected FM-1")

    dev = FakeFM1(image, identity="ota-FM-1_900", name="Felucca Update")
    rc, out, err = cli(["--info"], dev)
    ok(rc == 0 and "update loader" in out, "--info: device in update mode")


def errors():
    raw = package()
    image = I.logical_image(raw)
    p = pkgfile("ok.fwsc", raw)

    rc, out, err = cli([p, "--yes"], FakeFM1(image, name="IAC Driver Bus 1"))
    ok(rc == 3 and "not found" in err and "IAC Driver" in err, "no FM-1 port -> exit 3, lists the MIDI inputs")
    dev = FakeFM1(image)
    rc, out, err = cli([p, "--yes", "--port", "Felucca"], dev)
    ok(rc == 3 and dev.upgrades == 0, "--port that matches nothing -> exit 3")
    dev = FakeFM1(image, name="My Interface")
    rc, out, err = cli(["--info", "--port", "my int"], dev)
    ok(rc == 0 and "FM-1_015" in out, "--port picks a port the default match skips")

    t0 = time.monotonic()
    rc, out, err = cli([p, "--yes"], FakeFM1(image, unplug_after=2))
    ok(rc == 4 and "disconnected" in err and "nothing was written" in err and time.monotonic() - t0 < 3,
       "unplugged in step 1 -> exit 4 'lost', nothing written")
    t0 = time.monotonic()
    rc, out, err = cli([p, "--yes"], FakeFM1(image, identity="ota-FM-1_900", name="Felucca Update", unplug_after=3))
    ok(rc == 4 and "run the install again" in err and time.monotonic() - t0 < 3, "unplugged during the write -> exit 4 at once")
    rc, out, err = cli([p, "--yes"], FakeFM1(image, identity="ota-FM-1_900", name="Felucca Update", stall_after=3))
    ok(rc == 4 and "stopped answering" in err, "loader stops answering -> exit 4 after the idle time")
    rc, out, err = cli([p, "--yes"], FakeFM1(image, identity="ota-FM-1_900", name="Felucca Update", bad_addr=len(image)))
    ok(rc == 4 and "outside the package" in err, "read past the package -> exit 4")

    rc, out, err = cli([p, "--yes"], FakeFM1(image, after_write="FM-1_015"))
    ok(rc == 6 and "reports FM-1_015" in err, "another identity after the restart -> exit 6")
    dev = FakeFM1(image, identity="XY-9_001", name="usb-midi")
    rc, out, err = cli([p, "--yes"], dev)
    ok(rc == 6 and dev.upgrades == 0, "another model -> exit 6, nothing sent")

    class Stuck(FakeFM1):          # the loader never shows up
        def boot(self, identity, name):
            super().boot(identity, name)
            if identity.startswith("ota-"):
                self.connected = False
    rc, out, err = cli([p, "--yes"], Stuck(image))
    ok(rc == 5 and "loader did not appear" in err, "no loader after step 1 -> exit 5")

    rc, out, err = cli([pkgfile("short.fwsc", b"\0" * 100), "--yes"], FakeFM1(image))
    ok(rc == 2 and "too short" in err, "not a package -> exit 2")
    rc, out, err = cli([str(TMP / "missing.fwsc"), "--yes"], FakeFM1(image))
    ok(rc == 2, "missing file -> exit 2")


def loader_gate():
    """the installer enforces the approved loader before any device is opened (tests.md #1), and
    --force is the stock-rescue path that accepts only M-VAVE's V15 package by full sha256 (r1)."""
    good = pkgfile("approved.fwsc", package(loader=APPROVED_LOADER))
    bad = pkgfile("tampered.fwsc", package(loader=TAMPERED_LOADER))
    good_image = I.logical_image(Path(good).read_bytes())
    bad_image = I.logical_image(Path(bad).read_bytes())
    ok(I.LOADER_MARK in Path(bad).read_bytes(), "the tampered package keeps the FELUCCA-LOADER-1 marker")

    dev = FakeFM1(good_image)
    rc, out, err = cli([good, "--yes"], dev)
    ok(rc == 0 and dev.bad == 0 and dev.upgrades == 2 and "done: the FM-1 runs FM-1_900" in out,
       "the approved loader installs (type-100 sha matches)")

    dev = FakeFM1(bad_image)
    rc, out, err = cli([bad, "--yes"], dev)
    ok(rc == 2 and dev.upgrades == 0 and dev.sent == [] and "not the approved X0X 1.0.5 loader" in err,
       "a different loader is refused before any MIDI (exit 2, nothing sent)")

    # r1: --force is the stock-rescue path, not a bypass. Only M-VAVE's V15 (its full sha256) is
    # accepted; any other package, however well-formed, is refused before a device is opened.
    dev = FakeFM1(bad_image)
    rc, out, err = cli([bad, "--yes", "--force"], dev)
    ok(rc == 2 and dev.upgrades == 0 and dev.sent == [] and "stock FM-1 V15" in err,
       "--force refuses a package that is not M-VAVE's V15 (exit 2, nothing sent)")

    # The V15 path is exercised with a stub hash injected (never a device): the test package's own
    # sha256 becomes STOCK_SHA256, so only the hash gate is under test.
    v15 = pkgfile("v15.fwsc", package(loader=APPROVED_LOADER))
    old_stock = I.STOCK_SHA256
    I.STOCK_SHA256 = hashlib.sha256(Path(v15).read_bytes()).hexdigest()
    dev = FakeFM1(good_image)
    rc, out, err = cli([v15, "--yes", "--force"], dev, answer=False)
    ok(rc == 1 and dev.upgrades == 0 and "cancelled" in out,
       "--force ignores --yes and needs a typed confirmation (none -> cancelled)")
    dev = FakeFM1(good_image)
    rc, out, err = cli([v15, "--force"], dev, answer=True)
    ok(rc == 0 and dev.bad == 0 and dev.upgrades == 2 and "done: the FM-1 runs FM-1_900" in out,
       "--force installs the pinned V15 package once confirmed (stub hash, no device)")
    I.STOCK_SHA256 = old_stock

    broken = bytearray(Path(bad).read_bytes())
    broken[16] ^= 0xFF                                     # corrupt the encrypted chip tag
    dev = FakeFM1(bad_image)
    rc, out, err = cli([pkgfile("broken.fwsc", bytes(broken)), "--yes"], dev)
    ok(rc == 2 and dev.sent == [] and "chip" in err, "a structurally broken package fails closed")

    old = I.X0X_LOADER_SHA
    empty = TMP / "empty.sha256"
    empty.write_text("\n")                                 # r1: an empty sha file must be a clean badpkg
    I.X0X_LOADER_SHA = empty
    dev = FakeFM1(good_image)
    rc, out, err = cli([good, "--yes"], dev)
    ok(rc == 2 and dev.sent == [] and "empty" in err, "an empty approved-loader sha file fails closed")

    I.X0X_LOADER_SHA = TMP / "missing.sha256"
    dev = FakeFM1(good_image)
    rc, out, err = cli([good, "--yes"], dev)
    ok(rc == 2 and dev.sent == [] and "approved loader hashes" in err, "a missing approved-loader sha file fails closed")
    I.X0X_LOADER_SHA = old


def against_js():
    clean = ROOT / "build/x0x.fwsc"
    if not shutil.which("node") or not clean.exists():
        print("logical image vs fm1pkg.js: skipped (needs node and build/x0x.fwsc)")
        return
    js = ("import { logicalImage, productOf } from %r; import { readFileSync } from 'node:fs';"
          "const p = readFileSync(process.argv[1]); process.stderr.write(productOf(p));"
          "process.stdout.write(logicalImage(p));") % str(ROOT / "web/fm1pkg.js")
    r = subprocess.run(["node", "--input-type=module", "-e", js, str(clean)], capture_output=True, check=True)
    raw = clean.read_bytes()
    ok(I.logical_image(raw) == r.stdout and I.product_of(raw) == r.stderr.decode(),
       f"logical_image / product_of == fm1pkg.js ({clean.name}, {I.product_of(raw)})")
    ok(I.LOADER_MARK in raw, f"{clean.name} carries the Felucca loader marker")


wire()
installs()
errors()
loader_gate()
against_js()
shutil.rmtree(TMP, ignore_errors=True)
print(f"INSTALL TESTS FAILED ({failed})" if failed else "install tests passed")
sys.exit(1 if failed else 0)
