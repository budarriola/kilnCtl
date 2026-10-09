#!/usr/bin/env python3
"""Unit tests for kilnctrl.totp -- pure-Python RFC 6238 TOTP helper.
No board, no network, no filesystem: this exercises the HMAC/counter math
against RFC 6238 Appendix B's own published test vectors, the same vectors
WT-D's (not yet written) firmware host tests are expected to reuse.

Run with: python -m pytest tools/PcTools/tests/test_totp.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import totp  # noqa: E402

# RFC 6238 Appendix B: the SHA1 seed is the ASCII string "12345678901234567890"
# (20 bytes), used directly as the HMAC key (not base32-decoded -- the RFC's
# own vectors are stated in terms of the raw key bytes). Table entries are
# (unix_time, expected_8_digit_code).
_SHA1_SECRET = b"12345678901234567890"
_APPENDIX_B_SHA1_VECTORS = (
    (59, "94287082"),
    (1111111109, "07081804"),
    (1111111111, "14050471"),
    (1234567890, "89005924"),
    (2000000000, "69279037"),
    (20000000000, "65353130"),
)


class RfcAppendixBVectorTest(unittest.TestCase):
    """8-digit truncation at the RFC's own listed seed/counter values -- the
    underlying HMAC/counter math is identical up to the final modulus, so
    this validates the same code path :func:`totp.totp` uses for the
    project's fixed 6-digit codes (see totp.py's module docstring)."""

    def test_appendix_b_sha1_vectors_8_digit(self):
        for at_time, expected in _APPENDIX_B_SHA1_VECTORS:
            with self.subTest(at_time=at_time):
                got = totp.totp(_SHA1_SECRET, at_time=at_time, digits=8)
                self.assertEqual(got, expected)

    def test_appendix_b_sha1_vectors_6_digit_is_8_digit_suffix(self):
        """The project's fixed 6-digit code is the same computed value,
        truncated to fewer digits by construction (mod 10**6 instead of
        10**8) -- so it must equal the last 6 characters of the 8-digit
        vector, not an unrelated recomputation."""
        for at_time, expected8 in _APPENDIX_B_SHA1_VECTORS:
            with self.subTest(at_time=at_time):
                got6 = totp.totp(_SHA1_SECRET, at_time=at_time, digits=6)
                self.assertEqual(got6, expected8[-6:])

    def test_hotp_matches_totp_at_same_counter(self):
        at_time, expected8 = _APPENDIX_B_SHA1_VECTORS[0]
        counter = at_time // totp.DEFAULT_PERIOD_S
        self.assertEqual(totp.hotp(_SHA1_SECRET, counter, digits=8), expected8)


class VerifyWindowTest(unittest.TestCase):
    def test_exact_step_matches(self):
        at_time, _ = _APPENDIX_B_SHA1_VECTORS[0]
        code = totp.totp(_SHA1_SECRET, at_time=at_time)
        self.assertTrue(totp.verify_totp(_SHA1_SECRET, code, at_time=at_time))

    def test_one_step_before_and_after_match_within_window(self):
        at_time, _ = _APPENDIX_B_SHA1_VECTORS[0]
        period = totp.DEFAULT_PERIOD_S
        code = totp.totp(_SHA1_SECRET, at_time=at_time)
        self.assertTrue(totp.verify_totp(_SHA1_SECRET, code, at_time=at_time - period))
        self.assertTrue(totp.verify_totp(_SHA1_SECRET, code, at_time=at_time + period))

    def test_two_steps_away_is_outside_default_window(self):
        at_time, _ = _APPENDIX_B_SHA1_VECTORS[0]
        period = totp.DEFAULT_PERIOD_S
        code = totp.totp(_SHA1_SECRET, at_time=at_time)
        self.assertFalse(totp.verify_totp(_SHA1_SECRET, code, at_time=at_time + 2 * period))

    def test_wrong_code_never_matches(self):
        at_time, _ = _APPENDIX_B_SHA1_VECTORS[0]
        self.assertFalse(totp.verify_totp(_SHA1_SECRET, "000000", at_time=at_time))

    def test_whitespace_around_code_is_tolerated(self):
        at_time, _ = _APPENDIX_B_SHA1_VECTORS[0]
        code = totp.totp(_SHA1_SECRET, at_time=at_time)
        self.assertTrue(totp.verify_totp(_SHA1_SECRET, f"  {code}\n", at_time=at_time))


class Base32RoundTripTest(unittest.TestCase):
    def test_round_trip_various_lengths(self):
        for n in (0, 1, 5, 10, 20, 21, 32):
            data = bytes((i * 7 + 3) % 256 for i in range(n))
            encoded = totp.base32_encode(data)
            self.assertEqual(totp.base32_decode(encoded), data)

    def test_encode_has_no_padding_and_is_uppercase(self):
        encoded = totp.base32_encode(b"12345678901234567890")
        self.assertNotIn("=", encoded)
        self.assertEqual(encoded, encoded.upper())

    def test_decode_accepts_padding_and_lowercase(self):
        encoded = totp.base32_encode(b"hello world!!")
        decoded_padded = totp.base32_decode(encoded.lower() + "===")
        self.assertEqual(decoded_padded, b"hello world!!")

    def test_decode_rejects_invalid_character(self):
        with self.assertRaises(ValueError):
            totp.base32_decode("this-is-not-base32-1")


class NegativeGuardTest(unittest.TestCase):
    """Every check here needs a case that actually fails when the guard is
    broken, per this repo's negative-test-every-check rule -- these confirm
    the window/replay-adjacent logic in :func:`verify_totp` is doing real
    work rather than trivially returning True."""

    def test_verify_totp_rejects_when_window_is_zero_and_time_drifted(self):
        at_time, _ = _APPENDIX_B_SHA1_VECTORS[0]
        period = totp.DEFAULT_PERIOD_S
        code = totp.totp(_SHA1_SECRET, at_time=at_time)
        self.assertFalse(
            totp.verify_totp(_SHA1_SECRET, code, at_time=at_time + period, window=0))

    def test_different_secret_never_matches(self):
        at_time, _ = _APPENDIX_B_SHA1_VECTORS[0]
        code = totp.totp(_SHA1_SECRET, at_time=at_time)
        self.assertFalse(totp.verify_totp(b"a different secret!!", code, at_time=at_time))


if __name__ == "__main__":
    unittest.main()
