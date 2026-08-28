#!/usr/bin/env python3
"""Unit tests for kilnctrl.web_ui_client.WebUiClient, against MOCKED
urllib responses -- same pattern as test_zones_http_client.py. No real
socket and no live board.

Run with: python -m pytest tools/PcTools/tests/test_web_ui_client.py -q
"""
from __future__ import annotations

import io
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import web_ui_client as wc  # noqa: E402

_SAMPLE_HTML = """
<html><body>
<button id="runBtn" type="button">Start</button>
<button id="stopBtn" type="button" hidden>Stop</button>
<div id="chartMeta" style="display:none">meta</div>
<div id="statusLine">All zones idle</div>
</body></html>
"""


def _fake_response(body: bytes):
    resp = io.BytesIO(body)

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class GotoTest(unittest.TestCase):
    def test_stores_html(self):
        client = wc.WebUiClient("http://kiln.local")
        with unittest.mock.patch.object(wc.urllib.request, "urlopen",
                                         return_value=_fake_response(_SAMPLE_HTML.encode())):
            html = client.goto("/")
        self.assertIn("runBtn", html)
        self.assertEqual(client.current_html, html)
        self.assertEqual(client.current_path, "/")

    def test_unreachable_host_raises(self):
        client = wc.WebUiClient("http://192.0.2.1")
        with unittest.mock.patch.object(wc.urllib.request, "urlopen",
                                         side_effect=urllib.error.URLError("no route")):
            with self.assertRaises(wc.WebUiError):
                client.goto("/")


class FindElementTest(unittest.TestCase):
    def setUp(self):
        self.client = wc.WebUiClient("http://kiln.local")
        self.client.current_html = _SAMPLE_HTML

    def test_finds_visible_element_with_text(self):
        el = self.client.find_element("runBtn")
        self.assertEqual(el["tag"], "button")
        self.assertEqual(el["text"], "Start")
        self.assertFalse(el["hidden"])

    def test_hidden_attribute_detected(self):
        el = self.client.find_element("stopBtn")
        self.assertTrue(el["hidden"])

    def test_display_none_style_detected(self):
        el = self.client.find_element("chartMeta")
        self.assertTrue(el["hidden"])

    def test_missing_id_returns_none(self):
        self.assertIsNone(self.client.find_element("doesNotExist"))

    def test_div_text(self):
        el = self.client.find_element("statusLine")
        self.assertEqual(el["text"], "All zones idle")


class ClickTest(unittest.TestCase):
    def setUp(self):
        self.client = wc.WebUiClient("http://kiln.local")

    def test_unmapped_target_raises_with_hint(self):
        with self.assertRaises(wc.WebUiError) as ctx:
            self.client.click("notARealButton")
        self.assertIn("no click mapping for 'notARealButton'", str(ctx.exception))
        self.assertIn("_WEB_CLICK_ENDPOINTS", str(ctx.exception))

    def test_mapped_target_posts_to_real_endpoint(self):
        captured = {}

        def fake_urlopen(req, timeout=None):
            captured["url"] = req.full_url
            captured["method"] = req.get_method()
            captured["data"] = req.data
            return _fake_response(b'{"ok":true}')

        with unittest.mock.patch.object(wc.urllib.request, "urlopen", side_effect=fake_urlopen):
            body = self.client.click("runBtn", body="id=0")
        self.assertEqual(captured["url"], "http://kiln.local/api/profile_exec/start")
        self.assertEqual(captured["method"], "POST")
        self.assertEqual(captured["data"], b"id=0")
        self.assertEqual(body, '{"ok":true}')

    def test_stop_button_maps_to_stop_endpoint(self):
        def fake_urlopen(req, timeout=None):
            self.assertEqual(req.full_url, "http://kiln.local/api/profile_exec/stop")
            return _fake_response(b"ok")

        with unittest.mock.patch.object(wc.urllib.request, "urlopen", side_effect=fake_urlopen):
            self.client.click("stopBtn")

    def test_http_error_surfaces_detail(self):
        err = urllib.error.HTTPError(
            "http://kiln.local/api/profile_exec/start", 400, "Bad Request", hdrs=None,
            fp=io.BytesIO(b"no profile selected"))
        with unittest.mock.patch.object(wc.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(wc.WebUiError) as ctx:
                self.client.click("runBtn", body="id=")
        self.assertIn("no profile selected", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
