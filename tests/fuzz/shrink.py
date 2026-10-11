#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""T2c: shrink a failing UI-fuzz reproduction.

Given the full sequence the `fuzz SEED N` command wrote, delete event lines from the body while
the host runner still fails (ddmin), then always re-append the fixed release tail (release every
key and button, `stop`, wait 3 s, `expect voices 0`, `expect heldnotes 0`) and write the
minimised, ready-to-run .x0x.

    shrink.py FULL.x0x MIN.x0x HOST [OUTDIR]

The tail is pinned (never shrunk) so the minimiser cannot remove the release / stop / expect that
defines the failure. A candidate "still fails" when the host exits non-zero: a voice survives the
tail, a `expect heldnotes 0` mid-run check misses, or the runner crashed (a trap / signal).
"""
import os
import re
import subprocess
import sys

MAX_RUNS = 6000
TIMEOUT_S = 120

BTNS = ["FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ",
        "PLAY", "REC", "OCT-", "OCT+"]
# the fixed tail the fuzz release tail runs: every key and button released, both transports
# stopped (the `stop` command), 3 s, then the invariant. Never uses the all-notes-off.
TAIL = (["key %d up" % k for k in range(27)] + ["release " + b for b in BTNS]
        + ["stop", "wait 3000", "expect voices 0", "expect heldnotes 0"])
TAIL_RE = re.compile(r"^(key \d+ up|release \S+|stop|wait 3000|expect voices 0|expect heldnotes 0)$")


def main():
    if len(sys.argv) < 4:
        print("usage: shrink.py FULL.x0x MIN.x0x HOST [OUTDIR]", file=sys.stderr)
        return 2
    full, mini, host = sys.argv[1], sys.argv[2], sys.argv[3]
    outdir = sys.argv[4] if len(sys.argv) > 4 else (os.path.dirname(mini) or ".")
    os.makedirs(outdir, exist_ok=True)
    with open(full) as fh:
        raw = fh.read().splitlines()
    lines = [l for l in raw if l.strip() and not l.lstrip().startswith("#")]
    # the factory-reset guard is not an event: it must stay so the replay matches the fuzz run
    prelude = [l for l in lines if l.strip() == "noreset"]
    body = [l for l in lines if l.strip() != "noreset"]
    # the body is the events plus any mid-run checks; drop the file's own tail before shrinking
    while body and TAIL_RE.match(body[-1]):
        body.pop()
    orig = len(body)
    tmp = os.path.join(outdir, ".shrink_candidate.x0x")
    runs = [0]

    def fails(cand):
        if not cand:
            return False
        runs[0] += 1
        with open(tmp, "w") as fh:
            fh.write("\n".join(prelude + cand + TAIL) + "\n")
        try:
            rc = subprocess.run([host, tmp, outdir], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=TIMEOUT_S).returncode
        except subprocess.TimeoutExpired:
            rc = 124
        return rc != 0

    if not fails(body):
        print("shrink: the body no longer fails with the fixed tail; nothing to do", file=sys.stderr)
        return 1

    n = 2
    while len(body) >= 2:
        chunk = (len(body) + n - 1) // n
        cut = False
        i = 0
        while i < len(body):
            cand = body[:i] + body[i + chunk:]
            if fails(cand):
                body = cand
                n = max(n - 1, 2)
                cut = True
                break
            i += chunk
            if runs[0] >= MAX_RUNS:
                break
        if runs[0] >= MAX_RUNS:
            break
        if not cut:
            if n >= len(body):
                break
            n = min(len(body), n * 2)

    with open(mini, "w") as fh:
        fh.write("# T2c UI fuzz: minimised reproduction of %s (%d runs)\n"
                 % (os.path.basename(full), runs[0]))
        fh.write("#   build/host/x0x_host %s build/fuzz\n" % mini)
        fh.write("\n".join(prelude + body + TAIL) + "\n")
    try:
        os.remove(tmp)
    except OSError:
        pass
    print("shrink: %d body lines -> %d (%d runs)" % (orig, len(body), runs[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
