#!/usr/bin/env python3
"""Unit tests for kilnctrl.nvs_keys_http_client -- GET /api/nvs/keys's
client, all against MOCKED urllib responses. No real socket and no live
board.

Run with: python -m pytest tools/PcTools/tests/test_nvs_keys_http_client.py -q
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

from kilnctrl import nvs_keys_http_client as nk  # noqa: E402


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
    "partition": "nvs",
    "namespace": "nvs.net80211",
    "keys": [
        {"key": "sta.ssid", "type": "str"},
        {"key": "opmode", "type": "u8"},
    ],
}).encode("utf-8")


class RequestShapeTest(unittest.TestCase):
    def test_gets_the_right_path_and_query(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(_SAMPLE_BODY)

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            result = nk.get_nvs_keys("10.0.0.5", "nvs", "nvs.net80211")
        self.assertEqual(sent[0].get_method(), "GET")
        self.assertIn("/api/nvs/keys?", sent[0].full_url)
        self.assertIn("partition=nvs", sent[0].full_url)
        self.assertIn("namespace=nvs.net80211", sent[0].full_url)
        self.assertEqual(len(result["keys"]), 2)
        self.assertEqual(result["keys"][0]["key"], "sta.ssid")

    def test_empty_namespace_reads_as_empty_list(self):
        body = json.dumps({"partition": "nvs", "namespace": "nvs.net80211", "keys": []}).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(body)):
            result = nk.get_nvs_keys("10.0.0.5", "nvs", "nvs.net80211")
        self.assertEqual(result["keys"], [])


class ForbiddenNamespaceTest(unittest.TestCase):
    def test_kiln_auth_is_refused_locally_with_no_request(self):
        def fake_urlopen(req, timeout=None):
            raise AssertionError("no HTTP request should be made for the kiln_auth namespace")

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(nk.NvsKeysHttpError):
                nk.get_nvs_keys("10.0.0.5", "cfg", "kiln_auth")


class ErrorShapeTest(unittest.TestCase):
    def test_non_json_body_raises(self):
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(b"not json")):
            with self.assertRaises(nk.NvsKeysHttpError):
                nk.get_nvs_keys("10.0.0.5", "nvs", "some_ns")

    def test_missing_keys_list_raises(self):
        body = json.dumps({"nope": True}).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(body)):
            with self.assertRaises(nk.NvsKeysHttpError):
                nk.get_nvs_keys("10.0.0.5", "nvs", "some_ns")

    def test_http_error_is_wrapped(self):
        def fake_urlopen(req, timeout=None):
            raise urllib.error.HTTPError(req.full_url, 403, "forbidden", None, io.BytesIO(b"err"))

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(nk.NvsKeysHttpError) as ctx:
                nk.get_nvs_keys("10.0.0.5", "nvs", "some_ns")
        self.assertEqual(ctx.exception.status, 403)

    def test_unreachable_host_is_wrapped(self):
        def fake_urlopen(req, timeout=None):
            raise urllib.error.URLError("no route to host")

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            with self.assertRaises(nk.NvsKeysHttpError):
                nk.get_nvs_keys("10.0.0.5", "nvs", "some_ns")


if __name__ == "__main__":
    unittest.main()
