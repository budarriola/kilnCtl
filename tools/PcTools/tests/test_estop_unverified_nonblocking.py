#!/usr/bin/env python3
"""estop_verified not_done blocks HEAT only (owner decision 2026-10-10).
recovery_mode / safety_trip / crash_report still block everything.

Run with: python -m pytest tools/PcTools/tests/test_estop_unverified_nonblocking.py -q
"""
from __future__ import annotations

import dataclasses
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import capability_preflight as cp  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test.cases_heat import _capability_preflight_ok  # noqa: E402
from kilnctrl.bench_test.runner import BenchTestRunner  # noqa: E402
import test_bench_test_runner as T  # noqa: E402


def _readiness(**status):
    keys = ("recovery_mode", "safety_trip", "crash_report", "estop_verified")
    return {"items": [{"key": k, "label": k, "status": status.get(k, "ok"), "detail": "d"}
                      for k in keys]}


def _board_info(readiness):
    def fake_get_json(host, path, timeout):
        if path == "/api/readiness":
            return readiness, ""
        if path == "/api/crash_report":
            return {"present": False}, ""
        return {"fw_version": "1", "fw_build": "b"}, ""
    with mock.patch.object(cp, "_get_json", fake_get_json):
        return cp.get_board_info("h")


class PreflightSplitTest(unittest.TestCase):
    def test_estop_alone_is_heat_only(self):
        b = _board_info(_readiness(estop_verified="not_done"))
        self.assertEqual(b.readiness_blocked, ())
        self.assertEqual([k for k, _l, _d in b.heat_blocked], ["estop_verified"])
        rep = cp.PreflightReport("p", "h", b)
        self.assertTrue(rep.ok)
        self.assertFalse(rep.ok_for_heat)
        self.assertIn("E-stop not physically verified: heat cases skipped", rep.describe())

    def test_other_three_still_block(self):
        for key in ("recovery_mode", "safety_trip", "crash_report"):
            b = _board_info(_readiness(**{key: "not_done"}))
            rep = cp.PreflightReport("p", "h", b)
            self.assertFalse(rep.ok, key)
            self.assertEqual([k for k, _l, _d in b.readiness_blocked], [key])

    def test_heat_case_preflight_refuses_with_reason(self):
        rep = cp.PreflightReport("p", "h", _board_info(_readiness(estop_verified="not_done")))
        ok, why = _capability_preflight_ok({"capability_preflight_run": lambda p, h: rep, "host": "h"})
        self.assertFalse(ok)
        self.assertIn("estop_unverified", why)


class _Board:
    def __init__(self, heat_blocked):
        self.crash_unacknowledged = False
        self.crash_summary = None
        self.readiness_blocked = ()
        self.heat_blocked = heat_blocked
        self.reachable = True
        self.error = ""


class _Rep:
    ok = True

    def __init__(self, heat_blocked):
        self.board = _Board(heat_blocked)

    def describe(self):
        return "fake"


class RunnerEstopTest(unittest.TestCase):
    setUp = T.RunnerLifecycleTest.setUp
    tearDown = T.RunnerLifecycleTest.tearDown
    _patch_judge = T.RunnerLifecycleTest._patch_judge

    def setUp(self):  # noqa: F811
        T.RunnerLifecycleTest.setUp(self)
        self.cp_report = _Rep((("estop_verified", "E-stop", "not verified"),))

    def test_heat_case_skips_nonheat_runs(self):
        for cid in ("ST-05", "AX-T01"):
            self._saved_specs.setdefault(cid, R.REGISTRY[cid])
        self._patch_judge("ST-05", lambda ctx: R.CaseResult(R.Verdict.PASS))
        called = []
        self._patch_judge("AX-T01", lambda ctx: called.append(1) or R.CaseResult(R.Verdict.PASS))
        self.assertTrue(R.REGISTRY["AX-T01"].heat)
        runner = BenchTestRunner(self.ctx, logs_root=self.tmpdir)
        # Use the smoke suite for the non-heat case, and run the heat case via
        # whichever suite contains it.
        out = runner.run(suite="smoke", cases=["ST-05"])
        self.assertTrue(out.preflight_ok)
        self.assertEqual(out.results["ST-05"].verdict, R.Verdict.PASS)
        suite = next(s for s, ids in R.SUITES.items() if "AX-T01" in ids)
        out = BenchTestRunner(self.ctx, logs_root=self.tmpdir).run(suite=suite, cases=["AX-T01"])
        res = out.results["AX-T01"]
        self.assertEqual(res.verdict, R.Verdict.SKIP)
        self.assertEqual(res.reason, "estop_unverified")
        self.assertEqual(called, [])
        self.assertFalse(self.ctx["allow_heat"])

    def test_verified_estop_runs_heat_case(self):
        self.cp_report = _Rep(())
        called = []
        self._patch_judge("AX-T01", lambda ctx: called.append(1) or R.CaseResult(R.Verdict.PASS))
        suite = next(s for s, ids in R.SUITES.items() if "AX-T01" in ids)
        BenchTestRunner(self.ctx, logs_root=self.tmpdir).run(suite=suite, cases=["AX-T01"], allow_heat=True)
        self.assertEqual(called, [1])


if __name__ == "__main__":
    unittest.main()
