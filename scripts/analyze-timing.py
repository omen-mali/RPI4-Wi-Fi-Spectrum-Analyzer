#!/usr/bin/env python3
"""Derive scan-loop timing metrics from wifi_spectrum_analyzer logs.

Usage: analyze-timing.py [--json] <phase1.log> [<phase2.log> ...]

Log timestamps are wall-clock with 1 ms resolution, so every metric below
carries +/-1 ms of quantization (plus the cost of the log call itself).

Per scan cycle the scanner logs, in order:
  A  Sending wpa_supplicant command: SCAN
  B  SCAN accepted
  C  Monitor event: <3>CTRL-EVENT-SCAN-RESULTS   (poll() on the monitor socket;
                                                  older builds: 100 ms polling)
  D  Scan complete: N APs found                  (published; then sleeps)
  E  Analysis complete: ...                      (analyzer thread)

Metrics:
  cycle period      A[i+1] - A[i]
  period jitter     cycle period - mean(cycle period)
  wake latency      fixed-rate builds: logged "Wake lateness: N us" (actual -
                    deadline, CLOCK_MONOTONIC, microsecond resolution)
                    relative-sleep builds: A[i+1] - D[i] - 5000 ms proxy
  schedule error    fixed-rate builds: A[i] - (A[0] + i * period)  (drift)
  cmd round trip    B - A
  scan duration     C - B   (firmware scan; + up to 100 ms with older polling)
  event->publish    D - C   (SCAN_RESULTS request + parse + publish)
  publish->analysis E - D   (cond-var hand-off to the RR analyzer thread)

--json prints one JSON object keyed by log path instead of the tables. Each
value has cycles, period_ms (null for relative-sleep builds), overruns, warn,
error, max_aps (largest N in "Scan complete: N APs found", or 0) and metrics:
{name: {n, mean, std, min, p50, p99, max} or null}, in milliseconds, with
the same names and values as the tables.
"""
import json
import re
import statistics
import sys
from datetime import datetime

LEGACY_SLEEP_MS = 5000.0   # relative sleep after each cycle (pre-fixed-rate builds)

LINE = re.compile(r"^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3})\] \[(\w+)\] \[[^\]]+\] (.*)$")
PATTERNS = {
    "A": re.compile(r"Sending wpa_supplicant command: SCAN$"),
    "B": re.compile(r"SCAN (accepted|busy)"),
    "C": re.compile(r"CTRL-EVENT-SCAN-RESULTS"),
    "D": re.compile(r"Scan complete: \d+ APs found"),
    "E": re.compile(r"Analysis complete:"),
}
WAKE = re.compile(r"Wake lateness: (-?\d+) us")
PERIOD = re.compile(r"fixed-rate, period=(\d+) ms")
OVERRUN = re.compile(r"overran its \d+ ms period: skipped (\d+)")
APS = re.compile(r"Scan complete: (\d+) APs found")


def ts_ms(stamp):
    return datetime.strptime(stamp, "%Y-%m-%d %H:%M:%S.%f").timestamp() * 1000.0


def parse(path):
    events = {k: [] for k in PATTERNS}
    events["wake_us"] = []
    events["period_ms"] = None
    events["overruns"] = 0
    events["max_aps"] = 0
    levels = {"WARN": [], "ERROR": []}
    with open(path, errors="replace") as fh:
        for raw in fh:
            m = LINE.match(raw.rstrip("\n"))
            if not m:
                continue
            t, level, msg = ts_ms(m.group(1)), m.group(2), m.group(3)
            if level in levels:
                levels[level].append(msg)
            for key, pat in PATTERNS.items():
                if pat.search(msg):
                    events[key].append(t)
            if (w := WAKE.search(msg)):
                events["wake_us"].append(int(w.group(1)))
            elif (p := PERIOD.search(msg)):
                events["period_ms"] = float(p.group(1))
            elif (o := OVERRUN.search(msg)):
                events["overruns"] += int(o.group(1))
            elif (a := APS.search(msg)):
                events["max_aps"] = max(events["max_aps"], int(a.group(1)))
    return events, levels


