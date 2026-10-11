#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""T2c: shrink a failing UI-fuzz reproduction.

Given the full sequence the `fuzz SEED N` command wrote, delete event lines while the host
runner still fails (ddmin), and write the minimised, ready-to-run .x0x.

    shrink.py FULL.x0x MIN.x0x HOST [OUTDIR]

A candidate "still fails" when the host exits non-zero: a voice survives the release tail
(`expect voices 0` in the sequence) or the runner crashed (a trap / signal -> non-zero).
"""
import os
import subprocess
import sys

MAX_RUNS = 6000
TIMEOUT_S = 120


def main():
    if len(sys.argv) < 4:
        print("usage: shrink.py FULL.x0x MIN.x0x HOST [OUTDIR]", file=sys.stderr)
        return 2
    full, mini, host = sys.argv[1], sys.argv[2], sys.argv[3]
    outdir = sys.argv[4] if len(sys.argv) > 4 else (os.path.dirname(mini) or ".")
    os.makedirs(outdir, exist_ok=True)
    with open(full) as f:
        raw = f.read().splitlines()
    lines = [l for l in raw if l.strip() and not l.lstrip().startswith("#")]
    # lines that must stay for the replay to match the fuzz run (the factory-reset guard)
    prelude = [l for l in lines if l.strip() == "noreset"]
    lines = [l for l in lines if l.strip() != "noreset"]
    orig = len(lines) + len(prelude)
    tmp = os.path.join(outdir, ".shrink_candidate.x0x")
    runs = [0]

    def fails(cand):
        if not cand:
            return False
        runs[0] += 1
        with open(tmp, "w") as f:
            f.write("\n".join(prelude + cand) + "\n")
        try:
            rc = subprocess.run([host, tmp, outdir], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL, timeout=TIMEOUT_S).returncode
        except subprocess.TimeoutExpired:
            rc = 124
        return rc != 0

    if not fails(lines):
        print("shrink: the full sequence no longer fails; nothing to do", file=sys.stderr)
        return 1

    n = 2
    while len(lines) >= 2:
        chunk = (len(lines) + n - 1) // n
        cut = False
        i = 0
        while i < len(lines):
            cand = lines[:i] + lines[i + chunk:]
            if fails(cand):
                lines = cand
                n = max(n - 1, 2)
                cut = True
                break
            i += chunk
            if runs[0] >= MAX_RUNS:
                break
        if runs[0] >= MAX_RUNS:
            break
        if not cut:
            if n >= len(lines):
                break
            n = min(len(lines), n * 2)

    with open(mini, "w") as f:
        f.write("# T2c UI fuzz: minimised reproduction of %s (%d runs)\n"
                % (os.path.basename(full), runs[0]))
        f.write("#   build/host/x0x_host %s build/fuzz\n" % mini)
        f.write("\n".join(prelude + lines) + "\n")
    try:
        os.remove(tmp)
    except OSError:
        pass
    print("shrink: %d lines -> %d lines (%d runs)" % (orig, len(lines), runs[0]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
