#!/usr/bin/env python3
"""Unit tests for mcp_server_ota.recovery_enter() and
ota_http_client.recovery_boot_esp() (POST /api/ota/esp/recovery_boot).
Mocked HTTP only; never touches a board.

Run with: python -m pytest tools/PcTools/tests/test_recovery_enter_mcp.py -q
"""
from __future__ import annotations

import io
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota as mo  # noqa: E402
from kilnctrl import ota_http_client as oc  # noqa: E402


class _Resp(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class RecoveryEnterTool(unittest.TestCase):
    def test_refuses_without_confirm_and_never_calls_board(self):
        for bad in (False, 1, "yes", None):
            with unittest.mock.patch.object(mo.ota_http, "recovery_boot_esp") as m:
                out = mo.recovery_enter(host="10.0.0.5", confirm=bad)
            self.assertTrue(out.startswith("error: refused"), out)
            m.assert_not_called()

    def test_success(self):
        with unittest.mock.patch.object(
                mo.ota_http, "recovery_boot_esp",
                return_value={"ok": True, "status": "rebooting", "target": "recovery",
                              "version_before": "1.2.3"}) as m:
            out = mo.recovery_enter(host="10.0.0.5", confirm=True)
        self.assertTrue(out.startswith("ok"), out)
        self.assertIn("1.2.3", out)
        m.assert_called_once_with("10.0.0.5")

    def test_board_refusal_surfaces_as_error_string(self):
        err = mo.ota_http.OtaHttpError("refused", 409, "a relay is on")
        with unittest.mock.patch.object(mo.ota_http, "recovery_boot_esp", side_effect=err):
            out = mo.recovery_enter(host="10.0.0.5", confirm=True)
        self.assertTrue(out.startswith("error"), out)
        self.assertIn("409", out)


class RecoveryBootClient(unittest.TestCase):
    def test_posts_empty_body_to_route(self):
        seen = {}

        def fake_urlopen(req, timeout=None):
            seen["url"] = req.full_url
            seen["method"] = req.get_method()
            seen["data"] = req.data
            return _Resp(b'{"ok":true,"status":"rebooting","target":"recovery","version_before":"x"}')

        with unittest.mock.patch.object(oc.http_auth, "urlopen", side_effect=fake_urlopen):
            body = oc.recovery_boot_esp("10.0.0.5")
        self.assertTrue(body["ok"])
        self.assertTrue(seen["url"].endswith("/api/ota/esp/recovery_boot"))
        self.assertEqual(seen["method"], "POST")
        self.assertEqual(seen["data"], b"")

    def test_http_409_raises_with_detail(self):
        def boom(req, timeout=None):
            raise urllib.error.HTTPError(req.full_url, 409, "Conflict", {}, io.BytesIO(b"a relay is on"))

        with unittest.mock.patch.object(oc.http_auth, "urlopen", side_effect=boom):
            with self.assertRaises(oc.OtaHttpError) as cm:
                oc.recovery_boot_esp("10.0.0.5")
        self.assertEqual(cm.exception.status, 409)


if __name__ == "__main__":
    unittest.main()