def pairs(first, second, offset=0.0):
    """For each t in first, the first t2 in second with t2 >= t: t2 - t - offset."""
    out, j = [], 0
    for t in first:
        while j < len(second) and second[j] < t:
            j += 1
        if j == len(second):
            break
        out.append(second[j] - t - offset)
    return out


def next_after(first, second, offset=0.0):
    """D[i] -> next A after it (wake latency)."""
    return pairs(first, second, offset)


def stats(vals):
    if not vals:
        return None
    s = sorted(vals)
    p = lambda q: s[min(len(s) - 1, int(round(q * (len(s) - 1))))]
    return {
        "n": len(s), "mean": statistics.fmean(s),
        "std": statistics.pstdev(s) if len(s) > 1 else 0.0,
        "min": s[0], "p50": p(0.5), "p99": p(0.99), "max": s[-1],
    }


def metrics(events):
    A, B, C, D, E = (events[k] for k in "ABCDE")
    periods = [b - a for a, b in zip(A, A[1:])]
    m = {"cycle period": periods}
    if periods:
        mean = statistics.fmean(periods)
        m["period jitter"] = [p - mean for p in periods]
    if events["wake_us"]:
        m["wake latency"] = [us / 1000.0 for us in events["wake_us"]]
        if events["period_ms"]:
            # Slot index from elapsed time, so skipped (overrun) slots count.
            P = events["period_ms"]
            m["schedule error"] = [a - A[0] - round((a - A[0]) / P) * P for a in A]
    else:
        m["wake latency"] = next_after(D, A, LEGACY_SLEEP_MS)
    m["cmd round trip"] = pairs(A, B)
    m["scan duration"] = pairs(B, C)
    m["event->publish"] = pairs(C, D)
    m["publish->analysis"] = pairs(D, E)
    return m


def summary(path):
    """The --json summary of one log."""
    events, levels = parse(path)
    return {
        "cycles": len(events["A"]),
        "period_ms": events["period_ms"],
        "overruns": events["overruns"],
        "warn": len(levels["WARN"]),
        "error": len(levels["ERROR"]),
        "max_aps": events["max_aps"],
        "metrics": {name: stats(vals) for name, vals in metrics(events).items()},
    }


def main(paths):
    print(__doc__.split("\n\n")[2].strip())
    print()
    hdr = f"{'metric (ms)':<20}{'n':>5}{'mean':>11}{'std':>9}{'min':>10}{'p50':>10}{'p99':>10}{'max':>10}"
    for path in paths:
        events, levels = parse(path)
        print(f"== {path}")
        mode = (f"fixed-rate {events['period_ms']:.0f} ms" if events["period_ms"]
                else "relative sleep")
        print(f"   cycles: {len(events['A'])}   mode: {mode}   overrun slots: {events['overruns']}"
              f"   WARN: {len(levels['WARN'])}   ERROR: {len(levels['ERROR'])}")
        for msg in (levels["WARN"] + levels["ERROR"])[:5]:
            print(f"   ! {msg}")
        print(hdr)
        for name, vals in metrics(events).items():
            st = stats(vals)
            if st is None:
                print(f"{name:<20}{'(no data)':>15}")
                continue
            print(f"{name:<20}{st['n']:>5}{st['mean']:>11.3f}{st['std']:>9.3f}"
                  f"{st['min']:>10.3f}{st['p50']:>10.3f}{st['p99']:>10.3f}{st['max']:>10.3f}")
        A = events["A"]
        if len(A) > 1:
            # Drift of the scan grid vs. an ideal fixed period equal to the first period.
            span = A[-1] - A[0]
            print(f"   span {span / 1000:.1f} s over {len(A) - 1} periods "
                  f"-> effective rate {(len(A) - 1) / (span / 1000):.4f} scans/s")
        print()


if __name__ == "__main__":
    args = sys.argv[1:]
    as_json = bool(args) and args[0] == "--json"
    if as_json:
        args = args[1:]
    if not args:
        sys.exit(__doc__)
    if as_json:
        print(json.dumps({path: summary(path) for path in args}, indent=2))
    else:
        main(args)
