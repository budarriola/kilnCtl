#!/usr/bin/env python3
"""Unit tests for kilnctrl.kiln_configs_quarantine_http_client -- POST
/api/kiln_configs/quarantine_clear and its non-mutating status probe, all
against MOCKED urllib responses. No real socket and no live board.

Run with: python -m pytest tools/PcTools/tests/test_kiln_configs_quarantine_http_client.py -q
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

from kilnctrl import kiln_configs_quarantine_http_client as qc  # noqa: E402


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
    def test_clear_posts_confirm_1(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"ok":true,"discarded_reason":"wrong size for any known version"}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            result = qc.post_quarantine_clear("host")
        self.assertEqual(sent[0].get_method(), "POST")
        self.assertEqual(sent[0].data, b"confirm=1")
        self.assertTrue(sent[0].full_url.endswith("/api/kiln_configs/quarantine_clear"))
        self.assertEqual(result["ok"], True)

    def test_status_probe_sends_no_confirm_field(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            raise urllib.error.HTTPError("u", 409, "Conflict", {}, io.BytesIO(b"not quarantined"))

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            quarantined, detail = qc.get_quarantine_status("host")
        self.assertEqual(sent[0].data, b"")
        self.assertFalse(quarantined)
        self.assertIn("not quarantined", detail)

    def test_list_is_a_plain_get(self):
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return _fake_response(b'{"active_id":1,"configs":[],"max_count":8}')

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            result = qc.get_kiln_configs_list("host")
        self.assertEqual(sent[0].get_method(), "GET")
        self.assertEqual(result["max_count"], 8)


class QuarantineStatusProbeTest(unittest.TestCase):
    """The route's ordering (quarantine check before the confirm gate) is
    what lets a POST with no confirm field double as a status read -- 409
    means healthy, 400 means quarantined and confirm=1 would be required.
    Any other status is something this client does not understand."""

    def test_400_means_quarantined(self):
        err = urllib.error.HTTPError(
            "u", 400, "Bad Request", {},
            io.BytesIO(b"confirm=1 required to discard the quarantined store"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            quarantined, detail = qc.get_quarantine_status("host")
        self.assertTrue(quarantined)
        self.assertIn("confirm=1 required", detail)

    def test_409_means_not_quarantined(self):
        err = urllib.error.HTTPError(
            "u", 409, "Conflict", {},
            io.BytesIO(b"kiln config store is not quarantined -- nothing to clear"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            quarantined, detail = qc.get_quarantine_status("host")
        self.assertFalse(quarantined)

    def test_unexpected_status_raises(self):
        err = urllib.error.HTTPError("u", 500, "Internal Server Error", {}, io.BytesIO(b"boom"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(qc.KilnConfigsQuarantineHttpError):
                qc.get_quarantine_status("host")


class ClearFailureTest(unittest.TestCase):
    def test_unreachable_host_is_refused(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                unittest.mock.Mock(side_effect=urllib.error.URLError("no route to host"))):
            with self.assertRaises(qc.KilnConfigsQuarantineHttpError):
                qc.post_quarantine_clear("host")

    def test_non_json_body_on_clear_is_refused(self):
        with unittest.mock.patch(
                "urllib.request.urlopen",
                lambda req, timeout=None: _fake_response(b"<html>nope</html>")):
            with self.assertRaises(qc.KilnConfigsQuarantineHttpError):
                qc.post_quarantine_clear("host")

    def test_409_racing_clear_is_raised_with_status(self):
        err = urllib.error.HTTPError(
            "u", 409, "Conflict", {},
            io.BytesIO(b"kiln config store is not quarantined -- nothing to clear"))
        with unittest.mock.patch("urllib.request.urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(qc.KilnConfigsQuarantineHttpError) as ctx:
                qc.post_quarantine_clear("host")
        self.assertEqual(ctx.exception.status, 409)


if __name__ == "__main__":
    unittest.main()
