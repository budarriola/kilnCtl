#!/usr/bin/env python3
"""Unit tests for kilnctrl.crash_report_clear_http_client -- POST
/api/crash_report/clear's client, all against MOCKED urllib responses. No
real socket and no live board.

Run with: python -m pytest tools/PcTools/tests/test_crash_report_clear_http_client.py -q
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

from kilnctrl import crash_report_clear_http_client as cc  # noqa: E402


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
    def test_posts_with_no_body_fields(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            result = cc.post_crash_report_clear("host")
        self.assertEqual(sent[0].get_method(), "POST")
        self.assertEqual(sent[0].data, b"")
        self.assertTrue(sent[0].full_url.endswith("/api/crash_report/clear"))
        self.assertEqual(result, {"ok": True})


class FailureTest(unittest.TestCase):
    def test_500_erase_failure_is_raised_with_status(self):
        err = urllib.error.HTTPError(
            "u", 500, "Internal Server Error", {},
            io.BytesIO(json.dumps({"ok": False, "error": "ESP_ERR_NOT_FOUND"}).encode()))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(cc.CrashReportClearHttpError) as ctx:
                cc.post_crash_report_clear("host")
        self.assertEqual(ctx.exception.status, 500)
        self.assertIn("ESP_ERR_NOT_FOUND", ctx.exception.detail)

    def test_unreachable_host_is_refused(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                unittest.mock.Mock(side_effect=urllib.error.URLError("no route to host"))):
            with self.assertRaises(cc.CrashReportClearHttpError):
                cc.post_crash_report_clear("host")

    def test_non_json_body_is_refused(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                lambda req, timeout=None: _fake_response(b"<html>nope</html>")):
            with self.assertRaises(cc.CrashReportClearHttpError):
                cc.post_crash_report_clear("host")


if __name__ == "__main__":
    unittest.main()
