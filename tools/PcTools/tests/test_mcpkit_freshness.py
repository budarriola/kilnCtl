#!/usr/bin/env python3
"""Tests for mcpkit.registry's staleness/freshness tracking.

Why this exists: kiln_help()/kicad_help() are supposed to say plainly when
the running MCP server process is serving code older than what's on disk --
this has twice cost real debugging time (a fix landed on disk, the server
kept answering with the old in-memory code, and the still-present symptom
read as "the fix didn't work"). The mtime comparison is the entire mechanism
behind that warning, so it is tested directly here against a fake clock
rather than real filesystem timing, which would make the test flaky or slow.

Run with: python -m unittest discover -s tools/PcTools/tests
(pytest also collects this file directly, which is how it's normally run.)
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from mcpkit.registry import (  # noqa: E402
    SourceSnapshot,
    check_staleness,
    freshness_line,
)


def _snapshot(files: "dict[str, float]", *, commit: str = "abc123") -> SourceSnapshot:
    return SourceSnapshot(root="/fake/root", started_at=1_000_000.0, commit=commit, files=dict(files))


class CheckStalenessTests(unittest.TestCase):
    """check_staleness() against an injected fake mtime provider -- no real
    filesystem timing involved."""

    def test_fresh_when_no_file_changed(self):
        snap = _snapshot({"/fake/root/a.py": 100.0, "/fake/root/b.py": 200.0})
        clock = {"/fake/root/a.py": 100.0, "/fake/root/b.py": 200.0}
        stale, changed = check_staleness(snap, mtime_provider=clock.get)
        self.assertFalse(stale)
        self.assertEqual(changed, 0)

    def test_stale_when_one_file_mtime_advances(self):
        snap = _snapshot({"/fake/root/a.py": 100.0, "/fake/root/b.py": 200.0})
        clock = {"/fake/root/a.py": 100.0, "/fake/root/b.py": 999.0}  # b.py edited after startup
        stale, changed = check_staleness(snap, mtime_provider=clock.get)
        self.assertTrue(stale)
        self.assertEqual(changed, 1)

    def test_stale_when_a_cached_file_is_deleted(self):
        snap = _snapshot({"/fake/root/a.py": 100.0, "/fake/root/b.py": 200.0})

        def clock(path: str) -> float:
            if path == "/fake/root/b.py":
                raise OSError("no such file")  # deleted since startup
            return 100.0

        stale, changed = check_staleness(snap, mtime_provider=clock)
        self.assertTrue(stale)
        self.assertEqual(changed, 1)

    def test_multiple_changes_are_all_counted(self):
        snap = _snapshot({"/fake/root/a.py": 100.0, "/fake/root/b.py": 200.0, "/fake/root/c.py": 300.0})
        clock = {"/fake/root/a.py": 999.0, "/fake/root/b.py": 200.0, "/fake/root/c.py": 999.0}
        stale, changed = check_staleness(snap, mtime_provider=clock.get)
        self.assertTrue(stale)
        self.assertEqual(changed, 2)

    def test_empty_snapshot_is_fresh(self):
        snap = _snapshot({})
        stale, changed = check_staleness(snap, mtime_provider=lambda p: 0.0)
        self.assertFalse(stale)
        self.assertEqual(changed, 0)


class FreshnessLineTests(unittest.TestCase):
    def test_none_snapshot_yields_empty_line(self):
        self.assertEqual(freshness_line(None), "")

    def test_fresh_line_is_quiet(self):
        snap = _snapshot({"/fake/root/a.py": 100.0}, commit="deadbee")
        clock = {"/fake/root/a.py": 100.0}
        line = freshness_line(snap, mtime_provider=clock.get)
        self.assertNotIn("STALE", line)
        self.assertIn("fresh", line)
        self.assertIn("deadbee", line)

    def test_stale_line_is_loud_and_names_the_count_and_remedy(self):
        snap = _snapshot({"/fake/root/a.py": 100.0, "/fake/root/b.py": 100.0}, commit="deadbee")
        clock = {"/fake/root/a.py": 999.0, "/fake/root/b.py": 999.0}
        line = freshness_line(snap, mtime_provider=clock.get)
        self.assertIn("SERVER CODE IS STALE", line)
        self.assertIn("2 files changed", line)
        self.assertIn("deadbee", line)
        self.assertIn("mcp_servers.ps1 restart", line)


if __name__ == "__main__":
    unittest.main()
