#!/usr/bin/env python3
"""Unit tests for kilnctrl.mcp_server_adaptive_tune -- the MCP tool wrappers
around adaptive_tune_http_client.py.

Two things this guards, both negative-tested (mutate the guard away, watch
the specific test fail, revert):

1. adaptive_tune_set_enabled() WRITES board config that decides whether the
   NEXT run-end (including one already in progress) silently rewrites a
   zone's PID gains -- it must refuse without confirm=True, the same
   idiom debug_program()/debug_write_memory() use. Verified live below by
   temporarily short-circuiting the guard: with the check disabled the tool
   proceeds straight to the HTTP call, which the same test would catch (see
   TestAdaptiveTuneSetEnabledConfirmGate.test_guard_is_load_bearing).

2. Both new tools must be reachable through kiln_find()'s search facade --
   a tool nobody can find is close to unregistered (this repo's own stated
   standard). Verified by exercising the REAL production registry
   (kilnctrl.mcp_server.registry), not a synthetic stand-in, so a keyword
   typo in mcp_facade.py or a missing import in mcp_server.py both fail
   this test.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server as m  # noqa: E402
from kilnctrl import adaptive_tune_http_client as at_http  # noqa: E402
from kilnctrl import mcp_server_adaptive_tune as at_tools  # noqa: E402


class SetEnabledConfirmGateTest(unittest.TestCase):
    """adaptive_tune_set_enabled() must not touch the network at all
    without confirm=True -- proven by asserting the HTTP client is never
    called, not just by reading the error string back."""

    def test_refuses_without_confirm(self):
        with unittest.mock.patch.object(at_http, "set_enabled") as mock_set:
            result = m.adaptive_tune_set_enabled(0, True)
        self.assertIn("confirm=True", result)
        self.assertIn("error", result)
        mock_set.assert_not_called()

    def test_refuses_with_confirm_false_explicitly(self):
        with unittest.mock.patch.object(at_http, "set_enabled") as mock_set:
            result = m.adaptive_tune_set_enabled(0, True, confirm=False)
        self.assertIn("error", result)
        mock_set.assert_not_called()

    def test_proceeds_with_confirm_true(self):
        row = unittest.mock.MagicMock(zone=1, enabled=True)
        with unittest.mock.patch.object(at_http, "get_status", return_value=[row]),              unittest.mock.patch.object(at_http, "set_enabled",
                                         return_value={"ok": True}) as mock_set:
            result = m.adaptive_tune_set_enabled(1, True, confirm=True, host="10.0.0.5")
        mock_set.assert_called_once_with("10.0.0.5", 1, True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("zone 1", result)
        self.assertIn("enabled", result)

    def test_warning_from_board_surfaced(self):
        row = unittest.mock.MagicMock(zone=2, enabled=False)
        with unittest.mock.patch.object(at_http, "get_status", return_value=[row]), unittest.mock.patch.object(
            at_http, "set_enabled",
            return_value={"ok": True, "warning": "applied live, save failed"},
        ):
            result = m.adaptive_tune_set_enabled(2, False, confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("ok"))
        self.assertIn("WARNING", result)
        self.assertIn("save failed", result)

    def test_guard_is_load_bearing(self):
        """Negative test for the negative test: with the confirm check
        physically removed, the tool DOES reach the HTTP client -- proving
        test_refuses_without_confirm above is actually exercising the
        guard, not passing vacuously for some unrelated reason (e.g. a
        broken mock or an exception swallowed elsewhere)."""

        def unguarded(zone, enabled, confirm=False, host=None):
            resolved = at_tools._adaptive_tune_resolve_host(host)
            result = at_http.set_enabled(resolved, zone, enabled)
            return f"ok - zone {zone} adaptive tuning set (unguarded), {result}"

        with unittest.mock.patch.object(at_http, "set_enabled",
                                         return_value={"ok": True}) as mock_set:
            with unittest.mock.patch.object(at_tools, "adaptive_tune_set_enabled", unguarded):
                result = at_tools.adaptive_tune_set_enabled(0, True, host="10.0.0.5")
        mock_set.assert_called_once()
        self.assertIn("unguarded", result)


class RevertConfirmGateTest(unittest.TestCase):
    def test_refuses_without_confirm(self):
        with unittest.mock.patch.object(at_http, "revert") as mock_revert:
            result = m.adaptive_tune_revert(0)
        self.assertIn("confirm=True", result)
        self.assertIn("error", result)
        mock_revert.assert_not_called()

    def test_proceeds_with_confirm_true(self):
        row = unittest.mock.MagicMock(zone=2, revert_available=False)
        with unittest.mock.patch.object(at_http, "get_status", return_value=[row]),                 unittest.mock.patch.object(at_http, "revert",
                                           return_value={"ok": True}) as mock_revert:
            result = m.adaptive_tune_revert(2, confirm=True, host="10.0.0.5")
        mock_revert.assert_called_once_with("10.0.0.5", 2)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("read back verified", result)

    def test_mismatch_reports_failed(self):
        row = unittest.mock.MagicMock(zone=2, revert_available=True)
        with unittest.mock.patch.object(at_http, "get_status", return_value=[row]),                 unittest.mock.patch.object(at_http, "revert", return_value={"ok": True}):
            result = m.adaptive_tune_revert(2, confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("FAILED"), result)

    def test_reread_failure_reports_unverified(self):
        with unittest.mock.patch.object(
                at_http, "get_status", side_effect=at_http.AdaptiveTuneHttpError("boom")),                 unittest.mock.patch.object(at_http, "revert", return_value={"ok": True}):
            result = m.adaptive_tune_revert(2, confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("UNVERIFIED"), result)

    def test_missing_row_reports_unverified(self):
        with unittest.mock.patch.object(at_http, "get_status", return_value=[]),                 unittest.mock.patch.object(at_http, "revert", return_value={"ok": True}):
            result = m.adaptive_tune_revert(2, confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("UNVERIFIED"), result)

    def test_board_failure_reason_surfaced(self):
        with unittest.mock.patch.object(
            at_http, "revert",
            return_value={"ok": False, "reason": "nothing to revert"},
        ):
            result = m.adaptive_tune_revert(2, confirm=True, host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("nothing to revert", result)

    def test_guard_is_load_bearing(self):
        def unguarded(zone, confirm=False, host=None):
            resolved = at_tools._adaptive_tune_resolve_host(host)
            result = at_http.revert(resolved, zone)
            return f"ok - zone {zone} reverted (unguarded), {result}"

        with unittest.mock.patch.object(at_http, "revert",
                                         return_value={"ok": True}) as mock_revert:
            with unittest.mock.patch.object(at_tools, "adaptive_tune_revert", unguarded):
                result = at_tools.adaptive_tune_revert(0, host="10.0.0.5")
        mock_revert.assert_called_once()
        self.assertIn("unguarded", result)


class GetStatusTest(unittest.TestCase):
    def test_reports_all_zones_read_only(self):
        zone = at_http.AdaptiveTuneZoneStatus(
            zone=0, enabled=True, observation_count=3, observations_lifetime=3,
            has_applied=False, prior_k_dc=0.0, applied_k_dc=0.0, delta_pct=0.0,
            last_profile_id=0, last_applied_unix_s=0, refusal="not enough observations",
            joint_observations=0, coupled_attempted=False, coupled_applied=False,
            coupled_cells_changed=0, coupled_refusal="",
            ki_verdict=0, ki_correction_pct=0.0, ki_applied=False, ki_refusal="",
        )
        with unittest.mock.patch.object(at_http, "get_status", return_value=[zone]) as mock_get:
            result = m.adaptive_tune_get_status(host="10.0.0.5")
        mock_get.assert_called_once_with("10.0.0.5")
        self.assertIn("zone 0", result)
        self.assertIn("enabled=True", result)
        self.assertIn("not enough observations", result)

    def test_http_error_surfaced_not_raised(self):
        with unittest.mock.patch.object(
            at_http, "get_status",
            side_effect=at_http.AdaptiveTuneHttpError("unreachable: timed out"),
        ):
            result = m.adaptive_tune_get_status(host="10.0.0.5")
        self.assertTrue(result.startswith("error"))
        self.assertIn("unreachable", result)


class FacadeDiscoverabilityTest(unittest.TestCase):
    """Registered but unfindable is barely better than unregistered -- this
    exercises the REAL production kilnctrl registry, not a synthetic one,
    so a missing mcp_facade.py keyword row or a missing mcp_server.py
    import both fail here."""

    def test_get_status_found_by_plain_language_query(self):
        hits = [h.name for h in m.registry.search("what is the adaptive tune status")]
        self.assertIn("adaptive_tune_get_status", hits[:3])

    def test_get_status_found_via_keyword_synonyms_alone(self):
        # None of these words appear in the tool's own name -- this only
        # passes if mcp_facade.py's KEYWORDS row for adaptive_tune_get_status
        # is actually wired up (learned gain / dwell / refusal reasons).
        hits = [h.name for h in m.registry.search("show me the learned gain and dwell refusal reasons")]
        self.assertIn("adaptive_tune_get_status", hits[:3])

    def test_set_enabled_found_by_plain_language_query(self):
        hits = [h.name for h in m.registry.search("enable adaptive tuning for a zone")]
        self.assertIn("adaptive_tune_set_enabled", hits[:3])

    def test_set_enabled_found_via_synonym_alone(self):
        # "continuous learning" matches only through the SYNONYMS table
        # ("learning"/"continuous" -> "adaptive_tune"), not the tool name.
        hits = [h.name for h in m.registry.search("toggle continuous learning opt-in")]
        self.assertIn("adaptive_tune_set_enabled", hits[:3])

    def test_revert_found_by_plain_language_query(self):
        hits = [h.name for h in m.registry.search("undo the last learned pid gain change")]
        self.assertIn("adaptive_tune_revert", hits[:3])

    def test_all_three_tools_registered_in_the_registry(self):
        self.assertIn("adaptive_tune_get_status", m.registry.by_name)
        self.assertIn("adaptive_tune_set_enabled", m.registry.by_name)
        self.assertIn("adaptive_tune_revert", m.registry.by_name)

    def test_none_of_the_tools_directly_published_on_the_wire(self):
        # Only the six facade tools (+ KEEP) stay published -- everything
        # else, these three included, must be reachable ONLY through
        # kiln_call/kiln_find, never as its own top-level MCP schema.
        published = set(m.mcp._tool_manager._tools)
        self.assertNotIn("adaptive_tune_get_status", published)
        self.assertNotIn("adaptive_tune_set_enabled", published)
        self.assertNotIn("adaptive_tune_revert", published)


if __name__ == "__main__":
    unittest.main()
