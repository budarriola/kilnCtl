#!/usr/bin/env python3
"""Unit tests for kilnctrl.adaptive_tune_http_client -- request
construction and response parsing for GET /api/adaptive_tune and
POST /api/adaptive_tune/enable, all against MOCKED urllib responses. No
real socket and no live board is used or required.

Run with: python -m unittest discover -s tools/PcTools/tests
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

from kilnctrl import adaptive_tune_http_client as at  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


_ONE_ZONE_BODY = {
    "zones": [
        {
            "zone": 0, "enabled": True, "observation_count": 5,
            "observations_lifetime": 12, "has_applied": True,
            "prior_k_dc": 1.5, "applied_k_dc": 1.62, "delta_pct": 8.0,
            "last_profile_id": 3, "last_applied_unix_s": 1234567,
            "refusal": "",
            "joint_observations": 9, "coupled_attempted": True,
            "coupled_applied": False, "coupled_cells_changed": 0,
            "coupled_refusal": "ill-conditioned",
            "ki_verdict": 5, "ki_correction_pct": -12.5,
            "ki_applied": False, "ki_refusal": "cumulative bound reached",
        },
    ]
}


class GetStatusTest(unittest.TestCase):
    def test_parses_full_zone_row(self):
        body = json.dumps(_ONE_ZONE_BODY).encode()
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            zones = at.get_status("192.168.4.1")
        self.assertEqual(len(zones), 1)
        z = zones[0]
        self.assertEqual(z.zone, 0)
        self.assertTrue(z.enabled)
        self.assertEqual(z.observation_count, 5)
        self.assertEqual(z.observations_lifetime, 12)
        self.assertTrue(z.has_applied)
        self.assertAlmostEqual(z.prior_k_dc, 1.5)
        self.assertAlmostEqual(z.applied_k_dc, 1.62)
        self.assertAlmostEqual(z.delta_pct, 8.0)
        self.assertEqual(z.last_profile_id, 3)
        self.assertEqual(z.last_applied_unix_s, 1234567)
        self.assertEqual(z.refusal, "")
        self.assertEqual(z.joint_observations, 9)
        self.assertTrue(z.coupled_attempted)
        self.assertFalse(z.coupled_applied)
        self.assertEqual(z.coupled_cells_changed, 0)
        self.assertEqual(z.coupled_refusal, "ill-conditioned")
        self.assertEqual(z.ki_verdict, 5)
        self.assertEqual(z.ki_verdict_name, "limit_cycle")
        self.assertAlmostEqual(z.ki_correction_pct, -12.5)
        self.assertFalse(z.ki_applied)
        self.assertEqual(z.ki_refusal, "cumulative bound reached")

    def test_multiple_zones_all_decoded(self):
        body = json.dumps({"zones": [
            {"zone": 0, "enabled": False},
            {"zone": 1, "enabled": True},
            {"zone": 2, "enabled": False},
        ]}).encode()
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            zones = at.get_status("192.168.4.1")
        self.assertEqual([z.zone for z in zones], [0, 1, 2])
        self.assertEqual([z.enabled for z in zones], [False, True, False])

    def test_missing_fields_default_rather_than_raise(self):
        # A field this client expects but the live firmware's handler
        # hasn't grown yet (or has renamed) must not blow up the whole
        # decode -- see the module docstring on why this stays tolerant
        # while the surface is under active development by another agent.
        body = json.dumps({"zones": [{"zone": 4}]}).encode()
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            zones = at.get_status("192.168.4.1")
        self.assertEqual(len(zones), 1)
        self.assertEqual(zones[0].zone, 4)
        self.assertIsNone(zones[0].enabled)  # absent key must not read as False
        self.assertEqual(zones[0].ki_verdict_name, "insufficient")

    def test_rejects_non_json_body(self):
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(at.AdaptiveTuneHttpError):
                at.get_status("192.168.4.1")

    def test_rejects_missing_zones_list(self):
        body = json.dumps({"nope": True}).encode()
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            with self.assertRaises(at.AdaptiveTuneHttpError):
                at.get_status("192.168.4.1")

    def test_http_error_surfaces_status_and_detail(self):
        err = urllib.error.HTTPError("http://x/api/adaptive_tune", 500, "boom",
                                      hdrs=None, fp=io.BytesIO(b"internal error"))
        with unittest.mock.patch.object(at.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(at.AdaptiveTuneHttpError) as ctx:
                at.get_status("192.168.4.1")
        self.assertEqual(ctx.exception.status, 500)
        self.assertIn("internal error", ctx.exception.detail)


class SetEnabledTest(unittest.TestCase):
    def test_sends_zone_and_enabled_form_fields(self):
        captured = {}

        def fake_urlopen(req, timeout=None):
            captured["data"] = req.data
            captured["headers"] = dict(req.header_items())
            return _fake_response(json.dumps({"ok": True}).encode())

        with unittest.mock.patch.object(at.urllib.request, "urlopen", side_effect=fake_urlopen):
            result = at.set_enabled("192.168.4.1", 2, True)
        self.assertEqual(result, {"ok": True})
        self.assertEqual(captured["data"], b"zone=2&enabled=1")
        self.assertEqual(captured["headers"].get("Content-type"),
                          "application/x-www-form-urlencoded")

    def test_disabled_sends_enabled_zero(self):
        captured = {}

        def fake_urlopen(req, timeout=None):
            captured["data"] = req.data
            return _fake_response(json.dumps({"ok": True}).encode())

        with unittest.mock.patch.object(at.urllib.request, "urlopen", side_effect=fake_urlopen):
            at.set_enabled("192.168.4.1", 0, False)
        self.assertEqual(captured["data"], b"zone=0&enabled=0")

    def test_warning_body_passed_through(self):
        body = json.dumps({"ok": True, "warning": "applied live, save failed"}).encode()
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            result = at.set_enabled("192.168.4.1", 1, True)
        self.assertTrue(result["ok"])
        self.assertEqual(result["warning"], "applied live, save failed")

    def test_http_error_raises_with_status(self):
        err = urllib.error.HTTPError("http://x/api/adaptive_tune/enable", 400, "bad",
                                      hdrs=None, fp=io.BytesIO(b"zone out of range"))
        with unittest.mock.patch.object(at.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(at.AdaptiveTuneHttpError) as ctx:
                at.set_enabled("192.168.4.1", 9, True)
        self.assertEqual(ctx.exception.status, 400)
        self.assertIn("zone out of range", ctx.exception.detail)

    def test_rejects_non_json_response(self):
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(b"not json")):
            with self.assertRaises(at.AdaptiveTuneHttpError):
                at.set_enabled("192.168.4.1", 0, True)


class RevertTest(unittest.TestCase):
    def test_sends_zone_only_form_field(self):
        captured = {}

        def fake_urlopen(req, timeout=None):
            captured["data"] = req.data
            return _fake_response(json.dumps({"ok": True}).encode())

        with unittest.mock.patch.object(at.urllib.request, "urlopen", side_effect=fake_urlopen):
            result = at.revert("192.168.4.1", 3)
        self.assertEqual(result, {"ok": True})
        self.assertEqual(captured["data"], b"zone=3")

    def test_failure_reason_passed_through(self):
        body = json.dumps({"ok": False, "reason": "nothing to revert"}).encode()
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            result = at.revert("192.168.4.1", 1)
        self.assertFalse(result["ok"])
        self.assertEqual(result["reason"], "nothing to revert")

    def test_http_error_raises(self):
        err = urllib.error.HTTPError("http://x/api/adaptive_tune/revert", 400, "bad",
                                      hdrs=None, fp=io.BytesIO(b"zone out of range"))
        with unittest.mock.patch.object(at.urllib.request, "urlopen", side_effect=err):
            with self.assertRaises(at.AdaptiveTuneHttpError) as ctx:
                at.revert("192.168.4.1", 9)
        self.assertEqual(ctx.exception.status, 400)


class RevertAvailableFieldTest(unittest.TestCase):
    def test_none_when_absent(self):
        body = json.dumps({"zones": [{"zone": 0}]}).encode()
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            zones = at.get_status("192.168.4.1")
        self.assertIsNone(zones[0].revert_available)  # absent is not a confirmation

    def test_true_when_present(self):
        body = json.dumps({"zones": [{"zone": 0, "revert_available": True}]}).encode()
        with unittest.mock.patch.object(at.urllib.request, "urlopen",
                                         return_value=_fake_response(body)):
            zones = at.get_status("192.168.4.1")
        self.assertTrue(zones[0].revert_available)


if __name__ == "__main__":
    unittest.main()
