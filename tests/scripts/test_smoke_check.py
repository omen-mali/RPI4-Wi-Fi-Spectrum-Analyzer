"""Tests for scripts/smoke-check.py (run: python3 -m unittest discover -s tests/scripts)."""

import json
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

# The fixture: 180 s, 18 completed scans, 10000 ms period logged, wake latency p99 0.012 ms, 17 APs at most.
def limits(seconds=180, p99_max_ms=2.0, min_aps=1):
    """smoke-check.py's limit arguments; the defaults pass on the fixture."""
    return ["--seconds", str(seconds), "--p99-max-ms", str(p99_max_ms), "--min-aps", str(min_aps)]


PASSING = limits()


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

    def log_with_period(self, period_ms):
        """The fixture with its logged period changed to period_ms, plus STOP_OK."""
        path = os.path.join(self.dir, "smoke.log")
        with open(FIXTURE) as src, open(path, "w") as dst:
            dst.write(src.read().replace("period=10000 ms", "period=%d ms" % period_ms))
            dst.write(STOP_OK)
        return path

    def log_without(self, text, keep=0):
        """The fixture minus lines containing text (all but the first keep), plus STOP_OK."""
        path = os.path.join(self.dir, "smoke.log")
        with open(FIXTURE) as src, open(path, "w") as dst:
            for line in src:
                if text in line:
                    if keep <= 0:
                        continue
                    keep -= 1
                dst.write(line)
            dst.write(STOP_OK)
        return path

    def check(self, path, *args):
        """Run smoke-check.py on path with args."""
        p = subprocess.run([sys.executable, SCRIPT, *args, path],
                           capture_output=True, text=True)
        return p.returncode, p.stdout.strip()

    def test_pass_prints_summary(self):
        rc, out = self.check(self.log(STOP_OK), *PASSING)
        self.assertEqual(rc, 0, out)
        self.assertEqual(out, "PASS scans=18, p99=0.012ms, aps=17")

    def test_error_line_fails(self):
        rc, out = self.check(self.log(ERROR_LINE, STOP_OK), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL 1 ERROR line(s)")

    def test_too_few_cycles_fails(self):
        # 600 s at 10 s needs at least 60 - 2 = 58 completed scans.
        rc, out = self.check(self.log(STOP_OK), *limits(seconds=600))
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL scans=18, need >= 58")

    def test_cycle_floor_allows_two_missing(self):
        # 200 s needs 20 - 2 = 18: exactly what the fixture has.
        rc, out = self.check(self.log(STOP_OK), *limits(seconds=200))
        self.assertEqual(rc, 0, out)

    def test_wake_latency_over_threshold_fails(self):
        rc, out = self.check(self.log(STOP_OK), *limits(p99_max_ms=0.01))
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL wake latency p99=0.012ms, max 0.01ms")

    def test_too_few_aps_fails(self):
        rc, out = self.check(self.log(STOP_OK), *limits(min_aps=18))
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL aps=17, need >= 18")

    def test_missing_clean_stop_fails(self):
        rc, out = self.check(self.log(), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL no clean shutdown (=== Wi-Fi Spectrum Analyzer stopped (exit_code=0) ===)")

    def test_nonzero_exit_code_fails(self):
        rc, out = self.check(self.log(STOP_BAD), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL no clean shutdown (=== Wi-Fi Spectrum Analyzer stopped (exit_code=0) ===)")

    def test_first_failing_criterion_is_reported(self):
        # An ERROR line and a missing stop: the ERROR check comes first.
        rc, out = self.check(self.log(ERROR_LINE), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL 1 ERROR line(s)")

    def test_no_logged_period_fails(self):
        rc, out = self.check(self.log_without("fixed-rate, period="), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL no fixed-rate period in the log")

    def test_requested_but_uncompleted_scans_fail(self):
        # Every SCAN is still sent, but only 5 of 18 scans publish results.
        rc, out = self.check(self.log_without("Scan complete:", keep=5), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL scans=5, need >= 16")

    def test_no_wake_samples_fails(self):
        # Without logged lateness, wake latency would be the relative-sleep proxy.
        rc, out = self.check(self.log_without("Wake lateness"), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL no wake lateness samples")

    def test_scan_floor_uses_the_logged_period(self):
        # 180 s at a logged 5000 ms needs 36 - 2 = 34; the fixture has 18.
        rc, out = self.check(self.log_with_period(5000), *PASSING)
        self.assertEqual(rc, 1)
        self.assertEqual(out, "FAIL scans=18, need >= 34")

    def test_scan_floor_rounds_down(self):
        # 180 s at 8700 ms: floor(20.69) - 2 = 18, exactly what the fixture has.
        rc, out = self.check(self.log_with_period(8700), *PASSING)
        self.assertEqual(rc, 0, out)

    def test_stop_banner_without_log_prefix_fails(self):
        bare = "=== Wi-Fi Spectrum Analyzer stopped (exit_code=0) ===\n"
        rc, out = self.check(self.log(bare), *PASSING)
        self.assertEqual(rc, 1)
        self.assertTrue(out.startswith("FAIL no clean shutdown"), out)

    def test_stop_text_inside_another_message_fails(self):
        echoed = ("[2026-11-20 19:35:23.000] [DEBUG] [wifi_scanner.c:389] Monitor event: "
                  '<3>CTRL-EVENT-SSID-TEMP-DISABLED id=0 ssid="stopped (exit_code=0)"\n')
        rc, out = self.check(self.log(echoed), *PASSING)
        self.assertEqual(rc, 1)
        self.assertTrue(out.startswith("FAIL no clean shutdown"), out)

    def test_json_out_written_whatever_the_verdict(self):
        analyzer = os.path.join(ROOT, "scripts", "analyze-timing.py")
        for extra, want_rc in (((), 1), ((STOP_OK,), 0)):
            log = self.log(*extra)
            out_json = os.path.join(self.dir, "smoke-%d.json" % want_rc)
            rc, out = self.check(log, *PASSING, "--json-out", out_json)
            self.assertEqual(rc, want_rc, out)
            with open(out_json) as f:
                got = json.load(f)
            want = json.loads(subprocess.run([sys.executable, analyzer, "--json", log],
                                             check=True, capture_output=True, text=True).stdout)
            self.assertEqual(got, want)

    def test_missing_log_fails(self):
        rc, out = self.check(os.path.join(self.dir, "absent.log"), *PASSING)
        self.assertEqual(rc, 1)
        self.assertTrue(out.startswith("FAIL no log"), out)


if __name__ == "__main__":
    unittest.main()
