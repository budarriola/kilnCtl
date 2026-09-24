#!/usr/bin/env python3
"""Unit tests for the `host=None` resolution fix in
`kilnctrl.mcp_server_bench_test.bench_test_run` and
`kilnctrl.mcp_server_ota_matrix.ota_matrix_run`.

Bug (found on the bench 2026-09-24): both tools passed `host=None` straight
into `ctx["host"]` when the caller omitted `host`, so every HTTP-using case
(`ctx["host"]` used directly as a urlopen target) failed with a
getaddrinfo/DNS error instead of falling back to the board's address the way
`get_heap_status()` does via `mcp_server_ota._ota_resolve_host`.

Both tools now resolve via `mcp_server_ota._ota_resolve_host_with_source`,
which returns `(resolved_host, source)` -- `source` is one of "explicit"/
"STA IP"/"default", and both tools now name it in their report, since the
"default" branch can be a stale/cached address rather than the board
actually on the bench (review finding on commit 6ee73705). Every test here
fakes that resolver (never touching real UART/HTTP) and asserts:
  1. a `None` host is resolved before it reaches
     `ctx["host"]`/`BenchTestRunner`/`_run_ota_matrix`;
  2. an explicit host argument is passed through untouched;
  3. a resolution failure refuses loudly WITHOUT ever reaching the real
     board-touching path (`BenchTestRunner`/`_run_ota_matrix` must not be
     constructed/called);
  4. the report names the resolved host and its source;
  5. `dry_run=True` on `ota_matrix_run` never calls the resolver at all (a
     dry run makes no board contact).

Run with:
    python -m pytest tools/PcTools/tests/test_mcp_server_bench_test_host_resolution.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_bench_test as BT  # noqa: E402
from kilnctrl import mcp_server_ota as _ota_tool  # noqa: E402
from kilnctrl import mcp_server_ota_matrix as M  # noqa: E402


class _FakeOutcome:
    def __init__(self):
        self.run_id = "fake-run"
        self.exit_code = 0
        self.preflight_ok = True
        self.preflight_reason = None
        self.requested = []
        self.results = {}
        self.run_dir = "logs/bench_test/fake-run"


class _FakeRunner:
    """Stands in for BenchTestRunner, capturing the ctx it was built with."""
    captured_ctx = None
    should_be_constructed = True

    def __init__(self, ctx, logs_root=None):
        if not _FakeRunner.should_be_constructed:
            raise AssertionError("BenchTestRunner must not be constructed after a host-resolution refusal")
        _FakeRunner.captured_ctx = ctx

    def run(self, suite, cases, dry_run, allow_heat, tag):
        return _FakeOutcome()


class BenchTestRunHostResolutionTest(unittest.TestCase):
    def setUp(self):
        _FakeRunner.captured_ctx = None
        _FakeRunner.should_be_constructed = True

    def test_none_host_is_resolved_before_reaching_ctx(self):
        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source",
                                return_value=("192.168.1.87", "STA IP")) as fake_resolve, \
             mock.patch.object(BT, "BenchTestRunner", _FakeRunner):
            result = BT.bench_test_run(suite="smoke", dry_run=True, host=None)
        fake_resolve.assert_called_once_with(None)
        self.assertIsNotNone(_FakeRunner.captured_ctx)
        self.assertEqual(_FakeRunner.captured_ctx["host"], "192.168.1.87")
        self.assertNotIn("error:", result)

    def test_explicit_host_is_passed_through_untouched(self):
        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source",
                                side_effect=lambda h: (h, "explicit")) as fake_resolve, \
             mock.patch.object(BT, "BenchTestRunner", _FakeRunner):
            BT.bench_test_run(suite="smoke", dry_run=True, host="10.0.0.5")
        fake_resolve.assert_called_once_with("10.0.0.5")
        self.assertEqual(_FakeRunner.captured_ctx["host"], "10.0.0.5")

    def test_resolution_failure_refuses_loudly_and_never_reaches_runner(self):
        _FakeRunner.should_be_constructed = False
        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source",
                                return_value=("", "default")), \
             mock.patch.object(BT, "BenchTestRunner", _FakeRunner):
            result = BT.bench_test_run(suite="smoke", dry_run=True, host=None)
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("could not resolve a board host", result)
        self.assertIsNone(_FakeRunner.captured_ctx)

    def test_report_names_resolved_host_and_source(self):
        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source",
                                return_value=("192.168.1.87", "STA IP")), \
             mock.patch.object(BT, "BenchTestRunner", _FakeRunner):
            result = BT.bench_test_run(suite="smoke", dry_run=True, host=None)
        self.assertIn("host: 192.168.1.87 (STA IP)", result)


class OtaMatrixRunHostResolutionTest(unittest.TestCase):
    def setUp(self):
        _FakeRunner.captured_ctx = None
        _FakeRunner.should_be_constructed = True

    def test_none_host_is_resolved_before_reaching_ctx(self):
        captured = {}

        def fake_run(ctx, cases, tag, allow_heat, logs_root=None):
            captured["ctx"] = ctx
            return "ok"

        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source",
                                return_value=("192.168.1.87", "STA IP")) as fake_resolve, \
             mock.patch.object(M, "_run_ota_matrix", side_effect=fake_run):
            result = M.ota_matrix_run(confirm=True, host=None)
        fake_resolve.assert_called_once_with(None)
        self.assertEqual(captured["ctx"]["host"], "192.168.1.87")
        self.assertEqual(result, "ok")

    def test_explicit_host_is_passed_through_untouched(self):
        captured = {}

        def fake_run(ctx, cases, tag, allow_heat, logs_root=None):
            captured["ctx"] = ctx
            return "ok"

        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source",
                                side_effect=lambda h: (h, "explicit")) as fake_resolve, \
             mock.patch.object(M, "_run_ota_matrix", side_effect=fake_run):
            M.ota_matrix_run(confirm=True, host="10.0.0.5")
        fake_resolve.assert_called_once_with("10.0.0.5")
        self.assertEqual(captured["ctx"]["host"], "10.0.0.5")

    def test_resolution_failure_refuses_loudly_and_never_calls_run_ota_matrix(self):
        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source",
                                return_value=("", "default")), \
             mock.patch.object(M, "_run_ota_matrix") as fake_run:
            result = M.ota_matrix_run(confirm=True, host=None)
        fake_run.assert_not_called()
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("could not resolve a board host", result)

    def test_report_names_resolved_host_and_source(self):
        # Exercise the real _run_ota_matrix report-building path (not a
        # fake `_run_ota_matrix`), with the run-level preflight and
        # BenchTestRunner faked so this stays board-free -- the same seam
        # test_mcp_server_ota_matrix.py uses.
        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source",
                                return_value=("192.168.1.87", "STA IP")), \
             mock.patch.object(M, "_run_level_preflight", return_value=None), \
             mock.patch.object(M, "BenchTestRunner", _FakeRunner):
            result = M.ota_matrix_run(confirm=True, host=None)
        self.assertIn("host: 192.168.1.87 (STA IP)", result)

    def test_dry_run_never_calls_the_resolver(self):
        with mock.patch.object(_ota_tool, "_ota_resolve_host_with_source") as fake_resolve:
            M.ota_matrix_run(dry_run=True)
        fake_resolve.assert_not_called()


if __name__ == "__main__":
    unittest.main()
