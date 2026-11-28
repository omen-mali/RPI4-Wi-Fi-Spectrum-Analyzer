#!/usr/bin/env python3
"""Pass/fail verdict for a Pi smoke-run log (used by scripts/ci-local.sh).

Usage: smoke-check.py --seconds N --p99-max-ms X --min-aps K [--json-out PATH] <smoke.log>

The criteria don't depend on the RF environment. Checked in this order,
and the first that fails is reported:
  1. no ERROR lines
  2. at least floor(N * 1000 / P) - 2 completed scans ("Scan complete"),
     where P is the period the app logged ("fixed-rate, period=P ms")
  3. logged wake lateness samples, with p99 <= X ms
  4. at least K APs in some scan (the radio and supplicant are up)
  5. a clean shutdown: main.c's "=== ... stopped (exit_code=0) ===" banner

Prints one line, "PASS scans=..., p99=...ms, aps=..." (exit 0) or
"FAIL <criterion>" (exit 1). --json-out also writes the log's summary, in
the schema of analyze-timing.py --json, whatever the verdict.
"""

import argparse
import importlib.util
import json
import os
import sys


def load_analyzer():
    """Import analyze-timing.py, whose hyphenated name rules out a plain import."""
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "analyze-timing.py")
    spec = importlib.util.spec_from_file_location("analyze_timing", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def verdict(s, seconds, p99_max_ms, min_aps, clean_stop):
    """(passed, message) for summary s of a smoke log, clean_stop names the banner."""
    if s["error"]:
        return False, f"{s['error']} ERROR line(s)"
    if not s["period_ms"]:
        return False, "no fixed-rate period in the log"
    need = int(seconds * 1000 // s["period_ms"]) - 2
    if s["completed"] < need:
        return False, f"scans={s['completed']}, need >= {need}"
    if not s["wake_samples"]:
        return False, "no wake lateness samples"
    p99 = s["metrics"]["wake latency"]["p99"]
    if p99 > p99_max_ms:
        return False, f"wake latency p99={p99:g}ms, max {p99_max_ms:g}ms"
    if s["max_aps"] < min_aps:
        return False, f"aps={s['max_aps']}, need >= {min_aps}"
    if not s["clean_stop"]:
        return False, f"no clean shutdown ({clean_stop})"
    return True, f"scans={s['completed']}, p99={p99:g}ms, aps={s['max_aps']}"


def main(argv):
    """Parse the arguments, print the verdict line and return the exit code."""
    ap = argparse.ArgumentParser(description="Pass/fail verdict for a Pi smoke-run log.")
    ap.add_argument("--seconds", type=int, required=True, help="run length")
    ap.add_argument("--p99-max-ms", type=float, required=True, help="wake latency p99 limit")
    ap.add_argument("--min-aps", type=int, required=True, help="fewest APs one scan must see")
    ap.add_argument("--json-out", help="also write the log's summary here")
    ap.add_argument("log")
    a = ap.parse_args(argv)
    if not os.path.isfile(a.log):
        passed, message = False, f"no log ({os.path.basename(a.log)})"
    else:
        analyzer = load_analyzer()
        s = analyzer.summary(a.log)
        if a.json_out:
            with open(a.json_out, "w") as f:
                json.dump({a.log: s}, f, indent=2)
                f.write("\n")
        passed, message = verdict(s, a.seconds, a.p99_max_ms, a.min_aps, analyzer.CLEAN_STOP)
    print(("PASS " if passed else "FAIL ") + message)
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
