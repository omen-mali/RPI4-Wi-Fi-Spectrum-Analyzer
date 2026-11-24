"""Tests for scripts/analyze-timing.py (run: python3 -m unittest discover -s tests/scripts)."""

import json
import os
import subprocess
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "scripts", "analyze-timing.py")
FIXTURE = os.path.join(ROOT, "tests", "fixtures", "timing", "smoke.txt")
EXPECTED = os.path.join(ROOT, "tests", "fixtures", "timing", "smoke.expected.json")


def run(*args):
    return subprocess.run([sys.executable, SCRIPT, *args], check=True,
                          capture_output=True, text=True).stdout


class AnalyzeTimingTest(unittest.TestCase):
    def assert_close(self, got, want, path="$"):
        if isinstance(want, dict):
            self.assertIsInstance(got, dict, path)
            self.assertEqual(sorted(got), sorted(want), path)
            for k in want:
                self.assert_close(got[k], want[k], "%s.%s" % (path, k))
        elif isinstance(want, float):
            self.assertAlmostEqual(got, want, places=3, msg=path)
        else:
            self.assertEqual(got, want, path)

    def test_json_matches_expected(self):
        out = json.loads(run("--json", FIXTURE))
        self.assertEqual(list(out), [FIXTURE])          # keyed by the path given
        with open(EXPECTED) as f:
            want = json.load(f)
        self.assert_close(out[FIXTURE], want)

    def test_json_fields(self):
        summary = json.loads(run("--json", FIXTURE))[FIXTURE]
        self.assertEqual(summary["cycles"], 18)
        self.assertEqual(summary["period_ms"], 10000.0)
        self.assertEqual(summary["error"], 0)
        self.assertGreater(summary["max_aps"], 0)
        self.assertIn("p99", summary["metrics"]["wake latency"])

    def test_text_mode_unchanged(self):
        out = run(FIXTURE)
        self.assertTrue(out.startswith("Log timestamps are wall-clock with 1 ms resolution"))
        self.assertIn("cycles: 18", out)
        self.assertIn("wake latency", out)


if __name__ == "__main__":
    unittest.main()
