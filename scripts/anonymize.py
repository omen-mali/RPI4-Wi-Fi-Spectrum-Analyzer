#!/usr/bin/env python3
"""Anonymize wpa_supplicant scan output or an analyzer log for committing.

Usage:
    anonymize.py scan <in> <out>    SCAN_RESULTS text (header + tab-separated rows)
    anonymize.py log  <in> <out>    wifi-analyzer log (with the logged STATUS reply)

Every MAC-shaped token becomes a stable, locally administered
02:00:00:xx:xx:xx address, numbered in first-seen order. In scan mode the SSID
column becomes net-NN (first-seen order; an empty SSID stays empty). In log
mode the values of ssid=, bssid=, address=, p2p_device_address= and uuid= are
replaced, and ip_address= lines are dropped. The same input always gives the
same output.
"""

import re
import sys

MAC_RE = re.compile(r"\b[0-9a-f]{2}(?::[0-9a-f]{2}){5}\b", re.IGNORECASE)
KEY_RE = re.compile(r"^(ssid|bssid|address|p2p_device_address|uuid)=(.*)$")


class Mapper:
    """Stable first-seen mappings for MACs, SSIDs and UUIDs."""

    def __init__(self):
        self.macs = {}
        self.ssids = {}
        self.uuids = {}

    def mac(self, value):
        key = value.lower()
        if key not in self.macs:
            n = len(self.macs) + 1
            self.macs[key] = "02:00:00:%02x:%02x:%02x" % (
                (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF)
        return self.macs[key]

    def ssid(self, value):
        if value == "":
            return ""
        if value not in self.ssids:
            self.ssids[value] = "net-%02d" % (len(self.ssids) + 1)
        return self.ssids[value]

    def uuid(self, value):
        if value not in self.uuids:
            self.uuids[value] = "00000000-0000-0000-0000-%012d" % (len(self.uuids) + 1)
        return self.uuids[value]

    def macs_in(self, text):
        return MAC_RE.sub(lambda m: self.mac(m.group(0)), text)


def anonymize_scan(text, mapper):
    """Anonymize SCAN_RESULTS text; the header line is kept as is."""
    lines = text.split("\n")
    out = lines[:1]
    for line in lines[1:]:
        fields = line.split("\t")
        if len(fields) >= 5:
            fields[4] = mapper.ssid("\t".join(fields[4:]))
            fields = fields[:5]
        out.append(mapper.macs_in("\t".join(fields)))
    return "\n".join(out)


def anonymize_log(text, mapper):
    """Anonymize an analyzer log, including the STATUS reply it logs."""
    out = []
    for line in text.split("\n"):
        if line.startswith("ip_address="):
            continue
        m = KEY_RE.match(line)
        if m:
            key, value = m.groups()
            if key == "ssid":
                value = mapper.ssid(value)
            elif key == "uuid":
                value = mapper.uuid(value)
            else:
                value = mapper.mac(value) if MAC_RE.fullmatch(value) else "02:00:00:00:00:00"
            line = "%s=%s" % (key, value)
        out.append(mapper.macs_in(line))
    return "\n".join(out)


def main(argv):
    if len(argv) != 4 or argv[1] not in ("scan", "log"):
        sys.stderr.write(__doc__)
        return 2
    mode, src, dst = argv[1:]
    with open(src, encoding="utf-8", errors="surrogateescape", newline="") as f:
        text = f.read()
    fn = anonymize_scan if mode == "scan" else anonymize_log
    result = fn(text, Mapper())
    with open(dst, "w", encoding="utf-8", errors="surrogateescape", newline="") as f:
        f.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
