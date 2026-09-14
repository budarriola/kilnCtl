#!/usr/bin/env python3
"""Unit tests for control_get_zones()'s plant-model-field surfacing
(mcp_server_control.py, _describe_model_fields()).

THE GAP this closes: control_get_zones()'s docstring advertised "PID/model
config" but never actually rendered model_k_dc/model_tau_s/
model_dead_time_s/tuning_valid/model_fit_temp_c/model_fit_ambient_c --
these ARE present in GET /api/zones's response but were silently dropped,
the same defect class the coupling-matrix and HTTP-only-fields sections
were fixed for earlier (see test_mcp_server_control_coupling.py).

Found during the same review that discovered model_fit_temp_c reads the
-273.15 UNKNOWN sentinel on all three live zones -- exactly the kind of
fact this tool should have surfaced on its own instead of requiring a raw
GET /api/zones. See docs/audits/mcp_zone_model_fields_2026-09-13.md.

All against MOCKED _srv._control and zones_http_client -- no real socket,
no live board.

Run with:
  python -m pytest tools/PcTools/tests/test_mcp_server_control_model_fields.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_control as mc  # noqa: E402


_UNKNOWN = -273.15


def _zone(index, k_dc=0.0, tau_s=0.0, dead_time_s=0.0, tuning_valid=False,
          fit_temp_c=_UNKNOWN, fit_ambient_c=_UNKNOWN):
    return {
        "index": index,
        "model_k_dc": k_dc,
        "model_tau_s": tau_s,
        "model_dead_time_s": dead_time_s,
        "tuning_valid": tuning_valid,
        "model_fit_temp_c": fit_temp_c,
        "model_fit_ambient_c": fit_ambient_c,
        "coupling_diag_k_dc": 0.0,
    }


def _patch(zones_json):
    control_mock = unittest.mock.Mock()
    control_mock.get_zones.return_value = (3, 4, [])
    return (
        unittest.mock.patch.object(mc._srv, "_control", control_mock),
        unittest.mock.patch.object(
            mc.zones_http_client, "get_zones",
            unittest.mock.Mock(return_value=zones_json),
        ),
        unittest.mock.patch.object(
            mc, "_control_resolve_host", unittest.mock.Mock(return_value="10.0.0.9"),
        ),
        # docs/audits/zones_diag_endpoint_split_2026-09-14.md:
        # control_get_zones() now also fetches GET /api/zones_diag and
        # merges model_fit_temp_c/model_fit_ambient_c back in by index --
        # mocked here to return the SAME zones_json's model-fit fields via
        # merge_zones_diag() (the real merge path), so these tests keep
        # exercising the real code between the two fetches rather than
        # bypassing it by pre-populating model_fit_* directly on zones_json.
        unittest.mock.patch.object(
            mc.zones_http_client, "get_zones_diag",
            unittest.mock.Mock(return_value={
                "zones": [
                    {"index": z.get("index"), "model_fit_temp_c": z.get("model_fit_temp_c"),
                     "model_fit_ambient_c": z.get("model_fit_ambient_c")}
                    for z in zones_json.get("zones", [])
                ]
            }),
        ),
    )


class DescribeModelFieldsTest(unittest.TestCase):
    def test_unknown_fit_temp_sentinel_renders_as_UNKNOWN_not_a_number(self):
        """The load-bearing case: -273.15 (ZONE_MODEL_FIT_TEMP_UNKNOWN) must
        never appear in the output as though it were a measured temperature
        -- this is the exact fact that went unnoticed on the live board."""
        zones_json = {"zones": [_zone(0, fit_temp_c=_UNKNOWN, fit_ambient_c=_UNKNOWN)]}
        rendered = mc._describe_model_fields(zones_json)
        self.assertNotIn("-273.15", rendered)
        self.assertIn("UNKNOWN (never recorded)", rendered)

    def test_known_fit_temp_renders_as_a_temperature(self):
        zones_json = {"zones": [_zone(0, k_dc=42.7, tau_s=310.0, dead_time_s=25.0,
                                       tuning_valid=True,
                                       fit_temp_c=110.5, fit_ambient_c=21.3)]}
        rendered = mc._describe_model_fields(zones_json)
        self.assertIn("fit_at=110.50C (ambient=21.30C)", rendered)
        self.assertNotIn("UNKNOWN", rendered)

    def test_no_model_sentinel_renders_as_no_model_not_zero(self):
        """0.0/0.0/0.0 is the documented 'no model identified' encoding
        (zones_config_set_model()'s own header comment: 'Writing all zeros
        is legal and is how a caller clears a stale model'). Must not print
        as K_dc=0.0000."""
        zones_json = {"zones": [_zone(0, k_dc=0.0, tau_s=0.0, dead_time_s=0.0)]}
        rendered = mc._describe_model_fields(zones_json)
        self.assertIn("no model identified", rendered)
        self.assertNotIn("K_dc=0.0000", rendered)

    def test_real_model_values_render_with_units(self):
        zones_json = {"zones": [_zone(0, k_dc=42.731, tau_s=305.2, dead_time_s=18.4,
                                       tuning_valid=True)]}
        rendered = mc._describe_model_fields(zones_json)
        self.assertIn("K_dc=42.7310 C/duty", rendered)
        self.assertIn("tau=305.2s", rendered)
        self.assertIn("dead_time=18.4s", rendered)

    def test_tuning_valid_true_and_false_both_render(self):
        zones_json = {"zones": [
            _zone(0, tuning_valid=True),
            _zone(1, tuning_valid=False),
        ]}
        rendered = mc._describe_model_fields(zones_json)
        self.assertIn("z0:", rendered)
        self.assertIn("z1:", rendered)
        lines = {ln.split(":", 1)[0].strip(): ln for ln in rendered.splitlines() if ln.strip().startswith("z")}
        self.assertIn("tuning_valid=yes", lines["z0"])
        self.assertIn("tuning_valid=no", lines["z1"])

    def test_autotune_baseline_k_dc_reported_as_not_exposed(self):
        """autotune_baseline_k_dc is not currently emitted by GET /api/zones
        at all (zones_http_get.c never writes it, only zones_http_post_parse.c
        reads it back from a POST body) -- this is a firmware gap, and the
        tool must say so explicitly rather than silently omitting it."""
        zones_json = {"zones": [_zone(0)]}
        rendered = mc._describe_model_fields(zones_json)
        self.assertIn("autotune_baseline_k_dc", rendered)
        self.assertIn("NOT exposed by GET /api/zones", rendered)

    def test_missing_fields_render_as_missing_not_crash(self):
        zones_json = {"zones": [{"index": 0}]}
        rendered = mc._describe_model_fields(zones_json)
        self.assertIn("unavailable (field(s) missing", rendered)
        self.assertIn("missing", rendered)

    def test_failed_diag_fetch_does_not_render_like_an_absent_field(self):
        """Adversarial review 2026-09-14 (docs/audits/zones_diag_endpoint_
        split_2026-09-14.md, Review section). The split made model_fit_temp_c/
        model_fit_ambient_c come from a SECOND request; the original code
        swallowed that request's failure with a bare `except ...: pass`, so a
        404/unreachable /api/zones_diag rendered `fit_at=missing
        (ambient=missing)` -- byte-identical to what a firmware that never
        emitted the fields renders. A reader had no way to tell "this board
        has no recorded fit" from "this tool call could not ask". Verified by
        execution against a local HTTP server that 404s only the diag route.
        The two renderings must stay distinguishable."""
        # Realistic post-split shape: GET /api/zones no longer carries
        # model_fit_* at all, so with the diag fetch failing nothing supplies
        # them -- which is exactly the case that used to read "missing".
        zone = _zone(0, k_dc=42.731, tau_s=305.2, dead_time_s=18.4, tuning_valid=True)
        zone.pop("model_fit_temp_c")
        zone.pop("model_fit_ambient_c")
        zones_json = {"zones": [zone]}
        p1, p2, p3, _p4 = _patch(zones_json)
        failing_diag = unittest.mock.patch.object(
            mc.zones_http_client, "get_zones_diag",
            unittest.mock.Mock(side_effect=mc.zones_http_client.ZonesHttpError(
                "GET /api/zones_diag failed: HTTP 404")),
        )
        with p1, p2, p3, failing_diag:
            failed = mc.control_get_zones()
        # The rest of the tool still renders -- graceful degradation is not
        # regressed by carrying the reason.
        self.assertIn("K_dc=42.7310 C/duty", failed)
        self.assertIn("UNAVAILABLE(diag-fetch-failed)", failed)
        self.assertIn("NOT READ this call", failed)
        self.assertIn("404", failed)

        # ... and the genuinely-absent-field rendering, which must NOT claim
        # a fetch failed, still says plainly "missing".
        absent = mc._describe_model_fields({"zones": [{"index": 0}]})
        self.assertIn("missing", absent)
        self.assertNotIn("UNAVAILABLE(diag-fetch-failed)", absent)
        self.assertNotIn("NOT READ this call", absent)

    def test_control_get_zones_includes_plant_model_section(self):
        """End-to-end through control_get_zones() itself, not just the
        helper -- proves the section is actually wired into the tool's
        output, not just unit-tested in isolation."""
        zones_json = {"zones": [_zone(0, k_dc=42.731, tau_s=305.2, dead_time_s=18.4,
                                       tuning_valid=True, fit_temp_c=110.5,
                                       fit_ambient_c=21.3)]}
        p1, p2, p3, p4 = _patch(zones_json)
        with p1, p2, p3, p4:
            result = mc.control_get_zones()
        self.assertIn("plant model", result)
        self.assertIn("K_dc=42.7310 C/duty", result)
        self.assertIn("fit_at=110.50C", result)

    def test_NEGATIVE_sentinel_check_is_not_vacuous(self):
        """Negative test (required by feedback_negative_test_every_check):
        prove the UNKNOWN-sentinel test above would actually fail if the
        rendering regressed to printing the raw float instead of the label.
        Simulates that regression by calling a broken renderer inline rather
        than editing production source, since the point is to show the
        assertion has teeth, not to touch shipped code for this proof."""
        def _broken_describe_model_fields(zones_json):
            zones = zones_json.get("zones", [])
            lines = ["plant model:"]
            for i, z in enumerate(zones):
                lines.append(f"  z{i}: fit_at={z.get('model_fit_temp_c')}")
            return "\n".join(lines)

        zones_json = {"zones": [_zone(0, fit_temp_c=_UNKNOWN)]}
        broken_rendered = _broken_describe_model_fields(zones_json)
        self.assertIn("-273.15", broken_rendered)  # confirms the broken path IS caught
        with self.assertRaises(AssertionError):
            self.assertNotIn("-273.15", broken_rendered)


if __name__ == "__main__":
    unittest.main()
