#!/usr/bin/env python3
"""The recovery page's pure-JS SHA-256 / HMAC / CRC32 / deriveMac (the region
between /*CRYPTO-BEGIN*/ and /*CRYPTO-END*/ in
firmware/KilnFW_recovery/main/recovery_page.html) must agree byte-for-byte
with Python's hashlib/hmac/zlib and with kilnctrl.recovery_ota_auth_client.
derive_mac(), the PC-side signer the recovery firmware verifies. The page
cannot use crypto.subtle (plain-HTTP AP page is not a secure context), so this
hand-written copy is the only thing standing between a typo and a page that
silently fails every authenticated action.

Runs the extracted JS under node; skipped if node is not on PATH.
Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import shutil
import subprocess
import sys
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))

from kilnctrl import recovery_ota_auth_client as rec  # noqa: E402

PAGE = os.path.join(HERE, "..", "..", "..", "firmware", "KilnFW_recovery", "main",
                    "recovery_page.html")

# Fixed vectors: (password, nonce hex, context). Includes a password longer
# than the 64-byte HMAC block and non-ASCII to exercise key hashing and UTF-8.
VECTORS = [
    ("pw", "00" * 16, "esp"),
    ("correct horse battery staple", "0123456789abcdeffedcba9876543210", "recovery-exit"),
    ("kiln-ap-pass", "ff" * 16, "wifi-reset"),
    ("p" * 70, "a5" * 16, "esp"),
    ("pässö", "11" * 16, "wifi-reset"),
]
SHA_INPUTS = [b"", b"abc", b"a" * 55, b"a" * 56, b"a" * 64, bytes(range(256)) * 3]


def extract_crypto_js() -> str:
    with open(PAGE, encoding="utf-8") as f:
        text = f.read()
    m = re.search(r"/\*CRYPTO-BEGIN\*/(.*?)/\*CRYPTO-END\*/", text, re.DOTALL)
    if not m:
        raise AssertionError("CRYPTO-BEGIN/CRYPTO-END markers not found in recovery_page.html")
    return m.group(1)


@unittest.skipUnless(shutil.which("node"), "node not on PATH")
class RecoveryPageCrypto(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        js = extract_crypto_js() + """
const out = {sha: [], mac: [], crc: null};
const shaIn = JSON.parse(process.argv[1]);
for (const h of shaIn) out.sha.push(hex(sha256(unhex(h))));
for (const [pw, nonceHex, ctx] of JSON.parse(process.argv[2]))
  out.mac.push(hex(deriveMac(pw, unhex(nonceHex), ctx)));
out.crc = crc32(utf8("123456789"));
process.stdout.write(JSON.stringify(out));
"""
        args = ["node", "-e", js, json.dumps([b.hex() for b in SHA_INPUTS]), json.dumps(VECTORS)]
        r = subprocess.run(args, capture_output=True, text=True, timeout=60)
        if r.returncode != 0:
            raise AssertionError(f"node failed: {r.stderr}")
        cls.out = json.loads(r.stdout)

    def test_sha256_matches_hashlib(self):
        for data, got in zip(SHA_INPUTS, self.out["sha"]):
            self.assertEqual(got, hashlib.sha256(data).hexdigest(), f"len {len(data)}")

    def test_crc32_check_value(self):
        self.assertEqual(self.out["crc"], 0xCBF43926)
        self.assertEqual(self.out["crc"], zlib.crc32(b"123456789"))

    def test_derive_mac_matches_python_derive_mac(self):
        for (pw, nonce_hex, ctx), got in zip(VECTORS, self.out["mac"]):
            expected = rec.derive_mac(pw, bytes.fromhex(nonce_hex), ctx).hex()
            self.assertEqual(got, expected, f"{pw[:8]!r} {ctx}")

    def test_derive_mac_matches_independent_hmac(self):
        pw, nonce_hex, ctx = VECTORS[0]
        key = hmac.new(pw.encode(), b"kilnctl-ota-v1", hashlib.sha256).digest()
        expected = hmac.new(key, bytes.fromhex(nonce_hex) + ctx.encode(), hashlib.sha256).hexdigest()
        self.assertEqual(self.out["mac"][0], expected)


if __name__ == "__main__":
    unittest.main()
