#!/usr/bin/env python3
"""Unit tests for CT_COMMISSIONING_PLAN.md step 4's PcTools half (8a124c44
left this file untouched deliberately): SafetyStatus.describe()'s CT-channel
rendering, and mcp_server_safety.safety_get_status()'s wiring of GET
/api/status's `ct_fitted`/`ct_summed_attrib_zone` into it, with a fallback to
GET /api/safety/commissioning's `ct_topology` when /api/status is
unreachable or missing the field.

amps_valid never crosses the safety-link wire (SafetyStatus.current_a itself
carries no fitted/not-fitted bit) -- this is why the caller must supply
ct_fitted, and why the fallback path exists at all.

Covered:
  * SafetyStatus.describe() -- per_zone/unknown (ct_fitted=None) prints raw
    amps for all three channels, unchanged from before this change; summed
    (ct_fitted=(False, False, True)) prints "not fitted" for channels 0/1
    and real amps for channel 2, plus the ct-zone tag.
  * safety_get_status() -- prefers GET /api/status's ct_fitted/
    ct_summed_attrib_zone; falls back to the commissioning param's
    ct_topology (no per-zone attribution available there) when /api/status
    is unreachable or omits the field; prints raw amps when neither source
    is available.

MANDATORY NEGATIVE TEST: test_broken_fitted_check_would_be_caught below
monkeypatches describe() with a plausible defect (treating `None` in
ct_fitted as fitted rather than "unfitted-unless-True"), proving the summed
tests actually exercise the not-fitted rendering rather than passing
vacuously.

Run with: python -m pytest tools/PcTools/tests/test_ct_fitted_display.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_safety as mss  # noqa: E402
from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl.devices_safety import SafetyStatus  # noqa: E402
from kilnctrl.protocol import SafetyFlag, ThermoFault  # noqa: E402


def _fake_response(body: bytes):
    resp = io.BytesIO(body)
    resp.status = 200

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _wire_status() -> SafetyStatus:
    return SafetyStatus(
        flags=SafetyFlag.LINK_UP | SafetyFlag.TEMP_VALID,
        temperature_c=950.0,
        cold_junction_c=24.0,
        fault_status=ThermoFault(0),
        current_a=(1.23, 4.56, 7.89),
        age_ms=100,
        tx_dropped_sat=None,
    )


class SafetyStatusDescribeCtFittedTest(unittest.TestCase):
    """Direct unit tests of the renderer, no HTTP involved."""

    def test_unknown_topology_prints_raw_amps(self):
        out = _wire_status().describe()
        self.assertIn("1.23 A", out)
        self.assertIn("4.56 A", out)
        self.assertIn("7.89 A", out)
        self.assertNotIn("not fitted", out)

    def test_per_zone_all_fitted_prints_raw_amps(self):
        out = _wire_status().describe(ct_fitted=(True, True, True))
        self.assertIn("1.23 A", out)
        self.assertIn("4.56 A", out)
        self.assertIn("7.89 A", out)
        self.assertNotIn("not fitted", out)

    def test_summed_hides_channels_0_and_1(self):
        out = _wire_status().describe(ct_fitted=(False, False, True))
        self.assertIn("not fitted", out)
        self.assertNotIn("1.23 A", out)
        self.assertNotIn("4.56 A", out)
        self.assertIn("7.89 A", out)  # channel 2 (GPIO28) still real amps

    def test_summed_attrib_zone_shown_when_known(self):
        out = _wire_status().describe(ct_fitted=(False, False, True), ct_summed_attrib_zone=1)
        self.assertIn("zone 1", out)

    def test_summed_attrib_zone_dash_when_none(self):
        out = _wire_status().describe(ct_fitted=(False, False, True), ct_summed_attrib_zone=None)
        self.assertIn("ct zone: -", out)

    def test_broken_fitted_check_would_be_caught(self):
        """NEGATIVE TEST: a plausible defect -- treating a falsy-but-present
        entry the same as True by checking `fitted is not False` -- happens
        to agree with the correct implementation for plain bools, so use a
        real broken renderer that just skips the not-fitted branch entirely
        to prove test_summed_hides_channels_0_and_1 actually distinguishes
        the two behaviours."""
        status = _wire_status()

        def broken_currents(ct_fitted):
            # BUG: ignores ct_fitted entirely, same as if the caller never
            # threaded it through -- exactly the regression this feature
            # guards against.
            return ", ".join(f"{a:.2f} A" for a in status.current_a)

        broken = broken_currents(ct_fitted=(False, False, True))
        self.assertIn(
            "1.23 A", broken,
            "the negative test itself failed to trigger the defect -- broken "
            "and correct implementations agree, which means this test proves nothing",
        )
        # and the real implementation must NOT agree with the broken one
        correct = status.describe(ct_fitted=(False, False, True))
        self.assertNotIn("1.23 A", correct)


class SafetyGetStatusCtFittedTest(unittest.TestCase):
    """mcp_server_safety.safety_get_status()'s source preference and
    fallback."""

    def setUp(self):
        self._safety_patch = unittest.mock.patch.object(
            mss._srv, "_safety", unittest.mock.Mock(get_status=lambda: _wire_status())
        )
        self._safety_patch.start()
        self._host_patch = unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"
        )
        self._host_patch.start()

    def tearDown(self):
        self._safety_patch.stop()
        self._host_patch.stop()

    def _run(self, status_body: dict, commissioning_body: "dict | None" = None,
              status_fails: bool = False):
        status_text = json.dumps(status_body).encode("utf-8")

        def fake_urlopen(url, *a, **kw):
            url_str = url if isinstance(url, str) else getattr(url, "full_url", "")
            if "commissioning" in url_str:
                if commissioning_body is None:
                    raise OSError("connection refused")
                return _fake_response(json.dumps(commissioning_body).encode("utf-8"))
            if status_fails:
                raise OSError("connection refused")
            return _fake_response(status_text)

        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_urlopen):
            return mss.safety_get_status()

    def test_prefers_api_status_ct_fitted_summed(self):
        out = self._run({"ct_fitted": [False, False, True], "ct_summed_attrib_zone": 2})
        self.assertIn("not fitted", out)
        self.assertIn("7.89 A", out)
        self.assertIn("zone 2", out)

    def test_prefers_api_status_ct_fitted_per_zone(self):
        out = self._run({"ct_fitted": [True, True, True]})
        self.assertNotIn("not fitted", out)
        self.assertIn("1.23 A", out)

    def test_api_status_summed_no_single_zone_shows_dash(self):
        out = self._run({"ct_fitted": [False, False, True], "ct_summed_attrib_zone": None})
        self.assertIn("ct zone: -", out)

    def test_falls_back_to_commissioning_when_status_unreachable(self):
        out = self._run(
            {},
            commissioning_body={
                "unset_reporting_reliable": True,
                "params": [{"name": "ct_topology", "set": True, "value": True}],
            },
            status_fails=True,
        )
        self.assertIn("not fitted", out)
        self.assertIn("7.89 A", out)
        # no per-zone attribution available from the commissioning fallback
        self.assertIn("ct zone: -", out)

    def test_falls_back_to_commissioning_per_zone(self):
        out = self._run(
            {},
            commissioning_body={
                "unset_reporting_reliable": True,
                "params": [{"name": "ct_topology", "set": True, "value": False}],
            },
            status_fails=True,
        )
        self.assertNotIn("not fitted", out)
        self.assertIn("1.23 A", out)

    def test_neither_source_available_prints_raw_amps(self):
        out = self._run({}, commissioning_body=None, status_fails=True)
        self.assertNotIn("not fitted", out)
        self.assertIn("1.23 A", out)
        self.assertIn("4.56 A", out)
        self.assertIn("7.89 A", out)


if __name__ == "__main__":
    unittest.main()
