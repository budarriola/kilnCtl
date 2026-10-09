#!/usr/bin/env python3
"""Unit tests for kilnctrl.profile_edit_http_client -- the POST /api/profile
form-field builder and its ADMIN-session HTTP wrapper.

`FieldDriftTest` parses the "rule%u_..."/"seg%u_..." string literals
straight out of firmware/KilnFW/App/drivers/http/profiles_edit_http.c's
snprintf() calls and asserts every stem this client's build_post_fields()
emits actually appears in that firmware list -- a stem renamed on one side
without the other would otherwise post a field the parser never reads and
silently do nothing (see the negative test below for proof this catches it).

Run with: python -m pytest tools/PcTools/tests/test_profile_edit_http_client.py -q
"""
from __future__ import annotations

import io
import json
import os
import re
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import profile_edit_http_client as pehc  # noqa: E402
from kilnctrl.devices_profiles import ProfileSegment  # noqa: E402

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
_FIRMWARE_HTTP_C = os.path.join(
    _REPO_ROOT, "firmware", "KilnFW", "App", "drivers", "http", "profiles_edit_http.c"
)


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _firmware_field_stems() -> "set[str]":
    """Every ``rule%u_<name>``/``seg%u_<name>`` stem
    ``profiles_parse_profile_fields()`` builds via
    ``snprintf(key, sizeof(key), "rule%u_..."/"seg%u_...", i)``."""
    with open(_FIRMWARE_HTTP_C, "r", encoding="utf-8") as f:
        text = f.read()
    stems = set(re.findall(r'"((?:rule|seg)%u_[A-Za-z0-9_]+)"', text))
    assert stems, "no rule%u_*/seg%u_* literals found -- firmware source or regex drifted"
    return stems


class BuildPostFieldsTest(unittest.TestCase):
    def test_exact_fields_for_one_segment_and_one_rule(self):
        segments = [ProfileSegment(target_c=123.5, ramp_c_per_hr=60.0, dwell_min=5)]
        rule = pehc.OnOffRule(
            zone_index=2, segment_index=0, enable=True,
            phase_mask=0, direction_mask=0,
            temp_cmp=pehc.ON_OFF_TEMP_CMP_BELOW,
            temp_source=pehc.ON_OFF_TEMP_SOURCE_MEASURED_THIS_ZONE,
            temp_threshold_c=34.0, time_start_s=0, time_stop_s=0, invert=False,
        )
        fields = pehc.build_post_fields(7, "BENCH_HP", 1 << 2, segments, on_off_rules=[rule])
        self.assertEqual(fields, [
            ("id", "7"),
            ("name", "BENCH_HP"),
            ("zone_mask", str(1 << 2)),
            ("seg_count", "1"),
            ("seg0_target", repr(123.5)),
            ("seg0_ramp", repr(60.0)),
            ("seg0_dwell", "5"),
            ("rule0_zone", "2"),
            ("rule0_segment", "0"),
            ("rule0_enable", "1"),
            ("rule0_phase", "0"),
            ("rule0_direction", "0"),
            ("rule0_temp_cmp", str(pehc.ON_OFF_TEMP_CMP_BELOW)),
            ("rule0_temp_source", str(pehc.ON_OFF_TEMP_SOURCE_MEASURED_THIS_ZONE)),
            ("rule0_temp_c", repr(34.0)),
            ("rule0_time_start_s", "0"),
            ("rule0_time_stop_s", "0"),
            ("rule0_invert", "0"),
        ])

    def test_no_rules_omits_every_rule_field(self):
        segments = [ProfileSegment(target_c=100.0, ramp_c_per_hr=60.0, dwell_min=0)]
        fields = pehc.build_post_fields(0, "P", 0b1, segments)
        keys = [k for k, _ in fields]
        self.assertFalse(any(k.startswith("rule") for k in keys))


class FieldDriftTest(unittest.TestCase):
    """Parses the field stems straight out of firmware source rather than a
    second hand-copied list, per this module's own docstring convention."""

    def test_client_emitted_stems_all_exist_in_firmware(self):
        firmware_stems = _firmware_field_stems()
        client_stems = {
            re.sub(r"^(rule|seg)0", r"\g<1>%u", key)
            for key, _ in pehc.build_post_fields(
                0, "n", 0, [ProfileSegment(target_c=1.0, ramp_c_per_hr=1.0, dwell_min=0)],
                on_off_rules=[pehc.OnOffRule(zone_index=0, segment_index=0)],
            )
            if key.startswith("seg0_") or key.startswith("rule0_")
        }
        missing = client_stems - firmware_stems
        self.assertFalse(
            missing,
            f"client emits field stem(s) firmware's parser never reads: {sorted(missing)}",
        )

    def test_negative_a_renamed_stem_is_caught(self):
        """Proves the drift check above actually fails on a real drift,
        rather than passing vacuously. Simulates a client that renamed
        rule%u_zone to rule%u_zoneidx without firmware following -- this must
        NOT be restored by editing firmware; it is a pure in-test simulation
        of the comparison, not a source edit."""
        firmware_stems = _firmware_field_stems()
        simulated_client_stems = {"rule%u_zoneidx", "seg%u_target"}
        missing = simulated_client_stems - firmware_stems
        self.assertIn("rule%u_zoneidx", missing)


class PostProfileTest(unittest.TestCase):
    def test_success_returns_parsed_body(self):
        sent = []

        def fake_urlopen(req, timeout=None, no_relogin=False):
            sent.append(req)
            return _fake_response(json.dumps({"ok": True, "id": 7, "warnings": []}).encode())

        segments = [ProfileSegment(target_c=100.0, ramp_c_per_hr=60.0, dwell_min=0)]
        with unittest.mock.patch.object(pehc.http_auth, "urlopen", fake_urlopen):
            result = pehc.post_profile("10.0.0.5", 7, "BENCH_HP", 0b1, segments)
        self.assertEqual(result, {"ok": True, "id": 7, "warnings": []})
        self.assertEqual(sent[0].get_method(), "POST")
        self.assertTrue(sent[0].full_url.endswith("/api/profile"))

    def test_ok_false_body_raises_profile_edit_http_error(self):
        """NEGATIVE-shaped: a 200 response whose body says {"ok":false} must
        still raise, not be mistaken for success."""
        def fake_urlopen(req, timeout=None, no_relogin=False):
            return _fake_response(json.dumps({"ok": False, "error": "bad on/off rule"}).encode())

        segments = [ProfileSegment(target_c=100.0, ramp_c_per_hr=60.0, dwell_min=0)]
        with unittest.mock.patch.object(pehc.http_auth, "urlopen", fake_urlopen):
            with self.assertRaises(pehc.ProfileEditHttpError) as ctx:
                pehc.post_profile("10.0.0.5", 7, "BENCH_HP", 0b1, segments)
        self.assertIn("bad on/off rule", str(ctx.exception))

    def test_http_error_status_and_detail_surface(self):
        err = urllib.error.HTTPError(
            "u", 400, "Bad Request", {},
            io.BytesIO(json.dumps({"ok": False, "error": "seg_count too large"}).encode()))
        segments = [ProfileSegment(target_c=100.0, ramp_c_per_hr=60.0, dwell_min=0)]
        with unittest.mock.patch.object(pehc.http_auth, "urlopen", unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(pehc.ProfileEditHttpError) as ctx:
                pehc.post_profile("10.0.0.5", 7, "BENCH_HP", 0b1, segments)
        self.assertEqual(ctx.exception.status, 400)
        self.assertIn("seg_count too large", ctx.exception.detail)


if __name__ == "__main__":
    unittest.main()
