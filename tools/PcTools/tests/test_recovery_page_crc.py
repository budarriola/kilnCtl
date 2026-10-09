#!/usr/bin/env python3
"""The recovery page's pure-JS CRC32 (the region between /*CRC-BEGIN*/ and
/*CRC-END*/ in firmware/KilnFW_recovery/main/recovery_page.html) must agree
with zlib.crc32. The browser computes the Pico image CRC (owner decision), the
firmware recomputes and compares it, so a typo here silently fails every Pico
upload. The recovery image has no authentication (owner decision 2026-10-02,
docs/RECOVERY_IMAGE_PLAN.md), so the page must also carry no challenge/sign
code: the last two tests keep it that way.

Runs the extracted JS under node; skipped if node is not on PATH (the
check_recovery_page_crc.ps1 wrapper refuses to grade a skip as a pass).
Run with: python -m unittest tests.test_recovery_page_crc  (from tools/PcTools)
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
PAGE = os.path.join(HERE, "..", "..", "..", "firmware", "KilnFW_recovery", "main",
                    "recovery_page.html")

CRC_INPUTS = [b"", b"123456789", b"a" * 55, bytes(range(256)) * 3, b"\xff" * 1000]


def page_text() -> str:
    with open(PAGE, encoding="utf-8") as f:
        return f.read()


def extract_crc_js() -> str:
    m = re.search(r"/\*CRC-BEGIN\*/(.*?)/\*CRC-END\*/", page_text(), re.DOTALL)
    if not m:
        raise AssertionError("CRC-BEGIN/CRC-END markers not found in recovery_page.html")
    return m.group(1)


@unittest.skipUnless(shutil.which("node"), "node not on PATH")
class RecoveryPageCrc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        js = extract_crc_js() + """
const inputs = JSON.parse(process.argv[1]);
process.stdout.write(JSON.stringify(inputs.map(h =>
  crc32(Uint8Array.from(h.match(/../g) || [], x => parseInt(x, 16))))));
"""
        args = ["node", "-e", js, json.dumps([b.hex() for b in CRC_INPUTS])]
        r = subprocess.run(args, capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            raise AssertionError(f"node failed: {r.stderr}")
        cls.out = json.loads(r.stdout)

    def test_crc32_check_value(self):
        self.assertEqual(self.out[1], 0xCBF43926)

    def test_crc32_matches_zlib(self):
        for data, got in zip(CRC_INPUTS, self.out):
            self.assertEqual(got, zlib.crc32(data), f"len {len(data)}")


class RecoveryPageHasNoAuth(unittest.TestCase):
    def test_no_challenge_or_signature_code(self):
        text = page_text()
        for needle in ("X-Ota-Mac", "/api/ota/challenge", "deriveMac", "hmac(", "sha256("):
            self.assertNotIn(needle, text, needle)

    def test_no_password_field(self):
        self.assertNotRegex(page_text(), r'type="password"')


if __name__ == "__main__":
    unittest.main()
