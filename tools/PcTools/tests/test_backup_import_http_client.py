#!/usr/bin/env python3
"""Unit tests for kilnctrl.backup_import_http_client -- POST
/api/backup/import's client, all against MOCKED urllib responses. No real
socket and no live board.

Run with: python -m pytest tools/PcTools/tests/test_backup_import_http_client.py -q
"""
from __future__ import annotations

import io
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import backup_import_http_client as bi  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class RequestShapeTest(unittest.TestCase):
    def test_default_merge_sends_no_mode_header(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            status, body = bi.post_import("host", "{}")
        self.assertEqual(status, 200)
        self.assertEqual(sent[0].get_method(), "POST")
        self.assertTrue(sent[0].full_url.endswith("/api/backup/import"))
        self.assertIsNone(sent[0].get_header("X-kiln-config-mode"))

    def test_mirror_mode_sends_header(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            bi.post_import("host", "{}", mode="mirror")
        # urllib.request.Request header lookup is case-insensitive via get_header().
        self.assertEqual(sent[0].get_header("X-kiln-config-mode"), "mirror")

    def test_dry_run_sends_header(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b"plan line 1\nplan line 2")

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            status, body = bi.post_import("host", "{}", dry_run=True)
        self.assertEqual(sent[0].get_header("X-kiln-config-dry-run"), "1")
        self.assertIn("plan line", body)

    def test_ack_delete_count_sent_only_when_not_none(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            bi.post_import("host", "{}", ack_delete_count=3)
        self.assertEqual(sent[0].get_header("X-kiln-config-ack-delete"), "3")

    def test_ack_no_safety_sends_header(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            bi.post_import("host", "{}", ack_no_safety=True)
        self.assertEqual(sent[0].get_header("X-ota-ack-no-safety"), "1")


class RefusalStatusTupleTest(unittest.TestCase):
    """post_import() returns (status, body) for every non-2xx -- it never
    raises for an ordinary HTTP-level refusal, only for a transport
    failure -- same convention as kiln_configs_apply_http_client."""

    def test_400_validation_is_returned_not_raised(self):
        err = urllib.error.HTTPError("u", 400, "Bad Request", {}, io.BytesIO(b"bad profile entry 2"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            status, body = bi.post_import("host", "{}")
        self.assertEqual(status, 400)
        self.assertIn("bad profile entry", body)

    def test_500_partial_write_is_returned_not_raised(self):
        err = urllib.error.HTTPError("u", 500, "Internal Server Error", {}, io.BytesIO(b"partial write"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            status, body = bi.post_import("host", "{}")
        self.assertEqual(status, 500)

    def test_unreachable_host_raises(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                unittest.mock.Mock(side_effect=urllib.error.URLError("no route to host"))):
            with self.assertRaises(bi.BackupImportHttpError):
                bi.post_import("host", "{}")


class ClassifyRefusalTest(unittest.TestCase):
    def test_428_is_interlock_needs_ack(self):
        self.assertEqual(bi.classify_refusal(428, "safety link down"), bi.REFUSAL_INTERLOCK_NEEDS_ACK)

    def test_409_async_busy_marker(self):
        self.assertEqual(bi.classify_refusal(409, bi.ASYNC_BUSY_MARKER), bi.REFUSAL_ASYNC_BUSY)

    def test_409_mode_gate_marker(self):
        self.assertEqual(
            bi.classify_refusal(409, "refused -- a firing or autotune run is active"),
            bi.REFUSAL_MODE_GATE)

    def test_409_other_is_plain_interlock(self):
        self.assertEqual(bi.classify_refusal(409, "heater is on"), bi.REFUSAL_INTERLOCK)

    def test_400_is_validation(self):
        self.assertEqual(bi.classify_refusal(400, "bad json"), bi.REFUSAL_VALIDATION)

    def test_500_is_partial_write(self):
        self.assertEqual(bi.classify_refusal(500, "partial"), bi.REFUSAL_PARTIAL_WRITE)

    def test_unknown_status_is_other(self):
        self.assertEqual(bi.classify_refusal(503, "?"), bi.REFUSAL_OTHER)


if __name__ == "__main__":
    unittest.main()
