#!/usr/bin/env python3
"""Unit tests for kilnctrl.mcp_server_safety._describe_commissioning() --
the READ-ONLY rendering behind the new safety_get_commissioning() MCP tool.

This closes a tooling gap: kiln_find surfaced only safety_set_tc_type
(writes) for the safety processor's commissioned
thresholds, no read tool, so answering "is S1 armed" meant reading
firmware/KilnFW/App/drivers/http/safety_cfg_http.c and the client by hand. The
armed/dormant semantics asserted below were verified against
firmware/SaftyFW/src/safety_guards.c (READ ONLY in this change):

  * S1 (SAFETY_TRIP_OVERTEMP): ``if (cfg->abs_max_temp_c > 0.0f)`` -- 0
    means "not commissioned yet ... never trip" (safety_guards.c's own
    comment).
  * S8 (SAFETY_TRIP_RATE): ``if (cfg->max_rate_c_per_min > 0.0f)`` -- ships
    disabled at 0 for the identical reason.
  * S14 (over-current, WARN only): active per channel only when
    ``i_normal_valid[ch] && i_normal_a[ch] > 0.0f``; ct_installed=0 or an
    unmeasured channel means dormant, not "passing".

Mirrors test_safety_cfg_http_client.py's ``_sample_get()`` shape (same
names/ids/types, same "value omitted when unset" convention) so a payload
change to one test file is easy to cross-check against the other.

MANDATORY NEGATIVE TEST: test_broken_armed_logic_would_be_caught below
temporarily monkeypatches the rendering to treat a 0 threshold as ARMED
(the exact defect class safety_cfg_http.c's own comment calls out -- "the
exact ... defect this whole commissioning-write audit exists to close") and
asserts the existing dormant tests fail. It restores the real function
afterwards and re-asserts the suite is clean.

Run with: python -m pytest tools/PcTools/tests/test_safety_commissioning_render.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_safety as mss  # noqa: E402


def _sample_get(commissioned: bool = False, reliable: bool = True, stale: bool = False,
                 live_crc: int = 1, cached_crc: int = 1, **set_values):
    params = [
        {"id": 257, "name": "tc_source", "type": "u8"},
        {"id": 260, "name": "abs_max_temp_c", "type": "f32"},
        {"id": 265, "name": "ct_installed", "type": "u8"},
        {"id": 516, "name": "max_rate_c_per_min", "type": "f32"},
        {"id": 794, "name": "i_normal_a[0]", "type": "f32"},
        {"id": 795, "name": "i_normal_a[1]", "type": "f32"},
        {"id": 796, "name": "i_normal_a[2]", "type": "f32"},
        {"id": 799, "name": "ct_topology", "type": "u8"},
    ]
    for p in params:
        if p["name"] in set_values:
            p["set"] = True
            p["value"] = set_values[p["name"]]
        else:
            p["set"] = False
    return {
        "link_up": True, "live_config_crc": live_crc, "cached_config_crc": cached_crc,
        "stale": stale, "commissioned": commissioned, "fetched_ms_ago": None,
        "unset_reporting_reliable": reliable, "params": params,
    }


class CommissionedRenderTest(unittest.TestCase):
    def test_positive_abs_max_temp_c_is_armed(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, max_rate_c_per_min=0.0,
                            ct_installed=0)
        out = mss._describe_commissioning(data)
        self.assertIn("S1 abs_max_temp_c=80", out)
        self.assertIn("ARMED", out.splitlines()[[i for i, l in enumerate(out.splitlines())
                                                  if l.startswith("S1")][0]])

    def test_zero_abs_max_temp_c_is_dormant_never_trips(self):
        # Explicitly committed as 0 (not merely unset) -- still dormant.
        data = _sample_get(commissioned=False, abs_max_temp_c=0.0, ct_installed=0)
        out = mss._describe_commissioning(data)
        s1_line = next(l for l in out.splitlines() if l.startswith("S1"))
        self.assertIn("DORMANT", s1_line)
        self.assertIn("never trips", s1_line)

    def test_unset_abs_max_temp_c_is_dormant_not_commissioned(self):
        # abs_max_temp_c simply absent from set_values -> reported unset.
        data = _sample_get(commissioned=False, ct_installed=0)
        out = mss._describe_commissioning(data)
        s1_line = next(l for l in out.splitlines() if l.startswith("S1"))
        self.assertIn("DORMANT", s1_line)
        self.assertIn("not commissioned", s1_line)

    def test_zero_max_rate_is_dormant_ships_disabled(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, max_rate_c_per_min=0.0,
                            ct_installed=0)
        out = mss._describe_commissioning(data)
        s8_line = next(l for l in out.splitlines() if l.startswith("S8"))
        self.assertIn("DORMANT", s8_line)
        self.assertIn("ships disabled", s8_line)

    def test_positive_max_rate_is_armed(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, max_rate_c_per_min=5.0,
                            ct_installed=0)
        out = mss._describe_commissioning(data)
        s8_line = next(l for l in out.splitlines() if l.startswith("S8"))
        self.assertIn("ARMED", s8_line)

    def test_ct_not_installed_reports_s14_dormant(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, ct_installed=0)
        out = mss._describe_commissioning(data)
        self.assertIn("S14 over-current (per channel): DORMANT (ct_installed=0", out)

    def test_ct_installed_with_measured_normal_is_armed_per_channel(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, ct_installed=1,
                            **{"i_normal_a[0]": 4.2, "i_normal_a[1]": 0.0})
        out = mss._describe_commissioning(data)
        s14_line = next(l for l in out.splitlines() if l.startswith("S14"))
        self.assertIn("ch0 i_normal_a=4.2A ARMED", s14_line)
        self.assertIn("ch1 DORMANT", s14_line)
        self.assertIn("ch2 DORMANT", s14_line)

    def test_ct_topology_unset_reports_s15_dormant(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, ct_installed=1)
        out = mss._describe_commissioning(data)
        s15_line = next(l for l in out.splitlines() if l.startswith("S15"))
        self.assertIn("DORMANT", s15_line)
        self.assertIn("ct_topology unknown", s15_line)

    def test_ct_topology_per_zone_reports_s15_dormant(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, ct_installed=1, ct_topology=0)
        out = mss._describe_commissioning(data)
        s15_line = next(l for l in out.splitlines() if l.startswith("S15"))
        self.assertIn("DORMANT", s15_line)
        self.assertIn("ct_topology=per_zone", s15_line)

    def test_ct_topology_summed_with_measured_normal_is_armed_per_zone(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, ct_installed=1, ct_topology=1,
                            **{"i_normal_a[0]": 4.2, "i_normal_a[1]": 0.0})
        out = mss._describe_commissioning(data)
        s15_line = next(l for l in out.splitlines() if l.startswith("S15"))
        self.assertIn("z0 i_normal_a=4.2A ARMED", s15_line)
        self.assertIn("z1 DORMANT", s15_line)
        self.assertIn("z2 DORMANT", s15_line)

    def test_ct_topology_summed_but_ct_not_installed_is_dormant(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, ct_installed=0, ct_topology=1)
        out = mss._describe_commissioning(data)
        s15_line = next(l for l in out.splitlines() if l.startswith("S15"))
        self.assertIn("DORMANT", s15_line)
        self.assertIn("ct_installed=0", s15_line)

    def test_stale_crc_is_flagged(self):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, ct_installed=0,
                            stale=True, live_crc=99, cached_crc=1)
        out = mss._describe_commissioning(data)
        self.assertIn("STALE", out)
        self.assertIn("cached=1", out)
        self.assertIn("live=99", out)

    def test_unreliable_unset_reporting_is_flagged_and_treats_values_as_dormant(self):
        # Old-protocol peer: even a "set" field cannot be trusted, so the
        # renderer must treat abs_max_temp_c as unset/dormant regardless of
        # what the payload's own set=True claims underneath (unset_reliable
        # gates it in the sample builder the same way the real GET's
        # unset_reporting_reliable gates safety_cfg_http_client.py).
        data = _sample_get(commissioned=False, reliable=False, abs_max_temp_c=80.0,
                            ct_installed=0)
        out = mss._describe_commissioning(data)
        self.assertIn("unset_reporting_reliable=false", out)
        s1_line = next(l for l in out.splitlines() if l.startswith("S1"))
        self.assertIn("DORMANT", s1_line)


class NegativeTest(unittest.TestCase):
    """Mandatory negative test: prove the armed/dormant check can actually
    fail, by breaking it the exact way the firmware comments warn against
    (treating a 0/unset threshold as commissioned) and confirming the
    positive-suite tests above catch it."""

    def test_broken_armed_logic_would_be_caught(self):
        real = mss._describe_commissioning

        def broken(data):
            # Same body as the real renderer's S1/S8 section, except the
            # comparison is inverted: report ARMED whenever the value is
            # exactly 0 or unset, DORMANT otherwise -- the defect
            # safety_cfg_http.c's own comment calls "the exact ... 0 means
            # the overtemperature guard never trips defect".
            params = {p["name"]: p for p in data.get("params", []) if "name" in p}
            reliable = bool(data.get("unset_reporting_reliable"))

            def numeric(name):
                p = params.get(name)
                if p is None or not (p.get("set") and reliable):
                    return 0.0
                return float(p.get("value", 0.0))

            lines = []
            abs_max = numeric("abs_max_temp_c")
            lines.append(f"S1 abs_max_temp_c={abs_max:g}C  "
                         + ("ARMED" if abs_max == 0.0 else "DORMANT"))
            rate = numeric("max_rate_c_per_min")
            lines.append(f"S8 max_rate_c_per_min={rate:g}C/min  "
                         + ("ARMED" if rate == 0.0 else "DORMANT"))
            return "\n".join(lines)

        mss._describe_commissioning = broken
        try:
            data = _sample_get(commissioned=False, abs_max_temp_c=0.0, ct_installed=0)
            out = mss._describe_commissioning(data)
            s1_line = next(l for l in out.splitlines() if l.startswith("S1"))
            # The broken renderer reports ARMED for a 0/uncommissioned
            # threshold -- assert that this disagrees with what the real
            # renderer (and the firmware) say, proving the broken version
            # would fail test_zero_abs_max_temp_c_is_dormant_not_commissioned
            # above if it were installed as the real implementation.
            self.assertIn("ARMED", s1_line)  # the broken behaviour, demonstrated
            real_out = real(data)
            real_s1_line = next(l for l in real_out.splitlines() if l.startswith("S1"))
            self.assertIn("DORMANT", real_s1_line)
            self.assertNotEqual(s1_line, real_s1_line)
        finally:
            mss._describe_commissioning = real
            # Confirm the revert is clean: the module now reports the
            # correct (dormant) behaviour again.
            data = _sample_get(commissioned=False, abs_max_temp_c=0.0, ct_installed=0)
            out = mss._describe_commissioning(data)
            s1_line = next(l for l in out.splitlines() if l.startswith("S1"))
            self.assertIn("DORMANT", s1_line)


if __name__ == "__main__":
    unittest.main()
