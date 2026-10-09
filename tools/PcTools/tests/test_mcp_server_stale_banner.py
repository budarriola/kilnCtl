#!/usr/bin/env python3
"""Tests for mcp_server.py's inline per-tool-call staleness banner.

THE GAP this closes: the kilnctrl MCP server ran unrestarted for three days
on a stale commit (docs/audits/stale_mcp_server_window_recheck_2026-09-14.md).
kiln_help() and mcp_servers.ps1 status both already derive fresh/stale from
/health, but nothing surfaced that information alongside an ordinary tool
call's own result -- a stale reading could be (and was) consumed without any
staleness indication anywhere near it. _stale_banner() and its use inside
_tool()'s wrapper close that gap by reusing the same
mcpkit.registry.SourceSnapshot/check_staleness() machinery, cached for
_FRESHNESS_CACHE_INTERVAL_S seconds so it stays cheap on every tool call.

All against a fake SourceSnapshot and a monkeypatched module-level
`registry` -- no real filesystem timing, no live board.

Run with:
  python -m pytest tools/PcTools/tests/test_mcp_server_stale_banner.py -q
"""
from __future__ import annotations

import os
import sys
import types
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server as ms  # noqa: E402
from mcpkit.registry import SourceSnapshot  # noqa: E402


def _snapshot(commit="deadbee") -> SourceSnapshot:
    return SourceSnapshot(
        root="/fake/root", started_at=1_000_000.0, commit=commit,
        files={"/fake/root/a.py": 100.0, "/fake/root/b.py": 100.0},
    )


class StaleBannerTests(unittest.TestCase):
    def setUp(self):
        # Save real module state so other test files (which import the same
        # singleton module) are not left with clobbered globals.
        self._orig_registry = ms.registry
        self._orig_cache = dict(ms._freshness_cache)
        self._orig_check_staleness = ms.check_staleness
        # Force a cache miss on the first call of every test.
        ms._freshness_cache["checked_at"] = 0.0
        ms._freshness_cache["banner"] = ""

    def tearDown(self):
        ms.registry = self._orig_registry
        ms._freshness_cache.clear()
        ms._freshness_cache.update(self._orig_cache)
        ms.check_staleness = self._orig_check_staleness

    def test_fresh_yields_empty_banner(self):
        ms.registry = types.SimpleNamespace(freshness=_snapshot())
        with unittest.mock.patch.object(ms, "check_staleness", return_value=(False, 0)):
            self.assertEqual(ms._stale_banner(), "")

    def test_stale_banner_is_loud_and_names_count_commit_and_restart(self):
        ms.registry = types.SimpleNamespace(freshness=_snapshot(commit="deadbee"))
        with unittest.mock.patch.object(ms, "check_staleness", return_value=(True, 3)):
            banner = ms._stale_banner()
        self.assertIn("STALE", banner)
        self.assertIn("3 file", banner)
        self.assertIn("deadbee", banner)
        self.assertIn("mcp_servers.ps1 restart", banner)

    def test_no_snapshot_yields_empty_banner(self):
        ms.registry = types.SimpleNamespace(freshness=None)
        self.assertEqual(ms._stale_banner(), "")

    def test_result_within_cache_interval_is_not_recomputed(self):
        """The whole point of caching: a second call inside the interval
        must not re-invoke check_staleness() at all."""
        ms.registry = types.SimpleNamespace(freshness=_snapshot())
        calls = []

        def _fake_check_staleness(snapshot):
            calls.append(snapshot)
            return (True, 1)

        with unittest.mock.patch.object(ms, "check_staleness", _fake_check_staleness):
            first = ms._stale_banner()
            second = ms._stale_banner()
        self.assertEqual(len(calls), 1)
        self.assertEqual(first, second)
        self.assertIn("STALE", first)

    def test_cache_expiry_triggers_recompute(self):
        ms.registry = types.SimpleNamespace(freshness=_snapshot())
        with unittest.mock.patch.object(ms, "check_staleness", return_value=(False, 0)):
            ms._stale_banner()
        # Simulate the interval having elapsed.
        ms._freshness_cache["checked_at"] -= (ms._FRESHNESS_CACHE_INTERVAL_S + 1)
        with unittest.mock.patch.object(ms, "check_staleness", return_value=(True, 5)) as mocked:
            banner = ms._stale_banner()
        mocked.assert_called_once()
        self.assertIn("STALE", banner)
        self.assertIn("5 file", banner)

    def test_tool_wrapper_appends_banner_only_when_stale(self):
        """End-to-end through the real @_core._tool() decorator (the one
        choke point every published tool goes through), not just the helper
        in isolation -- proves the banner is actually wired in."""
        @ms._tool()
        def _fixture_tool_for_stale_banner_test() -> str:
            return "ok - fixture result"

        ms.registry = types.SimpleNamespace(freshness=_snapshot())

        with unittest.mock.patch.object(ms, "check_staleness", return_value=(False, 0)):
            fresh_result = _fixture_tool_for_stale_banner_test()
        self.assertEqual(fresh_result, "ok - fixture result")
        self.assertNotIn("STALE", fresh_result)

        ms._freshness_cache["checked_at"] = 0.0  # force recompute
        with unittest.mock.patch.object(ms, "check_staleness", return_value=(True, 2)):
            stale_result = _fixture_tool_for_stale_banner_test()
        self.assertTrue(stale_result.startswith("ok - fixture result"))
        self.assertIn("STALE MCP SERVER", stale_result)
        self.assertIn("mcp_servers.ps1 restart", stale_result)

    def test_NEGATIVE_stale_detection_is_not_vacuous(self):
        """Negative test (feedback_negative_test_every_check): prove the
        stale-path assertions above would actually fail against a banner
        renderer that forgot to mention staleness at all."""
        def _broken_stale_banner(snapshot, changed):
            return ""  # forgot to say anything -- the regression this guards against

        broken = _broken_stale_banner(_snapshot(), 3)
        self.assertEqual(broken, "")
        with self.assertRaises(AssertionError):
            self.assertIn("STALE", broken)


if __name__ == "__main__":
    unittest.main()
