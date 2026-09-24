#!/usr/bin/env python3
"""Unit tests for kilnctrl.estop_verify_http_client -- POST
/api/estop/verify's client, all against MOCKED urllib responses. No real
socket and no live board.

Run with: python -m pytest tools/PcTools/tests/test_estop_verify_http_client.py -q
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

from kilnctrl import estop_verify_http_client as ev  # noqa: E402


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
        """Patches kilnctrl.http_auth.urlopen -- the ADMIN-session seam this
        client actually calls -- rather than the underlying
        urllib.request.urlopen, so this test exercises the real call this
        client makes rather than bypassing the seam entirely."""
        sent = []

        def fake_urlopen(req, timeout=None, no_relogin=False):
            sent.append(req)
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch.object(ev.http_auth, "urlopen", fake_urlopen):
            result = ev.post_estop_verify("host")
        self.assertEqual(sent[0].get_method(), "POST")
        self.assertEqual(sent[0].data, b"")
        self.assertTrue(sent[0].full_url.endswith("/api/estop/verify"))
        self.assertEqual(result, {"ok": True})


class FailureTest(unittest.TestCase):
    def test_500_failure_is_raised_with_status(self):
        err = urllib.error.HTTPError(
            "u", 500, "Internal Server Error", {},
            io.BytesIO(json.dumps({"ok": False, "error": "ESP_ERR_NVS_NOT_FOUND"}).encode()))
        with unittest.mock.patch.object(ev.http_auth, "urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(ev.EstopVerifyHttpError) as ctx:
                ev.post_estop_verify("host")
        self.assertEqual(ctx.exception.status, 500)
        self.assertIn("ESP_ERR_NVS_NOT_FOUND", ctx.exception.detail)

    def test_unreachable_host_is_refused(self):
        with unittest.mock.patch.object(
                ev.http_auth, "urlopen",
                unittest.mock.Mock(side_effect=urllib.error.URLError("no route to host"))):
            with self.assertRaises(ev.EstopVerifyHttpError):
                ev.post_estop_verify("host")

    def test_non_json_body_is_refused(self):
        with unittest.mock.patch.object(
                ev.http_auth, "urlopen",
                lambda req, timeout=None, no_relogin=False: _fake_response(b"<html>nope</html>")):
            with self.assertRaises(ev.EstopVerifyHttpError):
                ev.post_estop_verify("host")


if __name__ == "__main__":
    unittest.main()
