"""Tests for scripts/anonymize.py (run: python3 -m unittest discover -s tests/scripts)."""

import os
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCRIPT = os.path.join(ROOT, "scripts", "anonymize.py")
FIXTURES = os.path.join(ROOT, "tests", "fixtures", "scan")
MAC_RE = re.compile(r"[0-9a-f]{2}(?::[0-9a-f]{2}){5}", re.IGNORECASE)

REAL_SCAN = (
    "bssid / frequency / signal level / flags / ssid\n"
    "BC:FC:E7:5A:EE:08\t2432\t-45\t[WPA2-PSK-CCMP][ESS]\tHome Net\n"
    "a4:91:b1:00:12:34\t5180\t-60\t[ESS]\t\n"
    "bc:fc:e7:5a:ee:08\t2432\t-46\t[ESS]\tHome Net\n"
)


def run(mode, text):
    """Run anonymize.py in mode on text (through temp files) and return its output."""
    with tempfile.TemporaryDirectory() as d:
        src, dst = os.path.join(d, "in"), os.path.join(d, "out")
        with open(src, "w", newline="") as f:
            f.write(text)
        subprocess.run([sys.executable, SCRIPT, mode, src, dst], check=True)
        with open(dst, newline="") as f:
            return f.read()


def data_rows(text):
    """The non-empty rows of scan text, without the header line."""
    return [l for l in text.split("\n")[1:] if l]


class AnonymizeScanTest(unittest.TestCase):
    def test_stable(self):
        with open(os.path.join(FIXTURES, "basic.txt"), newline="") as f:
            basic = f.read()
        self.assertEqual(run("scan", basic), run("scan", basic))

    def test_no_input_bssid_or_ssid_survives(self):
        out = run("scan", REAL_SCAN)
        self.assertNotIn("bc:fc:e7", out.lower())
        self.assertNotIn("a4:91:b1", out.lower())
        self.assertNotIn("Home Net", out)
        for mac in MAC_RE.findall(out):
            self.assertTrue(mac.startswith("02:00:00:"), mac)

    def test_same_mac_any_case_maps_once(self):
        rows = data_rows(run("scan", REAL_SCAN))
        self.assertEqual(rows[0].split("\t")[0], rows[2].split("\t")[0])
        self.assertEqual(rows[0].split("\t")[4], "net-01")
        self.assertEqual(rows[1].split("\t")[4], "")   # hidden stays hidden

    def test_rows_still_parse(self):
        out = run("scan", REAL_SCAN)
        self.assertEqual(len(data_rows(out)), len(data_rows(REAL_SCAN)))
        for row in data_rows(out):
            self.assertEqual(len(row.split("\t")), 5)


class AnonymizeLogTest(unittest.TestCase):
    LOG = (
        "[2026-11-20 19:32:28.600] [INFO] [wifi_scanner.c:694] wpa_supplicant STATUS:\n"
        "bssid=bc:fc:e7:5a:ee:08\n"
        "ssid=Home Net\n"
        "ip_address=192.0.2.10\n"
        "address=dc:a6:32:01:02:03\n"
        "p2p_device_address=de:a6:32:01:02:03\n"
        "uuid=12345678-aaaa-bbbb-cccc-1234567890ab\n"
        "wpa_state=COMPLETED\n"
    )

    def test_identifiers_replaced(self):
        out = run("log", self.LOG)
        self.assertNotIn("Home Net", out)
        self.assertNotIn("192.0.2.10", out)
        self.assertNotIn("12345678-aaaa", out)
        self.assertIn("ssid=net-01\n", out)
        self.assertIn("bssid=02:00:00:00:00:01\n", out)     # mapped once, not twice
        self.assertIn("address=02:00:00:00:00:02\n", out)
        self.assertIn("wpa_state=COMPLETED", out)
        for mac in MAC_RE.findall(out):
            self.assertTrue(mac.startswith("02:00:00:"), mac)


if __name__ == "__main__":
    unittest.main()
