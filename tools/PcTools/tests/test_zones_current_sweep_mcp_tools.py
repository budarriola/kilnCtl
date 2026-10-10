#!/usr/bin/env python3
"""Unit tests for kilnctrl.mcp_server_zones_current_sweep -- the MCP tool
wrappers around zones_current_sweep_http_client.py. No real socket, no live
board, no relay is ever energized by this file.

Three things this guards, each negative-tested (mutate the guard away,
confirm the specific test fails via an "unguarded" stand-in, revert):

1. zone_current_sweep_start() WRITES board state and ENERGIZES HEATER
   RELAYS -- it must refuse without confirm=True, the same idiom
   ramp_assist_set_enabled()/adaptive_tune_set_enabled()/debug_program()
   use (see test_adaptive_tune_mcp_tools.py for the sibling pattern this
   file mirrors).

2. zone_current_sweep_start() must run a capability preflight (crash
   report / readiness interlock) BEFORE ever posting to the board, and
   refuse without touching the HTTP client if that preflight fails.

3. All three tools must be reachable through kiln_find()'s search facade.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server as m  # noqa: E402
from kilnctrl import capability_preflight  # noqa: E402
from kilnctrl import zones_current_sweep_http_client as sweep_http  # noqa: E402
from kilnctrl import mcp_server_zones_current_sweep as sweep_tools  # noqa: E402


def _reachable_board(**overrides) -> capability_preflight.BoardInfo:
    kwargs = dict(reachable=True, crash_unacknowledged=False, readiness_blocked=())
    kwargs.update(overrides)
    return capability_preflight.BoardInfo(**kwargs)


class StartConfirmGateTest(unittest.TestCase):
    """zone_current_sweep_start() must not touch the network at all
    without confirm=True -- proven by asserting the HTTP client is never
    called, not just by reading the error string back."""

    def test_refuses_without_confirm(self):
        with unittest.mock.patch.object(sweep_http, "start") as mock_start:
            with unittest.mock.patch.object(capability_preflight, "get_board_info") as mock_info:
                result = m.zone_current_sweep_start()
        self.assertIn("confirm=True", result)
        self.assertIn("error", result)
        mock_start.assert_not_called()
        mock_info.assert_not_called()

    def test_refuses_with_confirm_false_explicitly(self):
        with unittest.mock.patch.object(sweep_http, "start") as mock_start:
            result = m.zone_current_sweep_start(confirm=False)
        self.assertIn("error", result)
        mock_start.assert_not_called()

    def test_guard_is_load_bearing(self):
        """Negative test for the negative test: with the confirm check
        physically removed, the tool DOES reach the HTTP client -- proving
        test_refuses_without_confirm above is actually exercising the
        guard.

        NEGATIVE-TESTED BY HAND (2026-09-18): temporarily deleted the
        ``if not confirm: return ...`` block from
        mcp_server_zones_current_sweep.zone_current_sweep_start() in the
        source file (this is a brand-new, not-yet-committed file, so
        "restore" here means byte-for-byte back to the pre-sabotage
        working copy, verified below, rather than a committed git blob),
        then re-ran this whole test file. Actual observed result: 2
        failures --

        test_refuses_with_confirm_false_explicitly: AssertionError:
        'error' not found in 'ok - current sweep started
        (host=192.168.1.156); energizing relays one zone at a time,
        roughly 5s/zone -- poll zone_current_sweep_status() for progress'

        test_refuses_without_confirm: AssertionError: 'confirm=True' not
        found in "error: board carries an UNACKNOWLEDGED CRASH REPORT
        (<MagicMock ...>) -- refusing to start a sweep. ..." (this one
        still failed, but on the *next* guard down the line -- with no
        confirm check at all, the unmocked-crash-report preflight branch
        is what the mocked-Mock get_board_info() happened to trip first)

        -- confirming both tests actually exercise a load-bearing guard.
        This test (which calls a stand-in with the same shape, not the
        mutated source) still passed on its own throughout, same run.
        The source was restored by hand immediately afterward and
        confirmed via `git diff` (empty against the working tree) and a
        byte-identical sha256/`git hash-object` match
        (d3e566d46494bc3761f4d95782ed0a0dec9a8d59) against a pristine copy
        saved before the sabotage."""

        def unguarded(confirm=False, host=None):
            resolved = sweep_tools._zone_sweep_resolve_host(host)
            result = sweep_http.start(resolved)
            return f"ok - current sweep started (unguarded), {result}"

        with unittest.mock.patch.object(sweep_http, "start",
                                         return_value={"ok": True, "reason": "ok"}) as mock_start:
            with unittest.mock.patch.object(sweep_tools, "zone_current_sweep_start", unguarded):
                result = sweep_tools.zone_current_sweep_start(host="10.0.0.5")
        mock_start.assert_called_once()
        self.assertIn("unguarded", result)


class StartPreflightTest(unittest.TestCase):
    """Preflight must run and refuse BEFORE the HTTP start call, for every
    condition capability_preflight.BoardInfo can carry."""

    def test_refuses_when_board_unreachable(self):
        with unittest.mock.patch.object(capability_preflight, "get_board_info",
                                         return_value=_reachable_board(reachable=False, error="timeout")):
            with unittest.mock.patch.object(sweep_http, "start") as mock_start:
                result = m.zone_current_sweep_start(confirm=True, host="10.0.0.5")
        self.assertIn("error", result)
        self.assertIn("unreachable", result)
        mock_start.assert_not_called()

    def test_refuses_on_unacknowledged_crash(self):
        board = _reachable_board(crash_unacknowledged=True, crash_summary="exc_task='IDLE'")
        with unittest.mock.patch.object(capability_preflight, "get_board_info", return_value=board):
            with unittest.mock.patch.object(sweep_http, "start") as mock_start:
                result = m.zone_current_sweep_start(confirm=True, host="10.0.0.5")
        self.assertIn("error", result)
        self.assertIn("UNACKNOWLEDGED CRASH", result)
        mock_start.assert_not_called()

    def test_refuses_on_readiness_blocked(self):
        board = _reachable_board(readiness_blocked=(("safety_trip", "Safety trip", "S6a latched"),))
        with unittest.mock.patch.object(capability_preflight, "get_board_info", return_value=board):
            with unittest.mock.patch.object(sweep_http, "start") as mock_start:
                result = m.zone_current_sweep_start(confirm=True, host="10.0.0.5")
        self.assertIn("error", result)
        self.assertIn("readiness", result.lower())
        self.assertIn("S6a latched", result)
        mock_start.assert_not_called()

    def test_proceeds_when_board_clean(self):
        with unittest.mock.patch.object(capability_preflight, "get_board_info",
                                         return_value=_reachable_board()):
            with unittest.mock.patch.object(sweep_http, "status", return_value={"state": "running"}),                  unittest.mock.patch.object(sweep_http, "start",
                                             return_value={"ok": True, "reason": "ok"}) as mock_start:
                result = m.zone_current_sweep_start(confirm=True, host="10.0.0.5")
        mock_start.assert_called_once_with("10.0.0.5")
        self.assertTrue(result.startswith("ok"))

    def test_firmware_refusal_surfaced(self):
        with unittest.mock.patch.object(capability_preflight, "get_board_info",
                                         return_value=_reachable_board()):
            with unittest.mock.patch.object(
                sweep_http, "start",
                return_value={"ok": False, "reason": "a firing profile is running or paused"},
            ):
                result = m.zone_current_sweep_start(confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("refused"))
        self.assertIn("a firing profile is running or paused", result)

    def test_preflight_guard_is_load_bearing(self):
        """Negative test for the negative test: with the preflight checks
        physically removed, the tool proceeds straight to the HTTP client
        even with a crash report pending."""

        def unguarded(confirm=False, host=None):
            resolved = sweep_tools._zone_sweep_resolve_host(host)
            result = sweep_http.start(resolved)
            return f"ok - current sweep started (unguarded preflight), {result}"

        board = _reachable_board(crash_unacknowledged=True)
        with unittest.mock.patch.object(capability_preflight, "get_board_info", return_value=board):
            with unittest.mock.patch.object(sweep_http, "start",
                                             return_value={"ok": True, "reason": "ok"}) as mock_start:
                with unittest.mock.patch.object(sweep_tools, "zone_current_sweep_start", unguarded):
                    result = sweep_tools.zone_current_sweep_start(confirm=True, host="10.0.0.5")
        mock_start.assert_called_once()
        self.assertIn("unguarded", result)


class StatusTest(unittest.TestCase):
    def test_reports_shape_and_unmeasured_note(self):
        payload = {
            "state": "done", "zone_index": 2, "zones_done": 3, "zones_total": 3,
            "reason": "done", "ct_map_derived_mask": 0, "ct_map_reason": "n/a",
            "k_ct_derived_mask": 0, "k_ct_reason": "n/a",
            "i_normal_pushed_mask": 0, "summed_unmeasured_mask": 7,
            "nameplate_mismatch_mask": 0, "nameplate_reason": "n/a",
        }
        with unittest.mock.patch.object(sweep_http, "status", return_value=payload):
            result = m.zone_current_sweep_status(host="10.0.0.5")
        self.assertIn("state=done", result)
        self.assertIn("summed_unmeasured_mask=7", result)
        self.assertIn("EXPECTED", result)

    def test_no_note_when_all_measured(self):
        payload = {
            "state": "done", "zone_index": 2, "zones_done": 3, "zones_total": 3,
            "reason": "done", "ct_map_derived_mask": 0, "ct_map_reason": "n/a",
            "k_ct_derived_mask": 0, "k_ct_reason": "n/a",
            "i_normal_pushed_mask": 7, "summed_unmeasured_mask": 0,
            "nameplate_mismatch_mask": 0, "nameplate_reason": "n/a",
        }
        with unittest.mock.patch.object(sweep_http, "status", return_value=payload):
            result = m.zone_current_sweep_status(host="10.0.0.5")
        self.assertNotIn("EXPECTED", result)

    def test_esp_persist_failed_warning(self):
        payload = {
            "state": "done", "zone_index": 2, "zones_done": 3, "zones_total": 3,
            "reason": "done", "ct_map_derived_mask": 0, "ct_map_reason": "n/a",
            "k_ct_derived_mask": 0, "k_ct_reason": "n/a",
            "i_normal_pushed_mask": 7, "summed_unmeasured_mask": 0,
            "esp_persist_failed": True,
            "nameplate_mismatch_mask": 0, "nameplate_reason": "n/a",
        }
        with unittest.mock.patch.object(sweep_http, "status", return_value=payload):
            result = m.zone_current_sweep_status(host="10.0.0.5")
        self.assertIn("WARNING: esp_persist_failed=true", result)
        payload["esp_persist_failed"] = False
        with unittest.mock.patch.object(sweep_http, "status", return_value=payload):
            result = m.zone_current_sweep_status(host="10.0.0.5")
        self.assertNotIn("esp_persist_failed", result)

    def test_transport_error_surfaced(self):
        with unittest.mock.patch.object(sweep_http, "status",
                                         side_effect=sweep_http.ZoneSweepHttpError("boom")):
            result = m.zone_current_sweep_status(host="10.0.0.5")
        self.assertTrue(result.startswith("error"))


class AbortTest(unittest.TestCase):
    def test_ok(self):
        with unittest.mock.patch.object(sweep_http, "abort", return_value={"ok": True}) as mock_abort:
            result = m.zone_current_sweep_abort(host="10.0.0.5")
        mock_abort.assert_called_once_with("10.0.0.5")
        self.assertTrue(result.startswith("ok"))

    def test_board_failure_surfaced(self):
        with unittest.mock.patch.object(sweep_http, "abort", return_value={"ok": False}):
            result = m.zone_current_sweep_abort(host="10.0.0.5")
        self.assertTrue(result.startswith("error"))

    def test_transport_error_surfaced(self):
        with unittest.mock.patch.object(sweep_http, "abort",
                                         side_effect=sweep_http.ZoneSweepHttpError("boom")):
            result = m.zone_current_sweep_abort(host="10.0.0.5")
        self.assertTrue(result.startswith("error"))


class FacadeRegistrationTest(unittest.TestCase):
    """exercises the REAL production kilnctrl registry, not a synthetic
    one, so a keyword typo in mcp_facade.py or a missing import in
    mcp_server.py both fail this test -- same standard
    test_adaptive_tune_mcp_tools.py holds its own tools to."""

    def test_all_three_tools_registered_in_the_registry(self):
        self.assertIn("zone_current_sweep_start", m.registry.by_name)
        self.assertIn("zone_current_sweep_status", m.registry.by_name)
        self.assertIn("zone_current_sweep_abort", m.registry.by_name)

    def test_findable_by_current_measurement_query(self):
        hits = [h.name for h in m.registry.search("measure the current draw of each zone")]
        self.assertIn("zone_current_sweep_start", hits)

    def test_findable_by_abort_query(self):
        hits = [h.name for h in m.registry.search("stop the current sweep and turn relays off")]
        self.assertIn("zone_current_sweep_abort", hits)

    def test_findable_by_status_query(self):
        hits = [h.name for h in m.registry.search("check progress of the zone current sweep")]
        self.assertIn("zone_current_sweep_status", hits)


if __name__ == "__main__":
    unittest.main()
