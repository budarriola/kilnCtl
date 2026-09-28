#!/usr/bin/env python3
"""Unit tests for kilnctrl.backup_export_http_client -- GET
/api/backup/export's client, all against MOCKED urllib responses. No real
socket and no live board.

Run with: python -m pytest tools/PcTools/tests/test_backup_export_http_client.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import backup_export_http_client as bx  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


_GOOD_DOC = {
    "kind": "kilnctl_backup",
    "version": 3,
    "profiles": [{"name": "bisque"}, {"name": "glaze"}],
    "zones": [{"id": 0}],
    "kiln_configs": [],
}


class GetExportTest(unittest.TestCase):
    def test_parses_good_document(self):
        body = json.dumps(_GOOD_DOC).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", lambda req, timeout=None: _fake_response(body)):
            raw_text, parsed = bx.get_export("host")
        self.assertEqual(parsed["version"], 3)
        self.assertEqual(len(parsed["profiles"]), 2)
        self.assertEqual(json.loads(raw_text), _GOOD_DOC)

    def test_get_uses_get_method(self):
        sent = []
        body = json.dumps(_GOOD_DOC).encode("utf-8")

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(body)

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            bx.get_export("host")
        self.assertEqual(sent[0].get_method(), "GET")
        self.assertTrue(sent[0].full_url.endswith("/api/backup/export"))

    def test_missing_kind_is_refused(self):
        body = json.dumps({"version": 1}).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", lambda req, timeout=None: _fake_response(body)):
            with self.assertRaises(bx.BackupExportHttpError):
                bx.get_export("host")

    def test_non_json_body_is_refused(self):
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(b"<html>nope</html>")):
            with self.assertRaises(bx.BackupExportHttpError):
                bx.get_export("host")

    def test_oversized_body_is_refused(self):
        huge = b"x" * (bx.BACKUP_EXPORT_MAX_BYTES + 10)
        with unittest.mock.patch("urllib.request.urlopen", lambda req, timeout=None: _fake_response(huge)):
            with self.assertRaises(bx.BackupExportHttpError):
                bx.get_export("host")

    def test_403_admin_tier_refusal_is_raised_with_status(self):
        err = urllib.error.HTTPError("u", 403, "Forbidden", {}, io.BytesIO(b"forbidden"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(bx.BackupExportHttpError) as ctx:
                bx.get_export("host")
        self.assertEqual(ctx.exception.status, 403)

    def test_unreachable_host_is_refused(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                unittest.mock.Mock(side_effect=urllib.error.URLError("no route to host"))):
            with self.assertRaises(bx.BackupExportHttpError):
                bx.get_export("host")


class ScanForSensitiveFieldsTest(unittest.TestCase):
    def test_clean_document_is_false(self):
        self.assertFalse(bx.scan_for_sensitive_fields(json.dumps(_GOOD_DOC)))

    def test_wifi_marker_is_true(self):
        self.assertTrue(bx.scan_for_sensitive_fields('{"wifi_ssid":"home"}'))

    def test_password_marker_is_case_insensitive(self):
        self.assertTrue(bx.scan_for_sensitive_fields('{"Password":"hunter2"}'))

    def test_psk_marker_is_true(self):
        self.assertTrue(bx.scan_for_sensitive_fields('{"psk":"abc"}'))


if __name__ == "__main__":
    unittest.main()
