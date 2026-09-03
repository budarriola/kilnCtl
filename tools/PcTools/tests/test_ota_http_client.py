#!/usr/bin/env python3
"""Unit tests for kilnctrl.ota_http_client -- request construction, HMAC
signing, and response parsing, all against MOCKED urllib responses. No real
socket and no live board is used or required.

These tests do NOT exercise ota_http.c on real hardware -- they only check
that this PC-side client builds the request the firmware documents
(ota_http.h/.c) and parses the firmware's documented response shapes
correctly. Live-board verification (does the real ESP actually accept these
bytes, does the challenge/lockout/interlock state machine behave as
expected end to end) is still outstanding -- see ROADMAP.md M8 / TODO.md 9.6.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import hashlib
import hmac
import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import ota_http_client as ota  # noqa: E402


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
    def test_matches_manual_double_hmac(self):
        nonce = bytes(range(16))
        expected_key = hmac.new(b"hunter2", b"kilnctl-ota-v1", hashlib.sha256).digest()
        expected = hmac.new(expected_key, nonce + b"esp", hashlib.sha256).digest()
        self.assertEqual(ota.derive_mac("hunter2", nonce, "esp"), expected)

    def test_esp_and_pico_contexts_diverge(self):
        nonce = bytes(range(16))
        mac_esp = ota.derive_mac("pw", nonce, "esp")
        mac_pico = ota.derive_mac("pw", nonce, "pico")
        self.assertNotEqual(mac_esp, mac_pico)

    def test_rejects_bad_context(self):
        with self.assertRaises(ValueError):
            ota.derive_mac("pw", bytes(16), "esp32")


class GetChallengeTest(unittest.TestCase):
    def test_parses_nonce_hex(self):
        nonce_hex = "00" * 16
        body = json.dumps({"nonce": nonce_hex}).encode()
        with unittest.mock.patch.object(ota.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            nonce = ota.get_challenge("192.168.4.1")
        self.assertEqual(nonce, bytes(16))

    def test_rejects_wrong_length_nonce(self):
        body = json.dumps({"nonce": "aa"}).encode()  # 1 byte, not 16
        with unittest.mock.patch.object(ota.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_challenge("192.168.4.1")

    def test_rejects_non_json_body(self):
        with unittest.mock.patch.object(ota.urllib.request, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_challenge("192.168.4.1")

    def test_http_error_surfaces_status_and_detail(self):
        err = urllib.error.HTTPError("http://x/api/ota/challenge", 500, "boom",
                                      hdrs=None, fp=io.BytesIO(b"internal error"))
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.get_challenge("192.168.4.1")
        self.assertEqual(ctx.exception.status, 500)
        self.assertIn("internal error", ctx.exception.detail)


class PushImageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        self.tmp.write(b"\xe9\x00\x00\x00fake-image-bytes")
        self.tmp.close()
        self.addCleanup(os.unlink, self.tmp.name)

    def _mock_challenge_then(self, post_response, post_side_effect=None):
        challenge_body = json.dumps({"nonce": "11" * 16}).encode()

        calls = {"n": 0}

        def fake_urlopen(req, timeout=None):
            calls["n"] += 1
            if calls["n"] == 1:
                return _fake_response(challenge_body)
            if post_side_effect is not None:
                raise post_side_effect
            return post_response

        return fake_urlopen, calls

    def test_push_esp_image_sends_mac_header_and_parses_result(self):
        ok_body = json.dumps({"ok": True, "bytes": 17, "partition": "ota_0",
                               "version": "1.2.3"}).encode()
        challenge_body = json.dumps({"nonce": "11" * 16}).encode()

        calls = {"n": 0}
        captured_req = {}

        def wrapper(req, timeout=None):
            calls["n"] += 1
            if calls["n"] == 1:
                return _fake_response(challenge_body)
            captured_req["req"] = req
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=wrapper):
            result = ota.push_esp_image("kiln.local", self.tmp.name, "hunter2")

        self.assertTrue(result.ok)
        self.assertEqual(result.body["version"], "1.2.3")
        req = captured_req["req"]
        self.assertEqual(req.full_url, "http://kiln.local/api/ota/esp")
        mac_header = req.headers.get("X-ota-mac") or req.headers.get("X-Ota-mac")
        self.assertIsNotNone(mac_header)
        self.assertEqual(len(mac_header), 64)
        # Must match a manually-derived MAC over the same nonce/context.
        expected = ota.derive_mac("hunter2", bytes.fromhex("11" * 16), "esp").hex()
        self.assertEqual(mac_header, expected)

    def test_push_pico_image_reports_202_relay_started(self):
        accepted_body = json.dumps({"ok": True, "status": "relay_started", "bytes": 17,
                                     "crc32": "0xdeadbeef"}).encode()
        fake_urlopen, _ = self._mock_challenge_then(_fake_response(accepted_body, status=202))
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            result = ota.push_pico_image("kiln.local", self.tmp.name, "hunter2")
        self.assertTrue(result.ok)
        self.assertEqual(result.status_code, 202)
        self.assertEqual(result.body["status"], "relay_started")

    def test_push_refuses_missing_file(self):
        with self.assertRaises(ota.OtaHttpError):
            ota.push_esp_image("kiln.local", "/no/such/file.bin", "hunter2")

    def test_push_refuses_empty_file(self):
        empty = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        empty.close()
        self.addCleanup(os.unlink, empty.name)
        with self.assertRaises(ota.OtaHttpError):
            ota.push_esp_image("kiln.local", empty.name, "hunter2")

    def test_push_surfaces_409_interlock_refusal_as_plain_text(self):
        """ota_http.c sends 409 Conflict as PLAIN TEXT (httpd_resp_send),
        e.g. "zone 2 is at 340 C" -- not JSON. This must come back to the
        caller as that specific text, not swallowed or misparsed."""
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"zone 2 is at 340 C"))
        fake_urlopen, _ = self._mock_challenge_then(None, post_side_effect=err)
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.push_esp_image("kiln.local", self.tmp.name, "hunter2")
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("zone 2 is at 340 C", ctx.exception.detail)

    def test_push_surfaces_403_wrong_password(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp", 403, "Forbidden", hdrs=None,
            fp=io.BytesIO(b"wrong password"))
        fake_urlopen, _ = self._mock_challenge_then(None, post_side_effect=err)
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.push_pico_image("kiln.local", self.tmp.name, "hunter2")
        self.assertEqual(ctx.exception.status, 403)


class GetPicoStatusTest(unittest.TestCase):
    def test_parses_status_json(self):
        body = json.dumps({"phase": "sending", "percent": 42,
                            "last_error": ""}).encode()
        with unittest.mock.patch.object(ota.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            status = ota.get_pico_status("kiln.local")
        self.assertEqual(status["phase"], "sending")
        self.assertEqual(status["percent"], 42)

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_pico_status("192.0.2.1")


class GetEspStatusTest(unittest.TestCase):
    def test_parses_status_json_with_no_last_update(self):
        body = json.dumps({"phase": "idle", "percent": 0, "last_update": None}).encode()
        with unittest.mock.patch.object(ota.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            status = ota.get_esp_status("kiln.local")
        self.assertEqual(status["phase"], "idle")
        self.assertIsNone(status["last_update"])

    def test_parses_status_json_with_last_update(self):
        body = json.dumps({
            "phase": "done",
            "percent": 100,
            "last_update": {
                "processor": "esp",
                "version_before": "1.0.0",
                "version_after": "1.1.0",
                "success": True,
                "reason": "ok",
                "uptime_s": 1234,
            },
        }).encode()
        with unittest.mock.patch.object(ota.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            status = ota.get_esp_status("kiln.local")
        self.assertEqual(status["phase"], "done")
        self.assertEqual(status["last_update"]["version_after"], "1.1.0")
        self.assertTrue(status["last_update"]["success"])

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_esp_status("192.0.2.1")

    def test_rejects_non_json_body(self):
        with unittest.mock.patch.object(ota.urllib.request, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_esp_status("kiln.local")


class RollbackEspTest(unittest.TestCase):
    def _mock_challenge_then(self, post_response=None, post_side_effect=None):
        challenge_body = json.dumps({"nonce": "22" * 16}).encode()
        calls = {"n": 0}

        def fake_urlopen(req, timeout=None):
            calls["n"] += 1
            if calls["n"] == 1:
                return _fake_response(challenge_body)
            if post_side_effect is not None:
                raise post_side_effect
            return post_response

        return fake_urlopen, calls

    def test_sends_rollback_context_mac_and_empty_body(self):
        ok_body = json.dumps({"ok": True, "status": "rebooting",
                               "version_before": "1.2.3"}).encode()
        challenge_body = json.dumps({"nonce": "22" * 16}).encode()

        calls = {"n": 0}
        captured_req = {}

        def wrapper(req, timeout=None):
            calls["n"] += 1
            if calls["n"] == 1:
                return _fake_response(challenge_body)
            captured_req["req"] = req
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=wrapper):
            result = ota.rollback_esp("kiln.local", "hunter2")

        self.assertTrue(result["ok"])
        self.assertEqual(result["version_before"], "1.2.3")
        req = captured_req["req"]
        self.assertEqual(req.full_url, "http://kiln.local/api/ota/esp/rollback")
        self.assertEqual(req.data, b"")
        mac_header = req.headers.get("X-ota-mac") or req.headers.get("X-Ota-mac")
        self.assertIsNotNone(mac_header)
        self.assertEqual(len(mac_header), 64)
        # Must be signed over the "esp-rollback" context, NOT "esp" -- a
        # plain update MAC must not double as rollback authorization.
        expected = ota.derive_mac("hunter2", bytes.fromhex("22" * 16), "esp-rollback").hex()
        self.assertEqual(mac_header, expected)
        not_esp_context = ota.derive_mac("hunter2", bytes.fromhex("22" * 16), "esp").hex()
        self.assertNotEqual(mac_header, not_esp_context)

    def test_surfaces_409_no_previous_image_refusal(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/rollback", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"no previous valid image to roll back to"))
        fake_urlopen, _ = self._mock_challenge_then(post_side_effect=err)
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.rollback_esp("kiln.local", "hunter2")
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("no previous valid image to roll back to", ctx.exception.detail)

    def test_surfaces_403_wrong_password(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/rollback", 403, "Forbidden", hdrs=None,
            fp=io.BytesIO(b"wrong password"))
        fake_urlopen, _ = self._mock_challenge_then(post_side_effect=err)
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.rollback_esp("kiln.local", "hunter2")
        self.assertEqual(ctx.exception.status, 403)

    def test_surfaces_409_interlock_refusal(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/rollback", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"zone 2 is at 340 C"))
        fake_urlopen, _ = self._mock_challenge_then(post_side_effect=err)
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.rollback_esp("kiln.local", "hunter2")
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("zone 2 is at 340 C", ctx.exception.detail)

    def test_rejects_non_json_response(self):
        fake_urlopen, _ = self._mock_challenge_then(_fake_response(b"not json"))
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(ota.OtaHttpError):
                ota.rollback_esp("kiln.local", "hunter2")

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.rollback_esp("192.0.2.1", "hunter2")


class DeriveMacRollbackContextTest(unittest.TestCase):
    def test_matches_manual_double_hmac(self):
        nonce = bytes(range(16))
        expected_key = hmac.new(b"hunter2", b"kilnctl-ota-v1", hashlib.sha256).digest()
        expected = hmac.new(expected_key, nonce + b"esp-rollback", hashlib.sha256).digest()
        self.assertEqual(ota.derive_mac("hunter2", nonce, "esp-rollback"), expected)

    def test_three_contexts_all_diverge(self):
        nonce = bytes(range(16))
        mac_esp = ota.derive_mac("pw", nonce, "esp")
        mac_pico = ota.derive_mac("pw", nonce, "pico")
        mac_rollback = ota.derive_mac("pw", nonce, "esp-rollback")
        self.assertEqual(len({mac_esp, mac_pico, mac_rollback}), 3)


class DeriveMacRecoveryContextTest(unittest.TestCase):
    def test_matches_manual_double_hmac(self):
        nonce = bytes(range(16))
        expected_key = hmac.new(b"hunter2", b"kilnctl-ota-v1", hashlib.sha256).digest()
        expected = hmac.new(expected_key, nonce + b"recovery", hashlib.sha256).digest()
        self.assertEqual(ota.derive_mac("hunter2", nonce, "recovery"), expected)

    def test_all_four_contexts_diverge(self):
        # The whole point of a per-action HMAC context is that a MAC
        # computed for one action must never be accepted for another --
        # this is what would silently break if recovery_exit's handler ever
        # reused (say) the "esp" context string instead of its own
        # "recovery" one. len(set(...)) == 4 fails immediately if any two
        # collide.
        nonce = bytes(range(16))
        mac_esp = ota.derive_mac("pw", nonce, "esp")
        mac_pico = ota.derive_mac("pw", nonce, "pico")
        mac_rollback = ota.derive_mac("pw", nonce, "esp-rollback")
        mac_recovery = ota.derive_mac("pw", nonce, "recovery")
        self.assertEqual(len({mac_esp, mac_pico, mac_rollback, mac_recovery}), 4)

    def test_recovery_mac_not_accepted_as_rollback_mac(self):
        """The specific negative case ROADMAP/CLAUDE.md's 'prove it can
        fail' rule asks for: a MAC signed over 'recovery' must not equal one
        signed over 'esp-rollback' for the same nonce/password -- if
        recovery_exit's context string were ever accidentally set to
        'esp-rollback' (reusing OTA_HTTP_CONTEXT_ESP_ROLLBACK instead of its
        own context), this assertion is what would catch it."""
        nonce = bytes(range(16))
        mac_recovery = ota.derive_mac("pw", nonce, "recovery")
        mac_rollback = ota.derive_mac("pw", nonce, "esp-rollback")
        self.assertNotEqual(mac_recovery, mac_rollback)

    def test_rejects_old_three_context_only_error_message(self):
        with self.assertRaises(ValueError):
            ota.derive_mac("pw", bytes(16), "not-a-real-context")


class RecoveryExitEspTest(unittest.TestCase):
    def _mock_challenge_then(self, post_response=None, post_side_effect=None):
        challenge_body = json.dumps({"nonce": "33" * 16}).encode()
        calls = {"n": 0}

        def fake_urlopen(req, timeout=None):
            calls["n"] += 1
            if calls["n"] == 1:
                return _fake_response(challenge_body)
            if post_side_effect is not None:
                raise post_side_effect
            return post_response

        return fake_urlopen, calls

    def test_sends_recovery_context_mac_and_empty_body(self):
        ok_body = json.dumps({"ok": True, "status": "rebooting"}).encode()
        challenge_body = json.dumps({"nonce": "33" * 16}).encode()

        calls = {"n": 0}
        captured_req = {}

        def wrapper(req, timeout=None):
            calls["n"] += 1
            if calls["n"] == 1:
                return _fake_response(challenge_body)
            captured_req["req"] = req
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=wrapper):
            result = ota.recovery_exit_esp("kiln.local", "hunter2")

        self.assertTrue(result["ok"])
        req = captured_req["req"]
        self.assertEqual(req.full_url, "http://kiln.local/api/ota/esp/recovery_exit")
        self.assertEqual(req.data, b"")
        mac_header = req.headers.get("X-ota-mac") or req.headers.get("X-Ota-mac")
        self.assertIsNotNone(mac_header)
        self.assertEqual(len(mac_header), 64)
        # Must be signed over the "recovery" context, NOT "esp"/"esp-rollback"/
        # "pico" -- a MAC for any of those other actions must not double as
        # authorization for recovery-mode exit.
        expected = ota.derive_mac("hunter2", bytes.fromhex("33" * 16), "recovery").hex()
        self.assertEqual(mac_header, expected)
        not_rollback_context = ota.derive_mac("hunter2", bytes.fromhex("33" * 16), "esp-rollback").hex()
        self.assertNotEqual(mac_header, not_rollback_context)

    def test_surfaces_403_not_in_recovery_mode(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/recovery_exit", 403, "Forbidden", hdrs=None,
            fp=io.BytesIO(b"board is not in recovery mode"))
        fake_urlopen, _ = self._mock_challenge_then(post_side_effect=err)
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.recovery_exit_esp("kiln.local", "hunter2")
        self.assertEqual(ctx.exception.status, 403)
        self.assertIn("board is not in recovery mode", ctx.exception.detail)

    def test_surfaces_403_wrong_password(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/recovery_exit", 403, "Forbidden", hdrs=None,
            fp=io.BytesIO(b"wrong password"))
        fake_urlopen, _ = self._mock_challenge_then(post_side_effect=err)
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.recovery_exit_esp("kiln.local", "hunter2")
        self.assertEqual(ctx.exception.status, 403)

    def test_rejects_non_json_response(self):
        fake_urlopen, _ = self._mock_challenge_then(_fake_response(b"not json"))
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertRaises(ota.OtaHttpError):
                ota.recovery_exit_esp("kiln.local", "hunter2")

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.recovery_exit_esp("192.0.2.1", "hunter2")


class PushImageLoggingTest(unittest.TestCase):
    """TODO.md: 'Every call logged with the image's SHA-256, and refusals
    logged too' / 'The password is never written to the log'."""

    SECRET = "correct-horse-battery-staple"

    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        self.image_bytes = b"\xe9\x00\x00\x00fake-image-bytes-for-logging"
        self.tmp.write(self.image_bytes)
        self.tmp.close()
        self.addCleanup(os.unlink, self.tmp.name)
        self.expected_sha256 = hashlib.sha256(self.image_bytes).hexdigest()

    def _mock_challenge_then(self, post_response=None, post_side_effect=None):
        challenge_body = json.dumps({"nonce": "44" * 16}).encode()
        calls = {"n": 0}

        def fake_urlopen(req, timeout=None):
            calls["n"] += 1
            if calls["n"] == 1:
                return _fake_response(challenge_body)
            if post_side_effect is not None:
                raise post_side_effect
            return post_response

        return fake_urlopen

    def test_successful_push_logs_sha256_and_never_the_password(self):
        ok_body = json.dumps({"ok": True, "bytes": 29, "partition": "ota_0",
                               "version": "1.2.3"}).encode()
        fake_urlopen = self._mock_challenge_then(_fake_response(ok_body))
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertLogs(ota.log, level="INFO") as cm:
                ota.push_esp_image("kiln.local", self.tmp.name, self.SECRET)
        all_output = "\n".join(cm.output)
        self.assertIn(self.expected_sha256, all_output)
        self.assertNotIn(self.SECRET, all_output)

    def test_refusal_is_logged_with_sha256_and_never_the_password(self):
        """A refused push must leave a record too -- not just successes."""
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"zone 2 is at 340 C"))
        fake_urlopen = self._mock_challenge_then(post_side_effect=err)
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=fake_urlopen):
            with self.assertLogs(ota.log, level="WARNING") as cm:
                with self.assertRaises(ota.OtaHttpError):
                    ota.push_esp_image("kiln.local", self.tmp.name, self.SECRET)
        all_output = "\n".join(cm.output)
        self.assertIn(self.expected_sha256, all_output)
        self.assertIn("zone 2 is at 340 C", all_output)
        self.assertNotIn(self.SECRET, all_output)


if __name__ == "__main__":
    unittest.main()
