#!/usr/bin/env python3
"""Unit tests for kilnctrl.safety_cfg_http_client -- POST-body construction,
read-back verification, and the ct_channel_map opt-in, all against MOCKED
urllib responses. No real socket and no live board.

Two of these tests are the negative halves that make the rest mean anything
(the repo's "prove a new check can actually fail" rule):

  * OptInTest.test_backup_section_is_not_written_by_default -- the whole
    reason the backup section exists. If applying a preset quietly included
    an unmeasured ct_channel_map, the safety processor would report itself
    COMMISSIONED on a mapping nobody verified.
  * VerifyTest.test_unset_readback_is_a_mismatch -- the "ACK is not proof"
    contract. A field the board ACKs but reads back UNSET must fail, not
    pass; this is exactly what happened on the live bench when the commit
    was refused while ARMED.

Run with: python -m pytest tools/PcTools/tests/test_safety_cfg_http_client.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock
import urllib.error
import urllib.parse

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import safety_cfg_http_client as sc  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


#: A cut-down version of the real GET body this bench actually returns --
#: same shape, same names/ids/types, with the "value omitted when unset"
#: convention preserved exactly (that omission is load-bearing).
def _sample_get(commissioned: bool = False, reliable: bool = True, **set_values):
    params = [
        {"id": 257, "name": "tc_source", "type": "u8"},
        {"id": 258, "name": "borrowed_zone_index", "type": "u8"},
        {"id": 259, "name": "tc_placement_mode", "type": "u8"},
        {"id": 260, "name": "abs_max_temp_c", "type": "f32"},
        {"id": 261, "name": "tc_type", "type": "u8"},
        {"id": 262, "name": "ct_channel_map[0]", "type": "u8"},
        {"id": 263, "name": "ct_channel_map[1]", "type": "u8"},
        {"id": 264, "name": "ct_channel_map[2]", "type": "u8"},
        {"id": 516, "name": "max_rate_c_per_min", "type": "f32"},
        {"id": 782, "name": "mains_voltage_v", "type": "f32"},
        {"id": 790, "name": "ct_cal[0].calibrated", "type": "bool"},
    ]
    for p in params:
        if p["name"] in set_values:
            p["set"] = True
            p["value"] = set_values[p["name"]]
        else:
            p["set"] = False
    return {
        "link_up": True, "live_config_crc": 1, "cached_config_crc": 1, "stale": False,
        "commissioned": commissioned, "fetched_ms_ago": None,
        "unset_reporting_reliable": reliable, "params": params,
    }


class FormatValueTest(unittest.TestCase):
    def test_u8_is_a_plain_integer_not_a_float_repr(self):
        # safety_cfg_http.c parses u8/u16 with strtol and rejects the pair if
        # anything follows the digits -- "3.0" would be refused outright.
        self.assertEqual(sc.format_value("tc_type", "u8", 3), "3")
        self.assertEqual(sc.format_value("tc_type", "u8", 3.0), "3")

    def test_u8_refuses_a_fractional_value(self):
        with self.assertRaises(sc.SafetyCfgHttpError):
            sc.format_value("tc_type", "u8", 3.5)

    def test_u8_refuses_an_out_of_range_value(self):
        with self.assertRaises(sc.SafetyCfgHttpError):
            sc.format_value("tc_type", "u8", 256)

    def test_bool_is_one_or_zero(self):
        self.assertEqual(sc.format_value("ct_cal[0].calibrated", "bool", True), "1")
        self.assertEqual(sc.format_value("ct_cal[0].calibrated", "bool", False), "0")

    def test_f32_refuses_nan_and_inf(self):
        for bad in (float("nan"), float("inf"), float("-inf")):
            with self.assertRaises(sc.SafetyCfgHttpError):
                sc.format_value("abs_max_temp_c", "f32", bad)

    def test_unknown_wire_type_is_refused(self):
        with self.assertRaises(sc.SafetyCfgHttpError):
            sc.format_value("whatever", "u64", 1)


class BuildPostBodyTest(unittest.TestCase):
    def test_only_the_named_fields_are_sent(self):
        """The core difference from zones_http_client: this endpoint is
        INCREMENTAL, so a field not named must not appear in the body at
        all. Echoing the page back would turn every unset field into an
        explicitly-set one and fake a commissioned board."""
        body = sc.build_post_body(_sample_get(), {"tc_type": 3})
        pairs = urllib.parse.parse_qsl(body)
        self.assertEqual(pairs, [("id", "261"), ("value", "3"), ("commit", "1")])

    def test_id_and_value_tokens_alternate_in_order(self):
        """parse_set_param_body() pairs each 'id' with the NEXT 'value' and
        returns -1 (HTTP 400) on two ids in a row."""
        body = sc.build_post_body(
            _sample_get(), {"tc_type": 3, "max_rate_c_per_min": 0.0, "borrowed_zone_index": 0})
        keys = [k for k, _ in urllib.parse.parse_qsl(body)]
        self.assertEqual(keys, ["id", "value", "id", "value", "id", "value", "commit"])

    def test_unknown_field_name_is_refused_not_dropped(self):
        with self.assertRaises(sc.SafetyCfgUnknownParamError):
            sc.build_post_body(_sample_get(), {"abs_max_temp_f": 176.0})

    def test_commit_can_be_omitted(self):
        body = sc.build_post_body(_sample_get(), {"tc_type": 3}, commit=False)
        self.assertNotIn("commit", body)


class VerifyTest(unittest.TestCase):
    def test_matching_readback_confirms(self):
        after = _sample_get(tc_type=3, abs_max_temp_c=80.0)
        confirmed, mismatches = sc.verify_fields(after, {"tc_type": 3, "abs_max_temp_c": 80.0})
        self.assertEqual(sorted(confirmed), ["abs_max_temp_c", "tc_type"])
        self.assertEqual(mismatches, [])

    def test_unset_readback_is_a_mismatch(self):
        """NEGATIVE TEST. The live bench produced exactly this: the commit
        was refused (relay ARMED), the field stayed unset, and anything that
        called that a success would have been lying."""
        after = _sample_get()  # tc_type not set
        confirmed, mismatches = sc.verify_fields(after, {"tc_type": 3})
        self.assertEqual(confirmed, [])
        self.assertEqual(len(mismatches), 1)
        self.assertIn("UNSET", mismatches[0])

    def test_wrong_value_readback_is_a_mismatch(self):
        after = _sample_get(tc_type=7)
        confirmed, mismatches = sc.verify_fields(after, {"tc_type": 3})
        self.assertEqual(confirmed, [])
        self.assertIn("expected 3", mismatches[0])

    def test_unreliable_unset_reporting_blocks_confirmation(self):
        """A peer too old (or of unknown version) to set the UNSET bit makes
        every 'set' meaningless -- confirming against it would be confirming
        against a value the board itself cannot vouch for."""
        after = _sample_get(reliable=False, tc_type=3)
        confirmed, mismatches = sc.verify_fields(after, {"tc_type": 3})
        self.assertEqual(confirmed, [])
        self.assertIn("unset_reporting_reliable", mismatches[0])

    def test_missing_field_is_a_mismatch(self):
        confirmed, mismatches = sc.verify_fields(_sample_get(), {"not_a_field": 1})
        self.assertEqual(confirmed, [])
        self.assertIn("not present", mismatches[0])


class RequiredFieldsTest(unittest.TestCase):
    def test_reports_exactly_the_unset_required_fields(self):
        current = _sample_get(tc_source=0, tc_placement_mode=0, abs_max_temp_c=80.0,
                              mains_voltage_v=240.0, tc_type=3, max_rate_c_per_min=0.0,
                              borrowed_zone_index=0)
        self.assertEqual(
            sc.unset_required_fields(current),
            ["ct_channel_map[0]", "ct_channel_map[1]", "ct_channel_map[2]"])

    def test_unreliable_reporting_means_everything_is_unset(self):
        current = _sample_get(reliable=False, tc_source=0, tc_type=3)
        self.assertEqual(len(sc.unset_required_fields(current)),
                          len(sc.REQUIRED_FOR_COMMISSIONING))


class ApplyTest(unittest.TestCase):
    def _run_apply(self, fields, post_body, after):
        responses = [
            _fake_response(json.dumps(_sample_get()).encode()),
            _fake_response(json.dumps(post_body).encode()),
            _fake_response(json.dumps(after).encode()),
        ]
        sent = []

        def fake_urlopen(req, timeout=None):
            sent.append(req)
            return responses.pop(0)

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            return sc.apply_safety_fields("host", fields), sent

    def test_confirmed_write_is_ok(self):
        result, sent = self._run_apply({"tc_type": 3}, {"ok": True}, _sample_get(tc_type=3))
        self.assertTrue(result.ok)
        self.assertEqual(result.confirmed, ["tc_type"])
        self.assertFalse(result.commissioned_after)
        # GET, POST, GET again -- the read-back is not optional.
        self.assertEqual([r.get_method() for r in sent], ["GET", "POST", "GET"])

    def test_armed_rejection_is_reported_and_not_confirmed(self):
        """The exact live-bench failure: the board answers ok:false with the
        ARMED reason and the field stays unset."""
        result, _ = self._run_apply(
            {"tc_type": 3},
            {"ok": False, "reason": "commit rejected: relay is ARMED -- config writes are "
                                     "refused while ARMED -- values were staged but NOT written"},
            _sample_get())
        self.assertFalse(result.ok)
        self.assertIn("ARMED", result.post_reason)
        self.assertEqual(result.confirmed, [])

    def test_ok_true_with_a_lying_readback_still_fails(self):
        """An {"ok":true} the read-back contradicts is a failure. This is the
        whole point of verifying independently of the server's self-report."""
        result, _ = self._run_apply({"tc_type": 3}, {"ok": True}, _sample_get())
        self.assertFalse(result.ok)
        self.assertIn("UNSET", result.mismatches[0])

    def test_link_down_refuses_before_posting(self):
        down = _sample_get()
        down["link_up"] = False
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(json.dumps(down).encode())):
            with self.assertRaises(sc.SafetyCfgHttpError):
                sc.apply_safety_fields("host", {"tc_type": 3})


