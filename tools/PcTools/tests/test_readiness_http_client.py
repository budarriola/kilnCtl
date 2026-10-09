#!/usr/bin/env python3
"""Unit tests for kilnctrl.readiness_http_client -- GET /api/readiness's
client, all against MOCKED urllib responses. No real socket and no live
board.

Run with: python -m pytest tools/PcTools/tests/test_readiness_http_client.py -q
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

from kilnctrl import readiness_http_client as rh  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


_SAMPLE_BODY = json.dumps({
    "items": [
        {"key": "network", "label": "Network configured", "status": "ok",
         "detail": "connected", "fix_url": "/wifi"},
        {"key": "estop_verified", "label": "E-stop interlock verified", "status": "not_done",
         "detail": "never verified", "fix_url": "/safety"},
    ]
}).encode("utf-8")


class RequestShapeTest(unittest.TestCase):
    def test_gets_the_right_path(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(_SAMPLE_BODY)

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            result = rh.get_readiness("10.0.0.5")
        self.assertEqual(sent[0].get_method(), "GET")
        self.assertTrue(sent[0].full_url.endswith("/api/readiness"))
        self.assertEqual(len(result["items"]), 2)
        self.assertEqual(result["items"][0]["key"], "network")


class ErrorShapeTest(unittest.TestCase):
    def test_non_json_body_raises(self):
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(b"not json")):
            with self.assertRaises(rh.ReadinessHttpError):
                rh.get_readiness("10.0.0.5")

    def test_missing_items_list_raises(self):
        body = json.dumps({"nope": True}).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(body)):
            with self.assertRaises(rh.ReadinessHttpError):
                rh.get_readiness("10.0.0.5")

    def test_http_error_is_wrapped(self):
        def fake_urlopen(req, timeout=None):
            raise urllib.error.HTTPError(req.full_url, 500, "boom", None, io.BytesIO(b"err"))

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(rh.ReadinessHttpError) as ctx:
                rh.get_readiness("10.0.0.5")
        self.assertEqual(ctx.exception.status, 500)

    def test_unreachable_host_is_wrapped(self):
        def fake_urlopen(req, timeout=None):
            raise urllib.error.URLError("no route to host")

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(rh.ReadinessHttpError):
                rh.get_readiness("10.0.0.5")


if __name__ == "__main__":
    unittest.main()
