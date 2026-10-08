#!/usr/bin/env python3
"""Unit tests for latency_soak_stats / latency_soak -- no board, no socket.

Run with: python -m pytest tools/PcTools/tests/test_latency_soak_stats.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import latency_soak as ls  # noqa: E402
from kilnctrl.latency_soak_stats import (  # noqa: E402
    Sample, find_clustered_ticks, format_report, merge_episodes, percentile, summarize)


def S(tick, surface, ms, ok=True, status=200):
    return Sample(tick, surface, ms, ok, status)


class PercentileTest(unittest.TestCase):
    def test_empty_single_and_interpolation(self):
        self.assertIsNone(percentile([], 50))
        self.assertEqual(percentile([7.0], 95), 7.0)
        self.assertEqual(percentile([10, 20, 30, 40], 50), 25.0)
        self.assertEqual(percentile([0, 100], 95), 95.0)
        self.assertEqual(percentile(range(1, 101), 100), 100)

    def test_bad_pct(self):
        with self.assertRaises(ValueError):
            percentile([1, 2], 101)


class SummarizeTest(unittest.TestCase):
    def test_per_surface_stats_and_errors(self):
        samples = [S(i, "a", 10 + i) for i in range(5)] + [S(0, "b", 500, ok=False, status=401)]
        st = summarize(samples)
        self.assertEqual(st["a"].n, 5)
        self.assertEqual(st["a"].p50_ms, 12)
        self.assertEqual(st["a"].max_ms, 14)
        self.assertEqual(st["a"].errors, 0)
        self.assertEqual((st["b"].n, st["b"].errors, st["b"].max_ms), (1, 1, 500))


class ClusterTest(unittest.TestCase):
    def test_cluster_needs_n_distinct_surfaces_in_same_tick(self):
        samples = [
            S(0, "a", 50), S(0, "b", 50), S(0, "c", 50),          # quiet
            S(1, "a", 2000), S(1, "b", 1500), S(1, "c", 50),      # 2 stalled
            S(2, "a", 2000), S(2, "b", 1500), S(2, "c", 3000),    # 3 stalled
            S(3, "a", 2000), S(3, "a", 2500), S(3, "b", 50),      # one surface twice
        ]
        got = find_clustered_ticks(samples, 1000, 3)
        self.assertEqual([c.tick for c in got], [2])
        self.assertEqual(got[0].stalled, ("a", "b", "c"))
        self.assertEqual(got[0].worst_ms, 3000)
        self.assertEqual([c.tick for c in find_clustered_ticks(samples, 1000, 2)], [1, 2])

    def test_threshold_is_strictly_greater(self):
        samples = [S(0, "a", 1000), S(0, "b", 1000)]
        self.assertEqual(find_clustered_ticks(samples, 1000, 2), [])

    def test_no_answer_counts_as_stall_but_fast_http_error_does_not(self):
        samples = [Sample(0, "a", 20, False, None, "timeout"),
                   Sample(0, "b", 20, False, None, "URLError"),
                   Sample(0, "c", 20, False, 401, "HTTP 401")]
        got = find_clustered_ticks(samples, 1000, 2)
        self.assertEqual(got[0].stalled, ("a", "b"))
        self.assertEqual(find_clustered_ticks(samples, 1000, 3), [])

    def test_min_surfaces_validated(self):
        with self.assertRaises(ValueError):
            find_clustered_ticks([], 1000, 0)

    def test_merge_episodes(self):
        samples = []
        for t in (4, 5, 6, 9):
            samples += [S(t, "a", 2000), S(t, "b", 2000 + t)]
        eps = merge_episodes(find_clustered_ticks(samples, 1000, 2))
        self.assertEqual([(e.first_tick, e.last_tick, e.ticks) for e in eps], [(4, 6, 3), (9, 9, 1)])
        self.assertEqual(eps[0].worst_ms, 2006)


class ReportTest(unittest.TestCase):
    def test_report_lists_surfaces_errors_and_episode(self):
        samples = [S(0, "a", 10), S(0, "b", 10, ok=False, status=401),
                   S(1, "a", 3000), S(1, "b", 3000)]
        text = format_report(samples, 1000, 2, "hdr")
        self.assertIn("hdr", text)
        self.assertIn("first error b: HTTP 401", text)
        self.assertIn("clustered stall ticks: 1 in 1 episode(s)", text)
        self.assertIn("ticks 1-1 (1)", text)


class RunSoakTest(unittest.TestCase):
    def test_polls_every_surface_each_tick_get_only_with_fake_clock(self):
        now = [0.0]
        calls = []

        def fetch(host, path, timeout):
            calls.append((host, path))
            now[0] += 0.01
            return 12.0, 200, ""

        samples = ls.run_soak("h", duration_s=10, interval_s=5, fetch=fetch,
                              clock=lambda: now[0], sleep=lambda s: now.__setitem__(0, now[0] + s))
        n = len(ls.SURFACES)
        self.assertEqual(len(samples), n * len({s.tick for s in samples}))
        self.assertGreaterEqual(len({s.tick for s in samples}), 2)
        self.assertEqual(len(calls), len(samples) + n)  # + one unrecorded warm-up pass
        self.assertTrue(all(p.startswith("/api/") for _, p in calls))

    def test_at_least_one_tick_and_error_status_recorded(self):
        samples = ls.run_soak("h", duration_s=0, fetch=lambda h, p, t: (5.0, 500, "HTTP 500"),
                              clock=lambda: 0.0, sleep=lambda s: None)
        self.assertEqual(len(samples), len(ls.SURFACES))
        self.assertFalse(any(s.ok for s in samples))

    def test_warmup_401_aborts_before_any_tick(self):
        calls = []

        def fetch(h, p, t):
            calls.append(p)
            return (5.0, 401, "HTTP 401") if p == "/api/zones" else (5.0, 200, "")

        with self.assertRaises(ls.SoakAborted):
            ls.run_soak("h", duration_s=60, fetch=fetch, clock=lambda: 0.0, sleep=lambda s: None)
        self.assertEqual(calls[-1], "/api/zones")  # stopped at the first 401, no ticks

    def test_real_fetch_login_failure_is_fast_401_not_stall(self):
        import unittest.mock as m

        def boom(req, timeout):
            raise ls.http_auth.HttpAuthError("POST /api/auth/login was refused")

        with m.patch.object(ls.http_auth, "urlopen", boom):
            lat, status, err = ls.real_fetch("h", "/api/zones", 1.0)
        self.assertEqual(status, 401)
        self.assertNotIn("refused", err)
        from kilnctrl.latency_soak_stats import is_stalled
        self.assertFalse(is_stalled(Sample(0, "zones", lat, False, status, err), 1000.0))

    def test_real_fetch_timeout_is_no_answer(self):
        import socket
        import unittest.mock as m

        def slow(req, timeout):
            raise socket.timeout("timed out")

        with m.patch.object(ls.http_auth, "urlopen", slow):
            _, status, err = ls.real_fetch("h", "/api/status", 1.0)
        self.assertIsNone(status)
        self.assertTrue(err)

    def test_real_fetch_only_issues_get(self):
        seen = []

        class Resp:
            status = 200

            def read(self):
                return b"{}"

            def __enter__(self):
                return self

            def __exit__(self, *a):
                return False

        import unittest.mock as m
        with m.patch.object(ls.http_auth, "urlopen", lambda req, timeout: seen.append(req) or Resp()):
            lat, status, err = ls.real_fetch("h", "/api/status", 1.0)
        self.assertEqual(status, 200)
        self.assertEqual(seen[0].get_method(), "GET")
        self.assertIsNone(seen[0].data)

    def test_surface_table_is_get_api_paths_only(self):
        for name, path in ls.SURFACES:
            self.assertTrue(path.startswith("/api/"), name)
        self.assertEqual(len({n for n, _ in ls.SURFACES}), len(ls.SURFACES))

    def test_every_surface_is_a_registered_get_route(self):
        import re
        table = os.path.join(os.path.dirname(__file__), "..", "..", "..", "firmware", "KilnFW",
                             "App", "drivers", "http", "route_tier_table.h")
        with open(table, encoding="utf-8") as fh:
            gets = set(re.findall(r'ROUTE_TIER\("([^"]+)",\s*HTTP_GET,', fh.read()))
        self.assertGreater(len(gets), 50)
        for name, path in ls.SURFACES:
            self.assertIn(path, gets, name)


class McpSoakToolTest(unittest.TestCase):
    def setUp(self):
        from kilnctrl import mcp_server_latency_soak as T
        self.T = T

    def test_bad_args_refused_before_any_request(self):
        import unittest.mock as m
        with m.patch.object(self.T._ls, "run_soak", side_effect=AssertionError("ran")):
            self.assertTrue(self.T.latency_soak(duration_s=10, interval_s=0, host="h").startswith("error"))
            self.assertTrue(self.T.latency_soak(duration_s=10, min_surfaces=0, host="h").startswith("error"))
            self.assertTrue(self.T.latency_soak(duration_s=300, host="h").startswith("error"))
            self.assertTrue(self.T.latency_soak_start(duration_s=1e9, host="h").startswith("error"))

    def test_all_failed_run_reports_error_so_job_reads_failed(self):
        import unittest.mock as m
        dead = [Sample(0, n, 8000.0, False, None, "timeout") for n, _ in ls.SURFACES]
        with m.patch.object(self.T._ls, "run_soak", return_value=dead):
            self.assertTrue(self.T._run("h", 10, 5, 1000, 3).startswith("error: no surface"))
        ok = [Sample(0, n, 8.0, True, 200) for n, _ in ls.SURFACES]
        with m.patch.object(self.T._ls, "run_soak", return_value=ok):
            self.assertFalse(self.T._run("h", 10, 5, 1000, 3).startswith("error"))

    def test_warmup_abort_reported_as_error(self):
        import unittest.mock as m
        with m.patch.object(self.T._ls, "run_soak", side_effect=ls.SoakAborted("x")):
            self.assertEqual(self.T._run("h", 10, 5, 1000, 3), "error: x")


if __name__ == "__main__":
    unittest.main()
