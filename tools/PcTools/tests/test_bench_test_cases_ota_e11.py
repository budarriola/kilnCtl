#!/usr/bin/env python3
"""Fake-board tests for OT-E11 (recovery image receives an app image) and
LCD-20 (recovery idle policy, runs only inside OT-E11)."""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_lcd as L  # noqa: E402
from kilnctrl.bench_test import cases_ota as C  # noqa: E402
from kilnctrl.bench_test.registry import CaseResult, Verdict, get_case  # noqa: E402


class _St:
    def __init__(self, s):
        self.state_name = s


class _Prof:
    def __init__(self, s):
        self.s = s

    def get_exec_status(self):
        return _St(self.s)


class _Srv:
    def __init__(self, s="idle"):
        self._profiles = _Prof(s)


class _Part:
    def __init__(self, running):
        self.running = running

    def get_partitions(self, host, **kw):
        return {"running": self.running}


def _ctx(**over):
    calls = []
    c = {
        "host": "h", "srv": _Srv(), "ota_image_path": "/img.bin", "_isfile_fn": lambda p: True,
        "_interlock_fn": lambda: {"ok": True}, "_sleep": lambda s: None, "_sleep_fn": lambda s: None,
        "partition_http_client": _Part("app"),
        "_recovery_enter_fn": lambda: calls.append("enter") or "ok - recovery boot accepted",
        "_recovery_status_fn": lambda: "recovery image (host=h): x",
        "_recovery_push_fn": lambda: calls.append("push") or "ok - pushed",
        "_recovery_exit_fn": lambda: calls.append("exit") or "ok - exit",
        "_lcd20_fn": lambda ctx: CaseResult(Verdict.PASS),
    }
    c.update(over)
    c["_calls"] = calls
    return c


class Ote11Test(unittest.TestCase):
    def test_registered(self):
        self.assertIsNotNone(get_case("OT-E11").judge)
        self.assertIsNotNone(get_case("LCD-20").judge)

    def test_pass_and_lcd20_recorded(self):
        c = _ctx()
        r = C._case_ote11(c)
        self.assertEqual(r.verdict, Verdict.PASS)
        self.assertEqual(c["_calls"], ["enter", "push"])
        self.assertEqual(L._case_lcd20(c).verdict, Verdict.PASS)

    def test_skips(self):
        self.assertEqual(C._case_ote11(_ctx(srv=_Srv("running"))).verdict, Verdict.SKIP)
        c = _ctx(); del c["ota_image_path"]
        self.assertEqual(C._case_ote11(c).verdict, Verdict.SKIP)
        c = _ctx(_interlock_fn=lambda: {"ok": False, "reason": "firing"})
        r = C._case_ote11(c)
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertEqual(c["_calls"], [])

    def test_enter_refused_no_exit(self):
        c = _ctx(_recovery_enter_fn=lambda: "error: 409")
        r = C._case_ote11(c)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertNotIn("exit", c["_calls"])

    def test_push_fail_exits_and_reports_stuck(self):
        c = _ctx(_recovery_push_fn=lambda: "FAILED: x")
        r = C._case_ote11(c)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("exit", c["_calls"])
        self.assertTrue(r.observed["left_in_recovery"])
        self.assertTrue(c["_taint"])

    def test_running_not_app_fails(self):
        c = _ctx(partition_http_client=_Part("recovery"), _recovery_status_fn=lambda: "recovery image")
        self.assertEqual(C._case_ote11(c).verdict, Verdict.FAIL)
        self.assertIn("exit", c["_calls"])

    def test_never_recovery_fails_and_exits(self):
        c = _ctx(_recovery_status_fn=lambda: "error: 404", ote11_recovery_wait_s=4.0)
        self.assertEqual(C._case_ote11(c).verdict, Verdict.FAIL)
        self.assertIn("exit", c["_calls"])

    def test_lcd20_not_run_without_ote11(self):
        self.assertEqual(L._case_lcd20({}).verdict, Verdict.NOT_RUN)


if __name__ == "__main__":
    unittest.main()
