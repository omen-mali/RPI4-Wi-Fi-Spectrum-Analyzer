"""Tests for scripts/smoke-check.py (run: python3 -m unittest discover -s tests/scripts)."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "scripts", "smoke-check.py")
FIXTURE = os.path.join(ROOT, "tests", "fixtures", "timing", "smoke.txt")

STOP_OK = "[2026-11-20 19:35:24.000] [INFO] [main.c:507] === Wi-Fi Spectrum Analyzer stopped (exit_code=0) ===\n"
STOP_BAD = "[2026-11-20 19:35:24.000] [INFO] [main.c:507] === Wi-Fi Spectrum Analyzer stopped (exit_code=1) ===\n"
ERROR_LINE = "[2026-11-20 19:35:23.900] [ERROR] [wifi_scanner.c:624] Failed to retrieve scan results\n"

# The fixture: 180 s, 18 cycles, 10000 ms period, wake latency p99 0.012 ms, 17 APs at most.
PASSING = ["--seconds", "180", "--period-ms", "10000", "--p99-max-ms", "2.0", "--min-aps", "1"]


class SmokeCheckTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir)

    def log(self, *extra):
        """The fixture with extra lines appended."""
        path = os.path.join(self.dir, "smoke.log")
        shutil.copyfile(FIXTURE, path)
        with open(path, "a") as f:
            f.writelines(extra)
        return path

    def check(self, path, *args):
        """Run smoke-check.py on path with args."""
        p = subprocess.run([sys.executable, SCRIPT, *args, path],
                           capture_output=True, text=True)
        return p.returncode, p.stdout.strip()

    def test_pass_prints_summary(self):
        rc, out = self.check(self.log(STOP_OK), *PASSING)
        self.assertEqual(rc, 0, out)
        self.assertEqual(out, "PASS cycles=18, p99=0.012ms, aps=17")

    def test_error_line_fails(self):
        rc, out = self.check(self.log(ERROR_LINE, STOP_OK), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL 1 ERROR line(s)")

    def test_too_few_cycles_fails(self):
        # 600 s at 10 s needs at least 60 - 2 = 58 cycles.
        args = list(PASSING)
        args[1] = "600"
        rc, out = self.check(self.log(STOP_OK), *args)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL cycles=18, need >= 58")

    def test_cycle_floor_allows_two_missing(self):
        # 200 s needs 20 - 2 = 18: exactly what the fixture has.
        args = list(PASSING)
        args[1] = "200"
        rc, out = self.check(self.log(STOP_OK), *args)
        self.assertEqual(rc, 0, out)

    def test_wake_latency_over_threshold_fails(self):
        args = list(PASSING)
        args[5] = "0.01"
        rc, out = self.check(self.log(STOP_OK), *args)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL wake latency p99=0.012ms, max 0.01ms")

    def test_too_few_aps_fails(self):
        args = list(PASSING)
        args[7] = "18"
        rc, out = self.check(self.log(STOP_OK), *args)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL aps=17, need >= 18")

    def test_missing_clean_stop_fails(self):
        rc, out = self.check(self.log(), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL no clean shutdown (stopped (exit_code=0))")

    def test_nonzero_exit_code_fails(self):
        rc, out = self.check(self.log(STOP_BAD), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL no clean shutdown (stopped (exit_code=0))")

    def test_first_failing_criterion_is_reported(self):
        # An ERROR line and a missing stop: the ERROR check comes first.
        rc, out = self.check(self.log(ERROR_LINE), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL 1 ERROR line(s)")

    def test_missing_log_fails(self):
        rc, out = self.check(os.path.join(self.dir, "absent.log"), *PASSING)
        self.assertEqual(rc, 1)
        self.assertTrue(out.startswith("FAIL no log"), out)


if __name__ == "__main__":
    unittest.main()
