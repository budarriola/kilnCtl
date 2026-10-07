#!/usr/bin/env python3
"""Unit tests for kilnctrl.ota_http_client -- request construction and
response parsing, all against MOCKED urllib responses. No real socket and no
live board is used or required.

Every route in this module used to also require an AP-password HMAC
challenge/response handshake on top of ROUTE_TIER_ADMIN; that scheme was
retired 2026-09-29 (WEB_AUTH_PLAN.md item 2b, owner decision "Retire; open
when login off"). These tests confirm the CURRENT contract: every write
route posts through ``http_auth.urlopen()`` (mocked here the same as a plain
``urllib.request.urlopen`` call, since these tests never exercise
http_auth's own login/retry logic) with no nonce, no MAC, no X-Ota-Mac
header -- and that no credential parameter is accepted any more.

These tests do NOT exercise ota_http.c on real hardware -- they only check
that this PC-side client builds the request the firmware documents
(ota_http.h/.c) and parses the firmware's documented response shapes
correctly. Live-board verification is still outstanding -- see ROADMAP.md
M8 / TODO.md 9.6.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import hashlib
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


class PushImageTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        self.tmp.write(b"\xe9\x00\x00\x00fake-image-bytes")
        self.tmp.close()
        self.addCleanup(os.unlink, self.tmp.name)

    def test_push_esp_image_sends_no_credential_and_parses_result(self):
        ok_body = json.dumps({"ok": True, "bytes": 17, "partition": "ota_0",
                               "version": "1.2.3"}).encode()
        captured_req = {}

        def wrapper(req, timeout=None):
            captured_req["req"] = req
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            result = ota.push_esp_image("kiln.local", self.tmp.name)

        self.assertTrue(result.ok)
        self.assertEqual(result.body["version"], "1.2.3")
        req = captured_req["req"]
        self.assertEqual(req.full_url, "http://kiln.local/api/ota/esp")
        self.assertIsNone(req.headers.get("X-ota-mac") or req.headers.get("X-Ota-mac"))
        self.assertIsNone(req.headers.get("X-ota-nonce") or req.headers.get("X-Ota-nonce"))

    def test_push_establishes_admin_session_before_big_post(self):
        """A bodyless ADMIN-tier GET (/api/ota/interlock) goes through
        http_auth.urlopen() BEFORE the large POST, for the esp and pico
        routes alike, so an unauthenticated 401 is handled on a tiny request
        (http_auth_refusal_should_close() would otherwise reset the upload)."""
        ok_body = json.dumps({"ok": True}).encode()
        for push, endpoint in ((ota.push_esp_image, "/api/ota/esp"), (ota.push_pico_image, "/api/ota/pico")):
            seen = []

            def wrapper(req, timeout=None, _seen=seen):
                _seen.append((req.get_method(), req.full_url, req.data))
                return _fake_response(ok_body)

            with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
                push("kiln.local", self.tmp.name)
            self.assertEqual([(m, u) for m, u, _ in seen],
                             [("GET", "http://kiln.local/api/ota/interlock"),
                              ("POST", "http://kiln.local" + endpoint)])
            self.assertIsNone(seen[0][2])
            self.assertTrue(seen[1][2])

    def test_failed_session_probe_does_not_block_the_push(self):
        ok_body = json.dumps({"ok": True}).encode()
        calls = []

        def wrapper(req, timeout=None):
            calls.append(req.get_method())
            if req.get_method() == "GET":
                raise urllib.error.URLError("probe down")
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            self.assertTrue(ota.push_esp_image("kiln.local", self.tmp.name).ok)
        self.assertEqual(calls, ["GET", "POST"])

    def test_probe_401_403_aborts_before_upload(self):
        for code in (401, 403):
            calls = []

            def wrapper(req, timeout=None, _c=calls, _code=code):
                _c.append(req.get_method())
                raise urllib.error.HTTPError(req.full_url, _code, "no", hdrs=None, fp=io.BytesIO(b"denied"))

            with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
                with self.assertRaises(ota.OtaHttpError) as cm:
                    ota.push_esp_image("kiln.local", self.tmp.name)
            self.assertEqual(cm.exception.status, code)
            self.assertEqual(calls, ["GET"])

    def test_missing_file_makes_no_request(self):
        with unittest.mock.patch.object(ota.http_auth, "urlopen") as m:
            with self.assertRaises(ota.OtaHttpError):
                ota.push_esp_image("kiln.local", "/no/such/file.bin")
        m.assert_not_called()

    def test_push_pico_image_reports_202_relay_started(self):
        accepted_body = json.dumps({"ok": True, "status": "relay_started", "bytes": 17,
                                     "crc32": "0xdeadbeef"}).encode()
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
                                         side_effect=lambda *a, **k: _fake_response(accepted_body, status=202)):
            result = ota.push_pico_image("kiln.local", self.tmp.name)
        self.assertTrue(result.ok)
        self.assertEqual(result.status_code, 202)
        self.assertEqual(result.body["status"], "relay_started")

    def test_push_refuses_missing_file(self):
        with self.assertRaises(ota.OtaHttpError):
            ota.push_esp_image("kiln.local", "/no/such/file.bin")

    def test_push_refuses_empty_file(self):
        empty = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        empty.close()
        self.addCleanup(os.unlink, empty.name)
        with self.assertRaises(ota.OtaHttpError):
            ota.push_esp_image("kiln.local", empty.name)

    def test_push_surfaces_409_interlock_refusal_as_plain_text(self):
        """ota_http.c sends 409 Conflict as PLAIN TEXT (httpd_resp_send),
        e.g. "zone 2 is at 340 C" -- not JSON. This must come back to the
        caller as that specific text, not swallowed or misparsed."""
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"zone 2 is at 340 C"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.push_esp_image("kiln.local", self.tmp.name)
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("zone 2 is at 340 C", ctx.exception.detail)

    def test_push_surfaces_401_no_session(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp", 401, "Unauthorized", hdrs=None,
            fp=io.BytesIO(b"admin session required"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.push_pico_image("kiln.local", self.tmp.name)
        self.assertEqual(ctx.exception.status, 401)

    def test_push_pico_image_raises_typed_error_on_protocol_version_mismatch(self):
        """TODO.md 9.4: ota_http_pico.c refuses with 409 JSON, not the plain
        text every other 4xx/5xx on this surface sends -- this must come
        back as OtaPicoProtocolVersionMismatch, carrying both version
        numbers, not a generic OtaHttpError."""
        detail = json.dumps({"ok": False, "error": "protocol_version_mismatch",
                              "image_protocol_version": 11, "esp_protocol_version": 16})
        err = urllib.error.HTTPError(
            "http://x/api/ota/pico", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(detail.encode()))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaPicoProtocolVersionMismatch) as ctx:
                ota.push_pico_image("kiln.local", self.tmp.name)
        self.assertEqual(ctx.exception.status, 409)
        self.assertEqual(ctx.exception.image_protocol_version, 11)
        self.assertEqual(ctx.exception.esp_protocol_version, 16)

    def test_push_pico_image_default_omits_force_version_header(self):
        accepted_body = json.dumps({"ok": True, "status": "relay_started", "bytes": 17,
                                     "crc32": "0xdeadbeef"}).encode()
        captured_req = {}

        def wrapper(req, timeout=None):
            captured_req["req"] = req
            return _fake_response(accepted_body, status=202)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            ota.push_pico_image("kiln.local", self.tmp.name)
        req = captured_req["req"]
        self.assertIsNone(req.headers.get("X-ota-force-version") or req.headers.get("X-Ota-force-version"))

    def test_push_pico_image_force_version_sets_header(self):
        accepted_body = json.dumps({"ok": True, "status": "relay_started", "bytes": 17,
                                     "crc32": "0xdeadbeef"}).encode()
        captured_req = {}

        def wrapper(req, timeout=None):
            captured_req["req"] = req
            return _fake_response(accepted_body, status=202)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            result = ota.push_pico_image("kiln.local", self.tmp.name, force_version=True)
        self.assertTrue(result.ok)
        req = captured_req["req"]
        header = req.headers.get("X-ota-force-version") or req.headers.get("X-Ota-force-version")
        self.assertEqual(header, "1")


class GetPicoStatusTest(unittest.TestCase):
    def test_parses_status_json(self):
        body = json.dumps({"phase": "sending", "percent": 42,
                            "last_error": ""}).encode()
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
                                         return_value=_fake_response(body)):
            status = ota.get_pico_status("kiln.local")
        self.assertEqual(status["phase"], "sending")
        self.assertEqual(status["percent"], 42)

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_pico_status("192.0.2.1")


class GetEspStatusTest(unittest.TestCase):
    def test_parses_status_json_with_no_last_update(self):
        body = json.dumps({"phase": "idle", "percent": 0, "last_update": None}).encode()
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
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
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
                                         return_value=_fake_response(body)):
            status = ota.get_esp_status("kiln.local")
        self.assertEqual(status["phase"], "done")
        self.assertEqual(status["last_update"]["version_after"], "1.1.0")
        self.assertTrue(status["last_update"]["success"])

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_esp_status("192.0.2.1")

    def test_rejects_non_json_body(self):
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_esp_status("kiln.local")


class RollbackEspTest(unittest.TestCase):
    def test_sends_empty_body_no_credential(self):
        ok_body = json.dumps({"ok": True, "status": "rebooting",
                               "version_before": "1.2.3"}).encode()
        captured_req = {}

        def wrapper(req, timeout=None):
            captured_req["req"] = req
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            result = ota.rollback_esp("kiln.local")

        self.assertTrue(result["ok"])
        self.assertEqual(result["version_before"], "1.2.3")
        req = captured_req["req"]
        self.assertEqual(req.full_url, "http://kiln.local/api/ota/esp/rollback")
        self.assertEqual(req.data, b"")
        self.assertIsNone(req.headers.get("X-ota-mac") or req.headers.get("X-Ota-mac"))

    def test_surfaces_409_no_previous_image_refusal(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/rollback", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"no previous valid image to roll back to"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.rollback_esp("kiln.local")
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("no previous valid image to roll back to", ctx.exception.detail)

    def test_surfaces_401_no_session(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/rollback", 401, "Unauthorized", hdrs=None,
            fp=io.BytesIO(b"admin session required"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.rollback_esp("kiln.local")
        self.assertEqual(ctx.exception.status, 401)

    def test_surfaces_409_interlock_refusal(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/rollback", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"zone 2 is at 340 C"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.rollback_esp("kiln.local")
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("zone 2 is at 340 C", ctx.exception.detail)

    def test_rejects_non_json_response(self):
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(ota.OtaHttpError):
                ota.rollback_esp("kiln.local")

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.rollback_esp("192.0.2.1")


class RecoveryExitEspTest(unittest.TestCase):
    def test_sends_empty_body_no_credential(self):
        ok_body = json.dumps({"ok": True, "status": "rebooting"}).encode()
        captured_req = {}

        def wrapper(req, timeout=None):
            captured_req["req"] = req
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            result = ota.recovery_exit_esp("kiln.local")

        self.assertTrue(result["ok"])
        req = captured_req["req"]
        self.assertEqual(req.full_url, "http://kiln.local/api/ota/esp/recovery_exit")
        self.assertEqual(req.data, b"")
        self.assertIsNone(req.headers.get("X-ota-mac") or req.headers.get("X-Ota-mac"))

    def test_surfaces_403_not_in_recovery_mode(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/recovery_exit", 403, "Forbidden", hdrs=None,
            fp=io.BytesIO(b"board is not in recovery mode"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.recovery_exit_esp("kiln.local")
        self.assertEqual(ctx.exception.status, 403)
        self.assertIn("board is not in recovery mode", ctx.exception.detail)

    def test_surfaces_401_no_session(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/recovery_exit", 401, "Unauthorized", hdrs=None,
            fp=io.BytesIO(b"admin session required"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.recovery_exit_esp("kiln.local")
        self.assertEqual(ctx.exception.status, 401)

    def test_rejects_non_json_response(self):
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(ota.OtaHttpError):
                ota.recovery_exit_esp("kiln.local")

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.recovery_exit_esp("192.0.2.1")


class BootGuardResetEspTest(unittest.TestCase):
    """docs/audits/boot_guard_post_flash_recovery_footgun_2026-09-08.md's
    tool-driven trigger -- POST /api/ota/esp/boot_guard_reset."""

    def test_sends_empty_body_no_credential(self):
        ok_body = json.dumps({"ok": True, "boot_count": 0}).encode()
        captured_req = {}

        def wrapper(req, timeout=None):
            captured_req["req"] = req
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            result = ota.boot_guard_reset_esp("kiln.local")

        self.assertTrue(result["ok"])
        self.assertEqual(result["boot_count"], 0)
        req = captured_req["req"]
        self.assertEqual(req.full_url, "http://kiln.local/api/ota/esp/boot_guard_reset")
        self.assertEqual(req.data, b"")
        self.assertIsNone(req.headers.get("X-ota-mac") or req.headers.get("X-Ota-mac"))

    def test_reports_ok_false_without_raising(self):
        """A lying-write on the board (verified=false) is a normal 200
        response, not an HTTP error -- the caller must check `ok` in the
        body, and this client must not swallow or misreport it."""
        unverified_body = json.dumps({"ok": False, "boot_count": 2}).encode()
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
                                         return_value=_fake_response(unverified_body)):
            result = ota.boot_guard_reset_esp("kiln.local")
        self.assertFalse(result["ok"])
        self.assertEqual(result["boot_count"], 2)

    def test_surfaces_401_no_session(self):
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp/boot_guard_reset", 401, "Unauthorized", hdrs=None,
            fp=io.BytesIO(b"admin session required"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.boot_guard_reset_esp("kiln.local")
        self.assertEqual(ctx.exception.status, 401)

    def test_rejects_non_json_response(self):
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(ota.OtaHttpError):
                ota.boot_guard_reset_esp("kiln.local")

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.boot_guard_reset_esp("192.0.2.1")


class GetBootGuardStatusTest(unittest.TestCase):
    """GET /api/boot_guard -- admin-session-authenticated diagnostics."""

    def test_parses_count_and_recovery_mode(self):
        body = json.dumps({"boot_count": 3, "recovery_mode": True}).encode()
        with unittest.mock.patch.object(ota.http_auth, "urlopen", return_value=_fake_response(body)):
            result = ota.get_boot_guard_status("kiln.local")
        self.assertEqual(result["boot_count"], 3)
        self.assertTrue(result["recovery_mode"])

    def test_single_call_no_challenge(self):
        """No nonce/HMAC dance any more -- exactly one urlopen call."""
        body = json.dumps({"boot_count": 0, "recovery_mode": False}).encode()
        calls = {"n": 0}

        def wrapper(req, timeout=None):
            calls["n"] += 1
            return _fake_response(body)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            ota.get_boot_guard_status("kiln.local")
        self.assertEqual(calls["n"], 1)

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_boot_guard_status("192.0.2.1")


class PushImageLoggingTest(unittest.TestCase):
    """TODO.md: 'Every call logged with the image's SHA-256, and refusals
    logged too'."""

    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        self.image_bytes = b"\xe9\x00\x00\x00fake-image-bytes-for-logging"
        self.tmp.write(self.image_bytes)
        self.tmp.close()
        self.addCleanup(os.unlink, self.tmp.name)
        self.expected_sha256 = hashlib.sha256(self.image_bytes).hexdigest()

    def test_successful_push_logs_sha256(self):
        ok_body = json.dumps({"ok": True, "bytes": 29, "partition": "ota_0",
                               "version": "1.2.3"}).encode()
        with unittest.mock.patch.object(ota.http_auth, "urlopen",
                                         side_effect=lambda *a, **k: _fake_response(ok_body)):
            with self.assertLogs(ota.log, level="INFO") as cm:
                ota.push_esp_image("kiln.local", self.tmp.name)
        all_output = "\n".join(cm.output)
        self.assertIn(self.expected_sha256, all_output)

    def test_refusal_is_logged_with_sha256(self):
        """A refused push must leave a record too -- not just successes."""
        err = urllib.error.HTTPError(
            "http://x/api/ota/esp", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"zone 2 is at 340 C"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertLogs(ota.log, level="WARNING") as cm:
                with self.assertRaises(ota.OtaHttpError):
                    ota.push_esp_image("kiln.local", self.tmp.name)
        all_output = "\n".join(cm.output)
        self.assertIn(self.expected_sha256, all_output)
        self.assertIn("zone 2 is at 340 C", all_output)


class GetInterlockTest(unittest.TestCase):
    """GET /api/ota/interlock -- reads through http_auth.urlopen()."""

    def test_parses_ok_true(self):
        body = json.dumps({"ok": True}).encode()
        with unittest.mock.patch.object(ota.http_auth, "urlopen", return_value=_fake_response(body)):
            result = ota.get_interlock("kiln.local")
        self.assertTrue(result["ok"])

    def test_parses_ok_false_with_reason(self):
        body = json.dumps({"ok": False, "reason": "an update is already in progress"}).encode()
        with unittest.mock.patch.object(ota.http_auth, "urlopen", return_value=_fake_response(body)):
            result = ota.get_interlock("kiln.local")
        self.assertFalse(result["ok"])
        self.assertEqual(result["reason"], "an update is already in progress")

    def test_single_call(self):
        body = json.dumps({"ok": True}).encode()
        calls = {"n": 0}

        def wrapper(req, timeout=None):
            calls["n"] += 1
            return _fake_response(body)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            ota.get_interlock("kiln.local")
        self.assertEqual(calls["n"], 1)

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_interlock("192.0.2.1")

    def test_bad_json_raises(self):
        with unittest.mock.patch.object(ota.http_auth, "urlopen", return_value=_fake_response(b"not json")):
            with self.assertRaises(ota.OtaHttpError):
                ota.get_interlock("kiln.local")


class PushImageUnauthenticatedTest(unittest.TestCase):
    """OT-E09: no admin session cookie at all -- plain unauthenticated POST,
    bypassing http_auth.urlopen() on purpose (see this function's own
    docstring for why)."""

    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        self.tmp.write(b"\xe9\x00\x00\x00fake-image-bytes")
        self.tmp.close()
        self.addCleanup(os.unlink, self.tmp.name)

    def test_no_mac_header_sent(self):
        ok_body = json.dumps({"ok": True}).encode()
        captured = {}

        def wrapper(req, timeout=None):
            captured["req"] = req
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=wrapper):
            result = ota.push_esp_image_unauthenticated("kiln.local", self.tmp.name)
        self.assertTrue(result.ok)
        self.assertIsNone(captured["req"].get_header("X-ota-mac"))
        self.assertIsNone(captured["req"].get_header("X-ota-nonce"))
        self.assertIsNone(captured["req"].get_header("Cookie"))

    def test_refused_returns_not_ok(self):
        err = urllib.error.HTTPError("http://x/api/ota/esp", 401, "Unauthorized", hdrs=None,
                                      fp=io.BytesIO(b"missing credential"))
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=err):
            result = ota.push_esp_image_unauthenticated("kiln.local", self.tmp.name)
        self.assertFalse(result.ok)
        self.assertEqual(result.status_code, 401)

    def test_missing_file_raises(self):
        with self.assertRaises(ota.OtaHttpError):
            ota.push_esp_image_unauthenticated("kiln.local", "/no/such/file.bin")


