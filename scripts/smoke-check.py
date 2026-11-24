#!/usr/bin/env python3
"""Pass/fail verdict for a Pi smoke-run log (used by scripts/ci-local.sh).

Usage: smoke-check.py --seconds N --period-ms P --p99-max-ms X --min-aps K <smoke.log>

The criteria don't depend on the RF environment. Checked in this order,
and the first that fails is reported:
  1. no ERROR lines
  2. at least floor(N * 1000 / P) - 2 scan cycles
  3. wake latency p99 <= X ms
  4. at least K APs in some scan (the radio and supplicant are up)
  5. a clean shutdown: "stopped (exit_code=0)"

Prints one line, "PASS cycles=..., p99=...ms, aps=..." (exit 0) or
"FAIL <criterion>" (exit 1).
"""

import argparse
import importlib.util
import os
import sys

CLEAN_STOP = "stopped (exit_code=0)"


def load_analyzer():
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "analyze-timing.py")
    spec = importlib.util.spec_from_file_location("analyze_timing", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verdict(path, seconds, period_ms, p99_max_ms, min_aps):
    """(passed, message) for the log at path."""
    if not os.path.isfile(path):
        return False, f"no log ({os.path.basename(path)})"
    s = load_analyzer().summary(path)
    wake = s["metrics"].get("wake latency")
    p99 = wake["p99"] if wake else None
    need_cycles = seconds * 1000 // period_ms - 2

    if s["error"]:
        return False, f"{s['error']} ERROR line(s)"
    if s["cycles"] < need_cycles:
        return False, f"cycles={s['cycles']}, need >= {need_cycles}"
    if p99 is None:
        return False, "no wake latency samples"
    if p99 > p99_max_ms:
        return False, f"wake latency p99={p99:g}ms, max {p99_max_ms:g}ms"
    if s["max_aps"] < min_aps:
        return False, f"aps={s['max_aps']}, need >= {min_aps}"
    with open(path, errors="replace") as f:
        if CLEAN_STOP not in f.read():
            return False, f"no clean shutdown ({CLEAN_STOP})"
    return True, f"cycles={s['cycles']}, p99={p99:g}ms, aps={s['max_aps']}"


def main(argv):
    ap = argparse.ArgumentParser(description="Pass/fail verdict for a Pi smoke-run log.")
    ap.add_argument("--seconds", type=int, required=True, help="run length")
    ap.add_argument("--period-ms", type=int, required=True, help="scan period in effect")
    ap.add_argument("--p99-max-ms", type=float, required=True, help="wake latency p99 limit")
    ap.add_argument("--min-aps", type=int, required=True, help="fewest APs one scan must see")
    ap.add_argument("log")
    a = ap.parse_args(argv)
    passed, message = verdict(a.log, a.seconds, a.period_ms, a.p99_max_ms, a.min_aps)
    print(("PASS " if passed else "FAIL ") + message)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