class OptInTest(unittest.TestCase):
    PRESET = {
        "name": "t",
        "safety": {"tc_type": 3},
        "safety_ct_channel_map_backup": {
            "ct_channel_map[0]": 0, "ct_channel_map[1]": 1, "ct_channel_map[2]": 2},
    }

    def _capture_body(self, **kwargs):
        bodies = []

        def fake_urlopen(req, timeout=None):
            if req.get_method() == "POST":
                bodies.append(req.data.decode())
                return _fake_response(b'{"ok":true}')
            return _fake_response(json.dumps(_sample_get(
                tc_type=3, **{f"ct_channel_map[{i}]": i for i in range(3)})).encode())

        with unittest.mock.patch("urllib.request.urlopen", fake_urlopen):
            sc.apply_safety_preset("host", self.PRESET, **kwargs)
        return bodies[0]

    def test_backup_section_is_not_written_by_default(self):
        """NEGATIVE TEST, and the reason this section exists at all.
        Committing all three ct_channel_map ids makes the Pico set the group
        bit, which clears calibration_missing, which lets
        commissioning_gate.c grant heat. That must never happen as a side
        effect of loading a preset."""
        body = self._capture_body()
        self.assertNotIn("262", body)
        self.assertNotIn("263", body)
        self.assertNotIn("264", body)
        self.assertIn("261", body)  # tc_type still written

    def test_backup_section_is_written_on_explicit_opt_in(self):
        body = self._capture_body(use_ct_map_backup=True)
        for wire_id in ("262", "263", "264"):
            self.assertIn(wire_id, body)


class HttpErrorTest(unittest.TestCase):
    def test_non_2xx_detail_is_surfaced_verbatim(self):
        err = urllib.error.HTTPError("u", 400, "Bad Request", {},
                                      io.BytesIO(b"malformed id/value pairs"))
        with unittest.mock.patch("urllib.request.urlopen",
                                  unittest.mock.Mock(side_effect=err)):
            with self.assertRaises(sc.SafetyCfgHttpError) as ctx:
                sc.post_commissioning("host", "id=261&value=3&commit=1")
        self.assertIn("malformed id/value pairs", ctx.exception.detail)

    def test_non_json_get_is_refused(self):
        with unittest.mock.patch("urllib.request.urlopen",
                                  lambda req, timeout=None: _fake_response(b"<html>nope</html>")):
            with self.assertRaises(sc.SafetyCfgHttpError):
                sc.get_commissioning("host")


if __name__ == "__main__":
    unittest.main()