class PushImageWithSessionTest(unittest.TestCase):
    """OT-E10: a kiln_sid web-auth session cookie, admin vs. user tier."""

    def setUp(self):
        self.tmp = tempfile.NamedTemporaryFile(suffix=".bin", delete=False)
        self.tmp.write(b"\xe9\x00\x00\x00fake-image-bytes")
        self.tmp.close()
        self.addCleanup(os.unlink, self.tmp.name)

    def test_sends_cookie_header(self):
        ok_body = json.dumps({"ok": True}).encode()
        captured = {}

        def wrapper(req, timeout=None):
            captured["req"] = req
            return _fake_response(ok_body)

        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=wrapper):
            result = ota.push_esp_image_with_session("kiln.local", self.tmp.name, "abc123")
        self.assertTrue(result.ok)
        self.assertEqual(captured["req"].get_header("Cookie"), "kiln_sid=abc123")

    def test_user_tier_session_refused(self):
        err = urllib.error.HTTPError("http://x/api/ota/esp", 403, "Forbidden", hdrs=None,
                                      fp=io.BytesIO(b"admin required"))
        with unittest.mock.patch.object(ota.urllib.request, "urlopen", side_effect=err):
            result = ota.push_esp_image_with_session("kiln.local", self.tmp.name, "usersession")
        self.assertFalse(result.ok)
        self.assertEqual(result.status_code, 403)

    def test_missing_file_raises(self):
        with self.assertRaises(ota.OtaHttpError):
            ota.push_esp_image_with_session("kiln.local", "/no/such/file.bin", "abc")


