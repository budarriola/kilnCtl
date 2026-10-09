#!/usr/bin/env python3
"""Unit tests for mcp_server.safety_set_rate_guard()/safety_get_rate_guard()
-- the S8 (rate-of-rise) commissioning write/read path over
GET/POST /api/safety/commissioning (config_store ids 0x0204
max_rate_c_per_min / 0x0205 rate_window_s) -- plus
safety_set_commissioning_fields(), the generic named-field write path over
the same endpoint (the MCP-facade replacement for the paste-ready
apply_safety_fields() snippet CT_COMMISSIONING_PLAN.md step 6a used to
document).

No real socket and no live board: safety_cfg_http_client's
apply_safety_fields()/get_commissioning() are mocked directly, same
convention as test_safety_ct_cal.py mocking SafetyClient methods.

Run with: python -m pytest tools/PcTools/tests/test_safety_rate_guard.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server  # noqa: E402
from kilnctrl import safety_cfg_http_client as sc  # noqa: E402
from kilnctrl.autotune import AutotuneQueryError  # noqa: E402
from kilnctrl.profiles import ProfilesQueryError  # noqa: E402


def _idle_profile():
    return unittest.mock.MagicMock(state=0, state_name="idle", profile_id=0, name="")


def _idle_autotune():
    return unittest.mock.MagicMock(state=0, state_name="idle", zone=0)


class SafetySetRateGuardConfirmGateTests(unittest.TestCase):
    def test_refused_without_confirm(self):
        result = mcp_server.safety_set_rate_guard(33.3)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("confirm=True", result)

    def test_confirm_false_never_touches_the_wire(self):
        with unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            mcp_server.safety_set_rate_guard(33.3, confirm=False)
        mock_apply.assert_not_called()


class SafetySetRateGuardBusyGateTests(unittest.TestCase):
    def test_refused_while_profile_running(self):
        running = unittest.mock.MagicMock(state=1, state_name="running", profile_id=3, name="Cone 6")
        with unittest.mock.patch.object(mcp_server._profiles, "get_exec_status", return_value=running), \
             unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            result = mcp_server.safety_set_rate_guard(33.3, confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("running", result)
        mock_apply.assert_not_called()

    def test_refused_while_profile_paused(self):
        paused = unittest.mock.MagicMock(state=2, state_name="paused", profile_id=3, name="Cone 6")
        with unittest.mock.patch.object(mcp_server._profiles, "get_exec_status", return_value=paused), \
             unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            result = mcp_server.safety_set_rate_guard(33.3, confirm=True)
        self.assertTrue(result.startswith("refused"))
        mock_apply.assert_not_called()

    def test_refused_while_autotune_running(self):
        at = unittest.mock.MagicMock(state=3, state_name="relay_approach", zone=1)
        with unittest.mock.patch.object(mcp_server._profiles, "get_exec_status", return_value=_idle_profile()), \
             unittest.mock.patch.object(mcp_server._autotune, "get_status", return_value=at), \
             unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            result = mcp_server.safety_set_rate_guard(33.3, confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("autotune", result)
        mock_apply.assert_not_called()

    def test_link_down_for_status_checks_does_not_block_the_write(self):
        # A dead link answering the exec-status queries with a QueryError must
        # not be treated as "busy" -- that would make this tool unusable
        # exactly when the link is down, which is unrelated to whether a run
        # is in progress.
        with unittest.mock.patch.object(mcp_server._profiles, "get_exec_status",
                                        side_effect=ProfilesQueryError("timed out")), \
             unittest.mock.patch.object(mcp_server._autotune, "get_status",
                                        side_effect=AutotuneQueryError("timed out")), \
             unittest.mock.patch.object(
                 sc, "apply_safety_fields",
                 return_value=sc.SafetyApplyResult(ok=True, confirmed=["max_rate_c_per_min", "rate_window_s"]),
             ):
            result = mcp_server.safety_set_rate_guard(33.3, confirm=True)
        self.assertTrue(result.startswith("ok"))


class SafetySetRateGuardWriteTests(unittest.TestCase):
    def setUp(self):
        self._patches = [
            unittest.mock.patch.object(mcp_server._profiles, "get_exec_status", return_value=_idle_profile()),
            unittest.mock.patch.object(mcp_server._autotune, "get_status", return_value=_idle_autotune()),
        ]
        for p in self._patches:
            p.start()
            self.addCleanup(p.stop)

    def test_success_reports_armed(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(ok=True, confirmed=["max_rate_c_per_min", "rate_window_s"]),
        ) as mock_apply:
            result = mcp_server.safety_set_rate_guard(33.3, rate_window_s=60.0, confirm=True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("ARMED", result)
        self.assertIn("33.3", result)
        (host, fields), _kwargs = mock_apply.call_args
        self.assertEqual(fields["max_rate_c_per_min"], 33.3)
        self.assertEqual(fields["rate_window_s"], 60.0)

    def test_zero_rate_reports_dormant(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(ok=True, confirmed=["max_rate_c_per_min", "rate_window_s"]),
        ):
            result = mcp_server.safety_set_rate_guard(0.0, confirm=True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("DORMANT", result)

    def test_armed_rejection_names_the_grace_window_fix(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(
                ok=False,
                post_reason="commit rejected: max_rate_c_per_min (id 516) -- relay is ARMED -- "
                            "config writes are refused while ARMED -- values were staged but NOT written",
            ),
        ):
            result = mcp_server.safety_set_rate_guard(33.3, confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn('debug_reset(peer="pico")', result)
        self.assertIn("60 seconds", result)

    def test_readback_mismatch_reported_as_failure_not_success(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(
                ok=False, post_reason="",
                mismatches=["max_rate_c_per_min: expected 33.3, board reports 0.0"],
            ),
        ):
            result = mcp_server.safety_set_rate_guard(33.3, confirm=True)
        self.assertTrue(result.startswith("failed"))
        self.assertIn("33.3", result)

    def test_http_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields", side_effect=sc.SafetyCfgHttpError("unreachable")
        ):
            result = mcp_server.safety_set_rate_guard(33.3, confirm=True)
        self.assertTrue(result.startswith("error"))


class SafetyGetRateGuardTests(unittest.TestCase):
    def _get_payload(self, rate=0.0, window=60, rate_set=True, window_set=True, reliable=True, stale=False):
        params = []
        if rate_set:
            params.append({"id": 516, "name": "max_rate_c_per_min", "type": "f32", "set": True, "value": rate})
        else:
            params.append({"id": 516, "name": "max_rate_c_per_min", "type": "f32", "set": False})
        if window_set:
            params.append({"id": 517, "name": "rate_window_s", "type": "u16", "set": True, "value": window})
        else:
            params.append({"id": 517, "name": "rate_window_s", "type": "u16", "set": False})
        return {
            "link_up": True, "commissioned": True, "unset_reporting_reliable": reliable,
            "stale": stale, "cached_config_crc": 1, "live_config_crc": 1,
            "params": params,
        }

    def test_reports_dormant_when_zero(self):
        with unittest.mock.patch.object(sc, "get_commissioning", return_value=self._get_payload(rate=0.0)):
            result = mcp_server.safety_get_rate_guard()
        self.assertIn("DORMANT", result)
        self.assertIn("rate_window_s=60", result)

    def test_reports_armed_when_positive(self):
        with unittest.mock.patch.object(sc, "get_commissioning", return_value=self._get_payload(rate=33.3)):
            result = mcp_server.safety_get_rate_guard()
        self.assertIn("ARMED", result)
        self.assertIn("33.3", result)

    def test_reports_not_commissioned_when_unset(self):
        with unittest.mock.patch.object(
            sc, "get_commissioning", return_value=self._get_payload(rate_set=False, window_set=False)
        ):
            result = mcp_server.safety_get_rate_guard()
        self.assertIn("not commissioned", result)

    def test_query_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            sc, "get_commissioning", side_effect=sc.SafetyCfgHttpError("unreachable")
        ):
            result = mcp_server.safety_get_rate_guard()
        self.assertTrue(result.startswith("error"))

    def test_reports_stale_warning(self):
        payload = self._get_payload(rate=33.3, stale=True)
        payload["cached_config_crc"] = 1
        payload["live_config_crc"] = 2
        with unittest.mock.patch.object(sc, "get_commissioning", return_value=payload):
            result = mcp_server.safety_get_rate_guard()
        self.assertIn("STALE", result)
        self.assertIn("cached=1", result)
        self.assertIn("live=2", result)

    def test_no_stale_warning_when_not_stale(self):
        with unittest.mock.patch.object(sc, "get_commissioning", return_value=self._get_payload(stale=False)):
            result = mcp_server.safety_get_rate_guard()
        self.assertNotIn("STALE", result)

    def test_reports_unreliable_warning(self):
        with unittest.mock.patch.object(sc, "get_commissioning", return_value=self._get_payload(reliable=False)):
            result = mcp_server.safety_get_rate_guard()
        self.assertIn("unset_reporting_reliable=false", result)
        # unreliable reporting must fall back to DORMANT/not-commissioned,
        # never fabricate ARMED from a value it cannot trust
        self.assertIn("DORMANT", result)

    def test_window_set_reports_value_without_default_note(self):
        with unittest.mock.patch.object(
            sc, "get_commissioning", return_value=self._get_payload(window=90, window_set=True)
        ):
            result = mcp_server.safety_get_rate_guard()
        self.assertIn("rate_window_s=90", result)
        self.assertNotIn("firmware default", result)


class SafetySetCommissioningFieldsTests(unittest.TestCase):
    def setUp(self):
        self._patches = [
            unittest.mock.patch.object(mcp_server._profiles, "get_exec_status", return_value=_idle_profile()),
            unittest.mock.patch.object(mcp_server._autotune, "get_status", return_value=_idle_autotune()),
        ]
        for p in self._patches:
            p.start()
            self.addCleanup(p.stop)

    def test_empty_fields_refused_without_touching_the_wire(self):
        with unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            result = mcp_server.safety_set_commissioning_fields({})
        self.assertTrue(result.startswith("error"))
        mock_apply.assert_not_called()

    def test_refused_while_profile_running(self):
        # Same busy-gate as safety_set_rate_guard: a firing in progress must
        # refuse before anything reaches the wire, not rely on the Pico's own
        # ARMED rejection to catch it.
        running = unittest.mock.MagicMock(state=1, state_name="running", profile_id=3, name="Cone 6")
        with unittest.mock.patch.object(mcp_server._profiles, "get_exec_status", return_value=running), \
             unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            result = mcp_server.safety_set_commissioning_fields({"ct_installed": 1})
        self.assertTrue(result.startswith("refused"))
        self.assertIn("running", result)
        mock_apply.assert_not_called()

    def test_refused_while_autotune_running(self):
        at = unittest.mock.MagicMock(state=3, state_name="relay_approach", zone=1)
        with unittest.mock.patch.object(mcp_server._autotune, "get_status", return_value=at), \
             unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            result = mcp_server.safety_set_commissioning_fields({"ct_installed": 1})
        self.assertTrue(result.startswith("refused"))
        self.assertIn("autotune", result)
        mock_apply.assert_not_called()

    def test_success_reports_confirmed_fields_and_commissioned_state(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(
                ok=True, confirmed=["ct_installed", "ct_topology"], commissioned_after=True,
            ),
        ) as mock_apply:
            result = mcp_server.safety_set_commissioning_fields({"ct_installed": 1, "ct_topology": 1})
        self.assertTrue(result.startswith("ok"))
        self.assertIn("ct_installed=1", result)
        self.assertIn("commissioned: True", result)
        (host, fields), kwargs = mock_apply.call_args
        self.assertEqual(fields, {"ct_installed": 1, "ct_topology": 1})
        self.assertTrue(kwargs.get("verify", True))

    def test_unknown_field_refused_before_write(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            side_effect=sc.SafetyCfgUnknownParamError("the board's parameter table has no field named 'bogus_field'"),
        ):
            result = mcp_server.safety_set_commissioning_fields({"bogus_field": 1})
        self.assertTrue(result.startswith("error"))
        self.assertIn("bogus_field", result)

    def test_armed_rejection_names_the_grace_window_fix(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(
                ok=False,
                post_reason="commit rejected: ct_installed (id 265) -- relay is ARMED -- "
                            "config writes are refused while ARMED -- values were staged but NOT written",
            ),
        ):
            result = mcp_server.safety_set_commissioning_fields({"ct_installed": 1})
        self.assertTrue(result.startswith("refused"))
        self.assertIn('debug_reset(peer="pico")', result)
        self.assertIn("60 seconds", result)

    def test_readback_mismatch_fails_loudly_naming_the_field(self):
        # This is the exact hazard apply_safety_fields()'s verify=True path
        # exists to catch -- an {"ok":true} POST is not proof anything
        # landed. The tool must surface the mismatch, naming the field, not
        # report success on the strength of the POST's own reply.
        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(
                ok=False, post_reason="",
                mismatches=["ct_topology: expected 1, board reports 0"],
            ),
        ):
            result = mcp_server.safety_set_commissioning_fields({"ct_topology": 1})
        self.assertTrue(result.startswith("failed"))
        self.assertIn("ct_topology", result)
        self.assertIn("expected 1, board reports 0", result)

    def test_http_error_surfaces_as_error_string_not_exception(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields", side_effect=sc.SafetyCfgHttpError("unreachable")
        ):
            result = mcp_server.safety_set_commissioning_fields({"ct_installed": 1})
        self.assertTrue(result.startswith("error"))

    def test_still_unset_fields_reported(self):
        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(
                ok=True, confirmed=["ct_installed"], commissioned_after=False,
                still_unset=["abs_max_temp_c"],
            ),
        ):
            result = mcp_server.safety_set_commissioning_fields({"ct_installed": 1})
        self.assertTrue(result.startswith("ok"))
        self.assertIn("still UNSET", result)
        self.assertIn("abs_max_temp_c", result)


if __name__ == "__main__":
    unittest.main()
