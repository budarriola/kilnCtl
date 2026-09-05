#!/usr/bin/env python3
"""Unit tests for control_get_zones()'s coupling-matrix surfacing
(mcp_server_control.py).

THE GAP this closes: neither control_get_zones() nor get_board_state()
surfaced coupling_c0/c1/c2 or coupling_diag_k_dc -- the coupling matrix,
even though it is a first-class control parameter (an A/B campaign measured
a ~0.6 C whole-run IAE difference between two matrices), was only readable
by curling GET /api/zones directly. This fetches it via zones_http_client
(already unit-tested against a mocked HTTP response there) and renders it
labeled [affected][stepped] with a zero diagonal -- a transposed matrix is a
real bug class in this repo (test_zones_http_client.py's
test_TRANSPOSED_mapping_is_caught_by_this_test), so the orientation test
below pins it specifically: it would fail if the renderer instead printed
c[j][i] (transposed).

All against MOCKED _srv._control and zones_http_client -- no real socket, no
live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_control_coupling.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_control as mc  # noqa: E402
from kilnctrl import zones_http_client as zh  # noqa: E402


def _zones_json(n=3):
    """A GET /api/zones body carrying the confirmed-live board values:
    z0 [0, 27.32, 21.72]  z1 [14.30, 0, 22.15]  z2 [8.33, 12.42, 0].
    coupling_diag_k_dc is left at 0.0 (never identified) for zones 0/1,
    and given a measured-but-unused value for zone 2 to exercise that
    branch too."""
    rows = [
        [0.0, 27.32, 21.72],
        [14.30, 0.0, 22.15],
        [8.33, 12.42, 0.0],
    ]
    diag_k_dc = [0.0, 0.0, 9.5]
    zones = []
    for i in range(n):
        zone = {"index": i}
        for j in range(n):
            zone[f"coupling_c{j}"] = rows[i][j]
        zone["coupling_diag_k_dc"] = diag_k_dc[i]
        zones.append(zone)
    return {"zones": zones}


class ControlGetZonesCouplingTest(unittest.TestCase):
    def test_http_only_fields_are_surfaced(self):
        """The gap this closes: the 2026-09-04 bench snapshot reported
        fuzzy_strength_pct/ease_off_window_mult/approach_rate_cap_c_per_hr
        as unreadable from the live board at all. They ARE in the board's
        raw GET /api/zones response (confirmed against a live board); this
        tool just fetched that response and dropped everything but the
        coupling matrix. Prove all three now appear per zone."""
        zones_json = _zones_json()
        zones_json["zones"][0]["fuzzy_strength_pct"] = 50.0
        zones_json["zones"][0]["ease_off_window_mult"] = 2.0
        zones_json["zones"][0]["approach_rate_cap_c_per_hr"] = 0.0
        zones_json["zones"][1]["fuzzy_strength_pct"] = 0.0
        zones_json["zones"][1]["ease_off_window_mult"] = 2.0
        zones_json["zones"][1]["approach_rate_cap_c_per_hr"] = 0.0
        p1, p2, p3 = self._patch(zones_json)
        with p1, p2, p3:
            result = mc.control_get_zones()
        self.assertIn("http-only fields", result)
        self.assertIn(
            "z0: fuzzy_strength_pct=50.0, ease_off_window_mult=2.0, "
            "approach_rate_cap_c_per_hr=0.0",
            result,
        )
        self.assertIn(
            "z1: fuzzy_strength_pct=0.0, ease_off_window_mult=2.0, "
            "approach_rate_cap_c_per_hr=0.0",
            result,
        )

    def test_NEGATIVE_missing_projected_field_is_caught(self):
        """Negative test (required by feedback_negative_test_every_check):
        prove the positive test above would actually fail if a field were
        dropped from HTTP_ONLY_ZONE_FIELDS again, the same way
        fuzzy_strength_pct/ease_off_window_mult/approach_rate_cap_c_per_hr
        were silently dropped before this fix."""
        zones_json = _zones_json()
        zones_json["zones"][0]["fuzzy_strength_pct"] = 50.0
        zones_json["zones"][0]["ease_off_window_mult"] = 2.0
        zones_json["zones"][0]["approach_rate_cap_c_per_hr"] = 0.0
        with unittest.mock.patch.object(
            mc, "HTTP_ONLY_ZONE_FIELDS",
            ("fuzzy_strength_pct", "approach_rate_cap_c_per_hr"),
        ):
            rendered = mc._describe_http_only_zone_fields(zones_json)
        self.assertIn("fuzzy_strength_pct=50.0", rendered)
        self.assertNotIn("ease_off_window_mult", rendered)

    def _patch(self, zones_json):
        control_mock = unittest.mock.Mock()
        control_mock.get_zones.return_value = (5, 8, [])
        return (
            unittest.mock.patch.object(mc._srv, "_control", control_mock),
            unittest.mock.patch.object(
                mc.zones_http_client, "get_zones",
                unittest.mock.Mock(return_value=zones_json),
            ),
            unittest.mock.patch.object(
                mc, "_control_resolve_host", unittest.mock.Mock(return_value="10.0.0.9"),
            ),
        )

    def test_coupling_matrix_appears_in_documented_orientation(self):
        """Positive case: the tool's output matches the confirmed-live
        board values, in [affected][stepped] orientation, next to a label
        naming that orientation."""
        p1, p2, p3 = self._patch(_zones_json())
        with p1, p2, p3:
            result = mc.control_get_zones()
        self.assertIn("row i = AFFECTED zone, column j = STEPPED zone", result)
        self.assertIn("z0: [0, 27.32, 21.72]", result)
        self.assertIn("z1: [14.30, 0, 22.15]", result)
        self.assertIn("z2: [8.33, 12.42, 0]", result)

    def test_diag_k_dc_zero_means_unmeasured_not_zero_gain(self):
        p1, p2, p3 = self._patch(_zones_json())
        with p1, p2, p3:
            result = mc.control_get_zones()
        self.assertIn("z0=0.0 (never identified on hardware)", result)
        self.assertIn("z1=0.0 (never identified on hardware)", result)
        self.assertIn("z2=9.5000", result)
        self.assertIn("s_coupling_use_measured_diag_k_dc is compiled false", result)
        self.assertIn("not currently used", result)

    def test_TRANSPOSED_mapping_is_caught_by_this_test(self):
        """Negative test (required by feedback_negative_test_every_check):
        prove the orientation assertion above would actually fail if the
        renderer printed the matrix transposed -- c[j][i] instead of
        c[i][j]. Feed _describe_coupling_matrix a manually-transposed JSON
        body (row i now holds what should be column i) and confirm the
        known-good z0/z1/z2 strings from the positive test do NOT appear.

        Run standalone to see the real failure this guards against:
            python -m pytest tools/PcTools/tests/test_mcp_server_control_coupling.py::ControlGetZonesCouplingTest::test_TRANSPOSED_mapping_is_caught_by_this_test -q
        which passes (it asserts absence). Flip the assertIn/assertNotIn
        pair below by hand to see it fail against the correct orientation.
        """
        good = _zones_json()
        n = len(good["zones"])
        transposed = {"zones": [{"index": i} for i in range(n)]}
        for i in range(n):
            for j in range(n):
                transposed["zones"][i][f"coupling_c{j}"] = good["zones"][j][f"coupling_c{i}"]
            transposed["zones"][i]["coupling_diag_k_dc"] = good["zones"][i]["coupling_diag_k_dc"]

        rendered = mc._describe_coupling_matrix(transposed)
        # z0's row in the correct orientation is [0, 27.32, 21.72]; under
        # the transposed body it must NOT read that way.
        self.assertNotIn("z0: [0, 27.32, 21.72]", rendered)
        # what it actually prints instead -- z0's column from the correct
        # matrix, i.e. [0, 14.30, 8.33] -- proving this is a real, different
        # (wrong) result and not a no-op transpose.
        self.assertIn("z0: [0, 14.30, 8.33]", rendered)

    def test_http_fetch_failure_still_returns_pid_section(self):
        control_mock = unittest.mock.Mock()
        control_mock.get_zones.return_value = (5, 8, [])
        with unittest.mock.patch.object(mc._srv, "_control", control_mock), \
             unittest.mock.patch.object(
                 mc.zones_http_client, "get_zones",
                 unittest.mock.Mock(side_effect=zh.ZonesHttpError("unreachable: timed out")),
             ), \
             unittest.mock.patch.object(
                 mc, "_control_resolve_host", unittest.mock.Mock(return_value="10.0.0.9"),
             ):
            result = mc.control_get_zones()
        self.assertIn("5 thermocouple(s), 8 relay(s)", result)
        self.assertIn("coupling matrix: unavailable", result)
        self.assertIn("10.0.0.9", result)


if __name__ == "__main__":
    unittest.main()
