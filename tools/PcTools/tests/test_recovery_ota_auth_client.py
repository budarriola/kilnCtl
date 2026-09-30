#!/usr/bin/env python3
"""Unit tests for kilnctrl.recovery_ota_auth_client -- the AP-password
X-Ota-Mac signer kept alive ONLY for firmware/KilnFW_recovery/ after the main
app retired the whole scheme 2026-09-29 (WEB_AUTH_PLAN.md item 2b, owner
decision "Retire; open when login off"). All against MOCKED urllib
responses -- no real socket and no live board is used or required.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import hashlib
import hmac
import io
import json
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import recovery_ota_auth_client as rec  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class DeriveMacTest(unittest.TestCase):
    def test_matches_reference_derivation(self):
        ap_password = "s3cret-ap-pw"
        nonce = bytes(range(16))
        key = hmac.new(ap_password.encode("utf-8"), b"kilnctl-ota-v1", hashlib.sha256).digest()
        expected = hmac.new(key, nonce + b"esp", hashlib.sha256).digest()
        self.assertEqual(rec.derive_mac(ap_password, nonce, "esp"), expected)

    def test_distinct_contexts_give_distinct_macs(self):
        ap_password = "pw"
        nonce = b"\x01" * 16
        macs = {ctx: rec.derive_mac(ap_password, nonce, ctx) for ctx in
                ("esp", "boot-guard-reset", "sw-reset")}
        self.assertEqual(len(set(macs.values())), 3)

    def test_rejects_unknown_context(self):
        with self.assertRaises(ValueError):
            rec.derive_mac("pw", b"\x00" * 16, "factory-reset")  # retired main-app context

    def test_rejects_retired_main_app_contexts(self):
        for ctx in ("esp-rollback", "pico", "pico-rollback", "recovery", "factory-reset"):
            with self.assertRaises(ValueError):
                rec.derive_mac("pw", b"\x00" * 16, ctx)


class GetChallengeTest(unittest.TestCase):
    def test_parses_valid_nonce(self):
        nonce_hex = "00" * 16
        body = json.dumps({"nonce": nonce_hex}).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            nonce = rec.get_challenge("192.168.1.50")
        self.assertEqual(nonce, b"\x00" * 16)

    def test_rejects_wrong_length_nonce(self):
        body = json.dumps({"nonce": "00" * 8}).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            with self.assertRaises(rec.RecoveryOtaAuthError):
                rec.get_challenge("192.168.1.50")

    def test_rejects_malformed_json(self):
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(b"not json")):
            with self.assertRaises(rec.RecoveryOtaAuthError):
                rec.get_challenge("192.168.1.50")

    def test_http_error_wrapped(self):
        err = urllib.error.HTTPError("url", 500, "boom", {}, io.BytesIO(b"detail"))
        with unittest.mock.patch("urllib.request.urlopen", side_effect=err):
            with self.assertRaises(rec.RecoveryOtaAuthError) as ctx:
                rec.get_challenge("192.168.1.50")
        self.assertEqual(ctx.exception.status, 500)


class SignedPostTest(unittest.TestCase):
    def _mock_challenge_then(self, post_body: bytes, post_status: int = 200):
        nonce_hex = "11" * 16
        challenge_resp = _fake_response(json.dumps({"nonce": nonce_hex}).encode("utf-8"))
        post_resp = _fake_response(post_body, post_status)
        return unittest.mock.patch("urllib.request.urlopen", side_effect=[challenge_resp, post_resp])

    def test_boot_guard_reset_sends_correct_header_and_path(self):
        captured = {}
        real_request = None

        def fake_urlopen(req, timeout=None):
            if "challenge" in req.full_url:
                return _fake_response(json.dumps({"nonce": "22" * 16}).encode("utf-8"))
            captured["url"] = req.full_url
            captured["mac"] = req.get_header("X-ota-mac")
            return _fake_response(b"boot_guard counter cleared")

        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
            result = rec.recovery_boot_guard_reset("10.0.0.5", "the-ap-password")

        self.assertEqual(result["status"], 200)
        self.assertEqual(result["text"], "boot_guard counter cleared")
        self.assertIn("/api/ota/esp/boot_guard_reset", captured["url"])
        expected_mac = rec.derive_mac("the-ap-password", bytes.fromhex("22" * 16),
                                       "boot-guard-reset").hex()
        self.assertEqual(captured["mac"], expected_mac)

    def test_sw_reset_path_and_context(self):
        captured = {}

        def fake_urlopen(req, timeout=None):
            if "challenge" in req.full_url:
                return _fake_response(json.dumps({"nonce": "33" * 16}).encode("utf-8"))
            captured["url"] = req.full_url
            captured["mac"] = req.get_header("X-ota-mac")
            return _fake_response(b"resetting")

        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
            result = rec.recovery_sw_reset("10.0.0.5", "pw")

        self.assertEqual(result["status"], 200)
        self.assertEqual(result["text"], "resetting")
        self.assertIn("/api/sw_reset", captured["url"])
        expected_mac = rec.derive_mac("pw", bytes.fromhex("33" * 16), "sw-reset").hex()
        self.assertEqual(captured["mac"], expected_mac)

    def test_push_esp_image_sends_body_and_esp_context(self):
        captured = {}

        def fake_urlopen(req, timeout=None):
            if "challenge" in req.full_url:
                return _fake_response(json.dumps({"nonce": "44" * 16}).encode("utf-8"))
            captured["url"] = req.full_url
            captured["mac"] = req.get_header("X-ota-mac")
            captured["data"] = req.data
            return _fake_response(b"ok, rebooting into new application image")

        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
            result = rec.recovery_push_esp_image("10.0.0.5", b"fake-image-bytes", "pw")

        self.assertEqual(result["status"], 200)
        self.assertEqual(result["text"], "ok, rebooting into new application image")
        self.assertIn("/api/ota/esp", captured["url"])
        self.assertNotIn("boot_guard", captured["url"])
        self.assertEqual(captured["data"], b"fake-image-bytes")
        expected_mac = rec.derive_mac("pw", bytes.fromhex("44" * 16), "esp").hex()
        self.assertEqual(captured["mac"], expected_mac)

    def test_refusal_wrapped_and_password_never_in_error(self):
        err = urllib.error.HTTPError("url", 403, "forbidden", {}, io.BytesIO(b"bad mac"))

        def fake_urlopen(req, timeout=None):
            if "challenge" in req.full_url:
                return _fake_response(json.dumps({"nonce": "55" * 16}).encode("utf-8"))
            raise err

        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
            with self.assertRaises(rec.RecoveryOtaAuthError) as ctx:
                rec.recovery_boot_guard_reset("10.0.0.5", "super-secret-password")

        self.assertEqual(ctx.exception.status, 403)
        self.assertNotIn("super-secret-password", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
