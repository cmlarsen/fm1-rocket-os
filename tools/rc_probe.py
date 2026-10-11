#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""RC1p: measure the FM-1's CPU headroom over USB, without hands.

Talks to a development build (X0X_DEBUG=1; `make fw-debug`) through the editor protocol's
ED_STRESS / ED_STATS / ED_STATS_RESET commands (firmware/src/app/rc_debug.c). It resets the
stats, starts a stress level, samples ED_STATS every 500 ms for --seconds, stops the stress and
prints a table plus a JSON summary. It never installs or writes flash: `--info` only asks the
device who it is.

  rc_probe.py --stress N [--cap K] --seconds S [--port NAME] [--json FILE]
  rc_probe.py --info [--port NAME]

Stress levels (ED_STRESS n):
  1  the X0X baseline: 909 + 808 + 303 A + 303 B on a busy built-in pattern
  2  LIVE: a busy clip on every role, CHORDS a held 4-note FM6 chord, FX on NOISE
  3  level 2 plus every wired punch FX (none are wired yet, so it equals 2)
  4  level 2 with the FM6 voice cap raised to --cap (6 or 8; the fw-debug build's array is 8)
  5  level 2 with core 1 forced off (the single-core numbers; T5perf)
  6  level 1 with core 1 forced off (T5perf)

RC1 pass criteria (stress 2): mean CPU < 70 %, max < 85 %, 0 xruns over 60 s, and the overload
guard never engaged. See docs/engineering/rc1-probe.md. Needs mido with python-rtmidi.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

try:
    import mido
except ImportError:                       # --info shells out to the installer, which reports it too
    mido = None

HDR = [0x7D, 0x46, 0x4C]
ED_STRESS, ED_STATS, ED_STATS_RESET = 60, 61, 62

# ED_STATS fields (rc_debug.h's RC_ST_*)
ST_CPU, ST_PEAK, ST_XRUNS, ST_GUARD, ST_GUARDCOUNT = 0, 1, 2, 3, 4
ST_CPU2, ST_CPU2_GAVEUP, ST_FM6, ST_CORE1 = 5, 6, 7, 8
ST_PROF0 = 9
ST_PROF_N = 9
ST_BSS, ST_POOL, ST_POOL_TOTAL, ST_IRQ = ST_PROF0 + ST_PROF_N, ST_PROF0 + ST_PROF_N + 1, \
    ST_PROF0 + ST_PROF_N + 2, ST_PROF0 + ST_PROF_N + 3
ST_N = ST_IRQ + 1
PROF = ["909", "808", "303A", "303B", "FX", "MASTER", "SEQ", "FM6", "NOISE"]
HERE = os.path.dirname(os.path.abspath(__file__))
# the FM-1's port name: the same names tools/fm1_install.py trusts (never probe other gear)
PORT_RE = re.compile(r"fm-1|felucca|ota|composite|sinco|usb-midi", re.I)


def w35(v):
    return [(v >> (7 * k)) & 0x7F for k in range(5)]


def r35(b, i):
    return sum(b[i + k] << (7 * k) for k in range(5)) & 0xFFFFFFFF


class Probe:
    def __init__(self, port=None):
        if mido is None:
            sys.exit("rc_probe: needs mido and python-rtmidi (pip install mido python-rtmidi)")
        name = port or next((n for n in mido.get_output_names() if PORT_RE.search(n)), None)
        if not name:
            sys.exit("rc_probe: no FM-1 MIDI port (is the device on USB and running a debug build?)")
        self.name = name
        self.inp = mido.open_input(name)
        self.out = mido.open_output(name)
        while self.inp.poll():
            pass

    def ask(self, cmd, args=(), timeout=3.0, retries=3):
        """Send a command and wait for its reply. RC1 saw commands 60/61 miss a reply in LIVE, so a
        timeout is retried (every command here is idempotent: a duplicate ED_STRESS restarts the same
        level, ED_STATS/ED_STRESS 0 are reads/no-ops). A late reply to an earlier attempt is fine:
        the command word still matches."""
        for _ in range(max(1, retries)):
            self.out.send(mido.Message("sysex", data=HDR + [cmd] + list(args)))
            end = time.monotonic() + timeout
            while time.monotonic() < end:
                m = self.inp.poll()
                if m is None:
                    time.sleep(0.002)
                    continue
                if m.type == "sysex" and list(m.data[:4]) == HDR + [cmd]:
                    return list(m.data[4:])
        sys.exit("rc_probe: no reply to command %d after %d tries (not a debug build, or a bad argument)"
                 % (cmd, retries))

    def stats(self):
        b = self.ask(ED_STATS)
        n = r35(b, 0)
        if n != ST_N:
            sys.exit("rc_probe: ED_STATS returned %d fields, wanted %d" % (n, ST_N))
        return [r35(b, 5 + 5 * k) for k in range(n)]

    def reset(self):
        self.ask(ED_STATS_RESET)

    def stress(self, n, cap=0):
        b = self.ask(ED_STRESS, w35(n) + w35(cap))
        if not r35(b, 5):
            sys.exit("rc_probe: ED_STRESS %d refused" % n)


def run_probe(p, n, cap, seconds):
    p.reset()
    p.stress(n, cap)
    rows, samples = [], []
    t0 = time.monotonic()
    while True:
        el = time.monotonic() - t0
        s = p.stats()
        samples.append(s)
        rows.append((el, s))
        print("%6.2f  cpu %3u  core1 %3u  peak %3u  xruns %3u  guard %u (%u)  fm6 %u"
              % (el, s[ST_CPU], s[ST_CORE1], s[ST_PEAK], s[ST_XRUNS], s[ST_GUARD], s[ST_GUARDCOUNT], s[ST_FM6]))
        if el >= seconds:
            break
        time.sleep(max(0.0, 0.5 - ((time.monotonic() - t0) - el)))
    p.stress(0)

    cpu = [s[ST_CPU] for s in samples]
    peak = [s[ST_PEAK] for s in samples]
    core1 = [s[ST_CORE1] for s in samples]
    # per-bucket share of the render, from the deltas between samples (the counters are cumulative)
    acc = [0] * ST_PROF_N
    tot = 0
    for a, b in zip(samples, samples[1:]):
        for k in range(ST_PROF_N):
            d = (b[ST_PROF0 + k] - a[ST_PROF0 + k]) & 0xFFFFFFFF
            acc[k] += d
            tot += d
    share = {PROF[k]: (100.0 * acc[k] / tot if tot else 0.0) for k in range(ST_PROF_N)}
    summary = {
        "stress": n,
        "cap": cap,
        "seconds": round(seconds, 2),
        "samples": len(samples),
        "cpu_pct_mean": round(sum(cpu) / len(cpu), 1),
        "cpu_pct_max": max(cpu),
        "peak_pct": max(peak),
        "xruns": samples[-1][ST_XRUNS],
        "guard_engagements": samples[-1][ST_GUARDCOUNT] - samples[0][ST_GUARDCOUNT],
        "guard_active": any(s[ST_GUARD] for s in samples),
        "fm6_voices": samples[-1][ST_FM6],
        "core1_pct_mean": round(sum(core1) / len(core1), 1),   # T5perf: core 1's own busy share
        "core1_pct_max": max(core1),
        "cpu2": bool(samples[-1][ST_CPU2]),
        "cpu2_gave_up": samples[-1][ST_CPU2_GAVEUP],
        "single_core": n in (5, 6),          # T5perf: the level forces core 1 off
        "prof_share_pct": {k: round(v, 1) for k, v in share.items()},
    }
    if n == 2:
        summary["pass"] = {
            "mean_under_70": summary["cpu_pct_mean"] < 70.0,
            "max_under_85": summary["cpu_pct_max"] < 85,
            "no_xruns": summary["xruns"] == 0,
            "guard_never": summary["guard_engagements"] == 0 and not summary["guard_active"],
        }
        summary["pass"]["rc1_ok"] = all(summary["pass"].values())
    return summary


def main():
    ap = argparse.ArgumentParser(description="FM-1 CPU-headroom probe (RC1p)")
    ap.add_argument("--stress", type=int, choices=[2, 3, 4, 5], help="stress level to run (TCLS: the old X0X baseline levels 1/6 are gone)")
    ap.add_argument("--cap", type=int, default=0, help="FM6 voice cap for --stress 4 (6 or 8)")
    ap.add_argument("--seconds", type=float, default=60.0, help="how long to sample (default 60)")
    ap.add_argument("--port", help="the FM-1's MIDI port name")
    ap.add_argument("--json", metavar="FILE", help="also write the JSON summary here")
    ap.add_argument("--info", action="store_true", help="print the device identity (fm1_install.py --info)")
    a = ap.parse_args()

    if a.info:
        cmd = [sys.executable, os.path.join(HERE, "fm1_install.py"), "--info"]
        if a.port:
            cmd += ["--port", a.port]
        return subprocess.call(cmd)
    if not a.stress:
        ap.error("give --stress N, or --info")

    p = Probe(a.port)
    tag = (" cap %d" % a.cap) if a.stress == 4 and a.cap else ""
    if a.stress == 5:
        tag = " (core 1 forced off)"
    print("rc_probe: %s, stress %d%s, %.0f s" % (p.name, a.stress, tag, a.seconds))
    summary = run_probe(p, a.stress, a.cap, a.seconds)
    text = json.dumps(summary, indent=2)
    print(text)
    if a.json:
        with open(a.json, "w") as f:
            f.write(text + "\n")
    if a.stress == 2:
        print("rc_probe: RC1 %s" % ("PASS" if summary["pass"]["rc1_ok"] else "FAIL"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
