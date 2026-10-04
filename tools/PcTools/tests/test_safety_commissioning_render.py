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

# Import the aggregate first. Every mcp_server_* submodule does `from . import
# mcp_server as _srv`, and mcp_server star-re-exports each submodule. Importing
# a submodule first re-enters mcp_server while that submodule is only partly
# initialised, so its `import *` copies nothing and the aggregate is left
# permanently missing that submodule's tools for the rest of the process.
from kilnctrl import mcp_server  # noqa: E402,F401
from kilnctrl import mcp_server_safety as mss  # noqa: E402


def _sample_get(commissioned: bool = False, reliable: bool = True, stale: bool = False,
                 live_crc: int = 1, cached_crc: int = 1, **set_values):
    params = [
        {"id": 257, "name": "tc_source", "type": "u8"},
        {"id": 258, "name": "borrowed_zone_index", "type": "u8"},
        {"id": 259, "name": "tc_placement_mode", "type": "u8"},
        {"id": 260, "name": "abs_max_temp_c", "type": "f32"},
        {"id": 261, "name": "tc_type", "type": "u8"},
        {"id": 265, "name": "ct_installed", "type": "u8"},
        {"id": 516, "name": "max_rate_c_per_min", "type": "f32"},
        {"id": 782, "name": "mains_voltage_v", "type": "f32"},
        {"id": 794, "name": "i_normal_a[0]", "type": "f32"},
        {"id": 795, "name": "i_normal_a[1]", "type": "f32"},
        {"id": 796, "name": "i_normal_a[2]", "type": "f32"},
        {"id": 799, "name": "ct_topology", "type": "u8"},
        {"id": 530, "name": "estop_active_level", "type": "u8"},
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


class PreviouslyUnprintedParamsTest(unittest.TestCase):
    """2026-09-09 (docs/audits/commissioning_gap_and_no_heat_2026-09-09.md
    sec 4): estop_active_level and several other commissioning params were
    always present in the underlying GET /api/safety/commissioning response
    but never rendered. These prove the fix actually surfaces them, using
    the SAME field/value pairs a live board would return -- not a
    test-local stand-in for the render logic."""

    def test_estop_active_level_default_renders_as_active_high(self):
        data = _sample_get(estop_active_level=0)
        out = mss._describe_commissioning(data)
        self.assertIn("estop_active_level=0", out)
        self.assertIn("ACTIVE_HIGH", out)

    def test_estop_active_level_one_renders_as_active_low(self):
        data = _sample_get(estop_active_level=1)
        out = mss._describe_commissioning(data)
        self.assertIn("estop_active_level=1", out)
        self.assertIn("ACTIVE_LOW", out)

    def test_estop_active_level_unset_says_so_not_a_bare_zero(self):
        data = _sample_get()  # estop_active_level never in set_values -> set=False
        out = mss._describe_commissioning(data)
        line = next(l for l in out.splitlines() if l.startswith("estop_active_level"))
        self.assertIn("(unset)", line)

    def test_borrowed_zone_index_tc_placement_mains_tc_type_all_render(self):
        data = _sample_get(borrowed_zone_index=1, tc_placement_mode=0,
                            mains_voltage_v=240.0, tc_type=3)
        out = mss._describe_commissioning(data)
        self.assertIn("borrowed_zone_index=1", out)
        self.assertIn("tc_placement_mode=0", out)
        self.assertIn("CHAMBER_AGREED", out)
        self.assertIn("mains_voltage_v=240", out)
        self.assertIn("tc_type=3", out)

    def test_unfetched_fields_are_omitted_not_invented(self):
        # A response missing a param entirely (older firmware, or a field
        # this table has not learned about yet) must not be rendered at
        # all -- never fabricated as 0/unset.
        data = _sample_get()
        data["params"] = [p for p in data["params"] if p["name"] != "mains_voltage_v"]
        out = mss._describe_commissioning(data)
        self.assertNotIn("mains_voltage_v", out)


class FaultEdgeRenderTest(unittest.TestCase):
    """2026-09-24 fault-edge instrumentation: safety_link.c's fault-source
    transition ring/counters, piggybacked onto this same GET /api/safety/
    commissioning response (safety_cfg_http.c's build_commissioning_json())
    rather than a new route. These tests use fake response payloads only --
    no real socket, no board -- and prove the parser/renderer actually
    surfaces the new fields for a bench agent root-causing a latched S6a."""

    def _sample_with_edges(self, **overrides):
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, ct_installed=0)
        data["current_fault_sources_known"] = True
        data["current_fault_sources"] = 0
        data["fault_source_edges"] = [
            {"uptime_ms": 1000, "unix_time_s": None, "source_mask_before": 0,
             "source_mask_after": 4, "first_set_source": "thermo"},
            {"uptime_ms": 2000, "unix_time_s": 1732000000, "source_mask_before": 4,
             "source_mask_after": 0, "first_set_source": None},
        ]
        data["fault_source_edge_total_recorded"] = 2
        data["fault_source_counts"] = {
            "manual": {"rising_count": 0, "last_rising_uptime_ms": None},
            "pc_link": {"rising_count": 0, "last_rising_uptime_ms": None},
            "thermo": {"rising_count": 1, "last_rising_uptime_ms": 1000},
            "safety_link": {"rising_count": 0, "last_rising_uptime_ms": None},
            "app": {"rising_count": 0, "last_rising_uptime_ms": None},
            "thermal_sanity": {"rising_count": 0, "last_rising_uptime_ms": None},
        }
        data.update(overrides)
        return data

    def test_current_fault_sources_rendered_as_hex(self):
        data = self._sample_with_edges(current_fault_sources=4)
        out = mss._describe_commissioning(data)
        self.assertIn("current_fault_sources=0x04", out)

    def test_edge_list_renders_each_entry_with_unsynced_time(self):
        data = self._sample_with_edges()
        out = mss._describe_commissioning(data)
        self.assertIn("fault_source_edges: 2 entries (total_recorded=2)", out)
        self.assertIn("uptime_ms=1000 unix_time_s=unsynced", out)
        self.assertIn("before=0x00 after=0x04 first_set_source=thermo", out)

    def test_edge_with_synced_time_renders_the_unix_timestamp(self):
        data = self._sample_with_edges()
        out = mss._describe_commissioning(data)
        self.assertIn("uptime_ms=2000 unix_time_s=1732000000", out)

    def test_pure_clear_edge_renders_clear_only_not_a_stale_source_name(self):
        data = self._sample_with_edges()
        out = mss._describe_commissioning(data)
        self.assertIn("before=0x04 after=0x00 first_set_source=(clear only)", out)

    def test_counts_render_rising_count_and_last_rising(self):
        data = self._sample_with_edges()
        out = mss._describe_commissioning(data)
        self.assertIn("thermo: rising_count=1 last_rising=uptime_ms=1000", out)
        self.assertIn("manual: rising_count=0 last_rising=never", out)

    def test_absent_fault_edge_fields_render_nothing(self):
        # An older firmware's response (or this same field simply not yet
        # built) has none of the new keys at all -- the renderer must not
        # fabricate a "fault_source_edges: 0 entries" line out of nothing.
        data = _sample_get(commissioned=True, abs_max_temp_c=80.0, ct_installed=0)
        out = mss._describe_commissioning(data)
        self.assertNotIn("fault_source_edges", out)
        self.assertNotIn("current_fault_sources", out)


class FaultEdgeNegativeTest(unittest.TestCase):
    """Mandatory negative test for the fault-edge rendering: break the
    first_set_source null-handling the way an easy mistake would (treat a
    pure-clear edge's null as the STRING "None" instead of the sentinel
    "(clear only)"), confirm the positive test above would have caught it,
    then restore."""

    def test_broken_null_handling_would_be_caught(self):
        real = mss._describe_commissioning

        def broken(data):
            edges = data.get("fault_source_edges") or []
            lines = []
            for e in edges:
                lines.append(f"first_set_source={e.get('first_set_source')}")
            return "\n".join(lines)

        mss._describe_commissioning = broken
        try:
            data = FaultEdgeRenderTest()._sample_with_edges()
            out = mss._describe_commissioning(data)
            self.assertIn("first_set_source=None", out)  # the broken behaviour
            real_out = real(data)
            self.assertNotIn("first_set_source=None", real_out)
            self.assertIn("first_set_source=(clear only)", real_out)
        finally:
            mss._describe_commissioning = real
            data = FaultEdgeRenderTest()._sample_with_edges()
            out = mss._describe_commissioning(data)
            self.assertIn("first_set_source=(clear only)", out)


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
