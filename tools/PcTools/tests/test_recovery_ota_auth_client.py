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
                ("esp", "boot-guard-reset", "sw-reset", "recovery-exit", "wifi-reset",
                 "pico-upload", "pico-abort")}
        self.assertEqual(len(set(macs.values())), 7)

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


def _firmware_msg(nonce: bytes, context: str, query: str) -> bytes:
    """Transcription of recovery_http.c recovery_authenticate_request()'s
    message construction: memcpy nonce; memcpy context; then, only when
    httpd_req_get_url_query_len() > 0, msg[msg_len++] = '?' and the raw query
    bytes. Written from the C, not from the client under test."""
    msg = bytearray(nonce)
    msg += context.encode("ascii")
    if len(query) > 0:
        msg += b"?"
        msg += query.encode("ascii")
    return bytes(msg)


class QueryBoundMacTest(unittest.TestCase):
    PW = "ap-pass-test"
    NONCE = bytes(range(16))

    def test_matches_firmware_message_construction(self):
        key = hmac.new(self.PW.encode(), b"kilnctl-ota-v1", hashlib.sha256).digest()
        for q in ("", "crc=deadbeef", "crc=deadbeef&slot=A", "crc=00000000&slot=B"):
            expected = hmac.new(key, _firmware_msg(self.NONCE, "pico-upload", q), hashlib.sha256).digest()
            self.assertEqual(rec.derive_mac(self.PW, self.NONCE, "pico-upload", q), expected, q)

    def test_frozen_vectors(self):
        # Computed independently from the C construction above (HMAC key =
        # HMAC(pw, "kilnctl-ota-v1"); msg = nonce||ctx||"?"||query).
        self.assertEqual(rec.derive_mac(self.PW, self.NONCE, "pico-upload", "crc=deadbeef&slot=A").hex(),
                         "de9db9c86361082805a5136cb3ed3b10cca85fee923eb66be86d66d60bed6359")
        self.assertEqual(rec.derive_mac(self.PW, self.NONCE, "pico-upload", "crc=deadbeef").hex(),
                         "78f5c341d4f8e80934f8cb8d59e4e8dc166493526816bc6a2fc15fcef1d37fd1")
        # No query: byte-identical to the pre-W4 signature (no trailing '?').
        self.assertEqual(rec.derive_mac(self.PW, self.NONCE, "pico-upload").hex(),
                         "a06055fbeb3cc3564b74ebabd6767cd49b2d193d3e44c638f3fea5fc12971a9f")

    def test_query_changes_the_mac(self):
        a = rec.derive_mac(self.PW, self.NONCE, "pico-upload", "crc=deadbeef&slot=A")
        b = rec.derive_mac(self.PW, self.NONCE, "pico-upload", "crc=deadbeef&slot=B")
        c = rec.derive_mac(self.PW, self.NONCE, "pico-upload")
        self.assertEqual(len({a, b, c}), 3)

    def test_bad_queries_refused(self):
        for q in ("?crc=deadbeef", "crc=dead beef", "crc=dead#beef", "crc=é", "x" * 96):
            with self.assertRaises(ValueError, msg=q):
                rec.derive_mac(self.PW, self.NONCE, "pico-upload", q)
        rec.derive_mac(self.PW, self.NONCE, "pico-upload", "x" * 95)  # exactly AUTH_QUERY_MAX is fine

    def test_pico_upload_query_builder(self):
        self.assertEqual(rec.pico_upload_query(0xDEADBEEF), "crc=deadbeef")
        self.assertEqual(rec.pico_upload_query(0x1, "B"), "crc=00000001&slot=B")
        for bad in ((-1, None), (1 << 32, None), (1, "C"), (1, "a")):
            with self.assertRaises(ValueError, msg=repr(bad)):
                rec.pico_upload_query(*bad)

    def test_signed_post_signs_exactly_what_it_sends(self):
        captured = {}

        def fake_urlopen(req, timeout=None):
            if "challenge" in req.full_url:
                return _fake_response(json.dumps({"nonce": self.NONCE.hex()}).encode("utf-8"))
            captured["url"] = req.full_url
            captured["mac"] = req.get_header("X-ota-mac")
            captured["data"] = req.data
            return _fake_response(b'{"started":true,"image_slot":"A"}', 202)

        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
            r = rec.recovery_pico_upload("10.0.0.5", b"IMG", 0xDEADBEEF, self.PW, slot="A")
        self.assertEqual(r["status"], 202)
        self.assertEqual(captured["url"], "http://10.0.0.5/api/recovery/pico/upload?crc=deadbeef&slot=A")
        self.assertEqual(captured["data"], b"IMG")
        query = captured["url"].split("?", 1)[1]
        self.assertEqual(captured["mac"],
                         rec.derive_mac(self.PW, self.NONCE, "pico-upload", query).hex())
        self.assertEqual(captured["mac"],
                         "de9db9c86361082805a5136cb3ed3b10cca85fee923eb66be86d66d60bed6359")

    def test_bad_query_never_fetches_a_challenge(self):
        with unittest.mock.patch("urllib.request.urlopen") as m:
            with self.assertRaises(ValueError):
                rec.signed_post("10.0.0.5", "/x", "pico-upload", "pw", query="a b")
            with self.assertRaises(ValueError):
                rec.signed_post("10.0.0.5", "/x?crc=1", "pico-upload", "pw")
        m.assert_not_called()

    def test_exit_and_wifi_reset_contexts(self):
        for fn, ctx, path in ((rec.recovery_exit, "recovery-exit", "/api/recovery/exit"),
                              (rec.recovery_wifi_reset, "wifi-reset", "/api/recovery/wifi_reset")):
            captured = {}

            def fake_urlopen(req, timeout=None, _c=captured):
                if "challenge" in req.full_url:
                    return _fake_response(json.dumps({"nonce": self.NONCE.hex()}).encode("utf-8"))
                _c["url"] = req.full_url
                _c["mac"] = req.get_header("X-ota-mac")
                return _fake_response(b"ok")

            with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
                fn("10.0.0.5", self.PW)
            self.assertTrue(captured["url"].endswith(path))
            self.assertNotIn("?", captured["url"])
            self.assertEqual(captured["mac"], rec.derive_mac(self.PW, self.NONCE, ctx).hex())


if __name__ == "__main__":
    unittest.main()