class SwResetTest(unittest.TestCase):
    def test_sends_empty_body_no_credential(self):
        ok_text = b"ok -- rebooting this controller now"
        captured_req = {}

        def wrapper(req, timeout=None):
            captured_req["req"] = req
            return _fake_response(ok_text)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            result = ota.sw_reset("kiln.local")

        self.assertTrue(result["ok"])
        self.assertEqual(result["detail"], ok_text.decode())
        req = captured_req["req"]
        self.assertEqual(req.full_url, "http://kiln.local/api/sw_reset")
        self.assertEqual(req.data, b"")
        self.assertIsNone(req.headers.get("X-ota-mac") or req.headers.get("X-Ota-mac"))

    def test_surfaces_409_interlock_refusal(self):
        err = urllib.error.HTTPError(
            "http://x/api/sw_reset", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"refused: a firing is in progress"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.sw_reset("kiln.local")
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("a firing is in progress", ctx.exception.detail)

    def test_surfaces_401_no_session(self):
        err = urllib.error.HTTPError(
            "http://x/api/sw_reset", 401, "Unauthorized", hdrs=None,
            fp=io.BytesIO(b"admin session required"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.sw_reset("kiln.local")
        self.assertEqual(ctx.exception.status, 401)

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.sw_reset("192.0.2.1")


class FormatCfgfsTest(unittest.TestCase):
    """Unit tests for ota_http_client.format_cfgfs() -- POST
    /api/cfgfs/format_confirm."""

    def test_sends_empty_body_no_credential(self):
        ok_text = b"ok -- cfg partition formatted and mounted"
        captured_req = {}
        calls = {"n": 0}

        def wrapper(req, timeout=None):
            calls["n"] += 1
            captured_req["req"] = req
            return _fake_response(ok_text)

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            result = ota.format_cfgfs("kiln.local")

        self.assertTrue(result["ok"])
        self.assertEqual(result["detail"], ok_text.decode())
        req = captured_req["req"]
        self.assertEqual(req.full_url, "http://kiln.local/api/cfgfs/format_confirm")
        self.assertEqual(req.data, b"")
        self.assertIsNone(req.headers.get("X-ota-mac") or req.headers.get("X-Ota-mac"))
        # Posted exactly once -- never retried on its own behalf.
        self.assertEqual(calls["n"], 1)

    def test_default_url_has_no_override_query(self):
        captured = {}

        def wrapper(req, timeout=None):
            captured["req"] = req
            return _fake_response(b"ok")

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            ota.format_cfgfs("kiln.local")
        self.assertNotIn("force_healthy", captured["req"].full_url)

    def test_force_healthy_adds_override_query(self):
        captured = {}

        def wrapper(req, timeout=None):
            captured["req"] = req
            return _fake_response(b"ok")

        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=wrapper):
            ota.format_cfgfs("kiln.local", force_healthy=True)
        self.assertEqual(captured["req"].full_url, "http://kiln.local/api/cfgfs/format_confirm?force_healthy=1")

    def test_surfaces_409_healthy_refusal(self):
        err = urllib.error.HTTPError(
            "http://x/api/cfgfs/format_confirm", 409, "Conflict", hdrs=None,
            fp=io.BytesIO(b"refused: cfg is mounted and healthy"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.format_cfgfs("kiln.local")
        self.assertEqual(ctx.exception.status, 409)
        self.assertIn("healthy", ctx.exception.detail)

    def test_surfaces_500_format_failed(self):
        err = urllib.error.HTTPError(
            "http://x/api/cfgfs/format_confirm", 500, "Internal Server Error", hdrs=None,
            fp=io.BytesIO(b"format failed: ESP_ERR_INVALID_STATE"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.format_cfgfs("kiln.local")
        self.assertEqual(ctx.exception.status, 500)
        self.assertIn("format failed", ctx.exception.detail)

    def test_surfaces_401_no_session(self):
        err = urllib.error.HTTPError(
            "http://x/api/cfgfs/format_confirm", 401, "Unauthorized", hdrs=None,
            fp=io.BytesIO(b"admin session required"))
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError) as ctx:
                ota.format_cfgfs("kiln.local")
        self.assertEqual(ctx.exception.status, 401)

    def test_unreachable_host_raises(self):
        err = urllib.error.URLError("no route to host")
        with unittest.mock.patch.object(ota.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ota.OtaHttpError):
                ota.format_cfgfs("192.0.2.1")


if __name__ == "__main__":
    unittest.main()
