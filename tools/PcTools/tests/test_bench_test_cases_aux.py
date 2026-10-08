#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_aux (AX suite). Fake board only.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_aux.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_aux as C  # noqa: E402
from kilnctrl.bench_test.registry import REGISTRY, SUITES, Verdict  # noqa: E402

IDS = ["AX-C01", "AX-C02", "AX-C03", "AX-T01", "AX-K01", "AX-T02", "AX-R01"]


def _aux_text(mask, r4_enabled, tc=0, r1_enabled=False):
    def line(n, en, tcz):
        return (f"relay {n}: {'ENABLED' if en else 'disabled'}, tc_zone={'none' if tcz == -1 else tcz}, "
                f"hyst_c=1.0, min_on_s=10, min_off_s=10")
    return (f"aux outputs (host=h): quarantined=False, enabled_mask={mask}, conflict_mask=0, \n"
            + "\n".join([line(1, r1_enabled, -1), line(2, False, -1), line(3, False, -1),
                          line(4, r4_enabled, tc if r4_enabled else -1)]))


class FakeSrv:
    """Minimal fake of the MCP functions cases_aux calls."""

    def __init__(self):
        self.r4 = False
        self.r1 = False
        self.relay4_shadow = False
        self.saved = {}
        self.calls = []
        self.exec_idle = True
        self.refuse_r1 = True
        self.trip_reason = 0
        self.delete_fails = False

    def mask(self):
        return (8 if self.r4 else 0) | (1 if self.r1 else 0)

    def control_get_aux_outputs(self):
        return _aux_text(self.mask(), self.r4, r1_enabled=self.r1)

    def control_set_aux_output(self, relay, enabled, tc_zone=None, confirm=False, **kw):
        self.calls.append(("set_aux", relay, enabled, confirm))
        if relay == 1 and enabled:
            if self.refuse_r1:
                return "refused: POST /api/aux_outputs refused (HTTP 400): relay in zone relay_mask"
            self.r1 = True
            return "ok - relay 1 ENABLED"
        if relay == 1:
            self.r1 = False
            return "ok - relay 1 disabled"
        self.r4 = enabled
        return "ok - relay 4"

    def thermo_read(self, ch=0):
        return "zone 0: 21.5 C"

    def profile_save_bench_aux_rule(self, **kw):
        self.saved[7] = kw
        return "ok - saved BENCH_AUX_RULE; profile id 7 (confirmed by read-back)"

    def profiles_start(self, pid):
        self.calls.append(("start", pid))
        self.exec_idle = False
        self.relay4_shadow = True
        return "ok"

    def profiles_stop(self):
        self.calls.append(("stop",))
        self.exec_idle = True
        self.relay4_shadow = False
        return "ok"

    def profiles_pause(self):
        self.calls.append(("pause",))
        return "ok"

    def profiles_resume(self):
        self.calls.append(("resume",))
        return "ok"

    def profiles_delete(self, pid):
        self.calls.append(("delete", pid))
        if self.delete_fails:
            return "error: busy"
        self.saved.pop(pid, None)
        return "ok"

    def safety_get_diag(self):
        return f"trip_reason: {self.trip_reason}\ntrip_mask: 0x{(1 << (self.trip_reason - 1)) if self.trip_reason else 0:04x}"

    def safety_clear_trip(self):
        self.trip_reason = 0
        return "ok"


def _ctx(srv, **kw):
    ctx = {"srv": srv, "aux_confirm": True, "allow_heat": True, "sleep_fn": lambda s: None,
           "aux_window_s": 6.0, "aux_idle_fn": lambda: srv.exec_idle,
           "aux_relay_fn": lambda r: srv.relay4_shadow}
    ctx.update(kw)
    return ctx


class WiringTest(unittest.TestCase):
    def test_registered_and_judged(self):
        for cid in IDS:
            self.assertIn(cid, REGISTRY)
            self.assertIsNotNone(REGISTRY[cid].judge, cid)
        self.assertEqual(SUITES["aux"], IDS)
        self.assertEqual(SUITES["full"][-2:].count("AX-R01"), 1 if "AX-R01" in SUITES["full"][-2:] else 0)

    def test_heat_flags(self):
        self.assertEqual({c for c in IDS if REGISTRY[c].heat}, {"AX-T01", "AX-K01", "AX-T02"})


class GateTest(unittest.TestCase):
    def test_no_confirm_skips_every_writer_and_touches_nothing(self):
        srv = FakeSrv()
        for cid in IDS[:-1]:
            ctx = _ctx(srv, aux_confirm=False, attended=True)
            self.assertEqual(REGISTRY[cid].judge(ctx).verdict, Verdict.SKIP, cid)
        self.assertEqual(srv.calls, [])

    def test_confirm_must_be_exactly_true(self):
        srv = FakeSrv()
        self.assertEqual(C._case_ax_c01(_ctx(srv, aux_confirm=1)).verdict, Verdict.SKIP)

    def test_env_opt_in(self):
        old = os.environ.get(C.CONFIRM_ENV)
        os.environ[C.CONFIRM_ENV] = "1"
        try:
            self.assertTrue(C._confirmed({}))
        finally:
            if old is None:
                del os.environ[C.CONFIRM_ENV]
            else:
                os.environ[C.CONFIRM_ENV] = old
        self.assertFalse(C._confirmed({}))

    def test_heat_needs_allow_heat(self):
        srv = FakeSrv()
        r = C._case_ax_t01(_ctx(srv, allow_heat=False))
        self.assertEqual(r.verdict, Verdict.SKIP)

    def test_tainted_skips(self):
        srv = FakeSrv()
        self.assertEqual(C._case_ax_c01(_ctx(srv, _tainted=True)).verdict, Verdict.SKIP)


class ConfigureAndConflictTest(unittest.TestCase):
    def test_c01_pass_records_original(self):
        srv = FakeSrv()
        ctx = _ctx(srv)
        self.assertEqual(C._case_ax_c01(ctx).verdict, Verdict.PASS)
        self.assertEqual(ctx["_aux_orig"]["enabled_mask"], 0)
        self.assertTrue(srv.r4)

    def test_c02_refused_passes(self):
        srv = FakeSrv()
        self.assertEqual(C._case_ax_c02(_ctx(srv)).verdict, Verdict.PASS)

    def test_c02_accepted_write_fails_and_is_undone(self):
        srv = FakeSrv()
        srv.refuse_r1 = False
        ctx = _ctx(srv)
        r = C._case_ax_c02(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertFalse(srv.r1)
        self.assertNotIn("_tainted", ctx)

    def test_c03_skips_without_writer_and_judges_status(self):
        srv = FakeSrv()
        self.assertEqual(C._case_ax_c03(_ctx(srv)).verdict, Verdict.SKIP)
        self.assertEqual(C._case_ax_c03(_ctx(srv, aux_zone_mask_post_fn=lambda: (400, {}))).verdict, Verdict.PASS)
        self.assertEqual(C._case_ax_c03(_ctx(srv, aux_zone_mask_post_fn=lambda: (200, {}))).verdict, Verdict.FAIL)


class RuleCasesTest(unittest.TestCase):
    def _configured(self):
        srv = FakeSrv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        return srv, ctx

    def test_t01_pass_and_teardown(self):
        srv, ctx = self._configured()
        r = C._case_ax_t01(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertIn(("stop",), srv.calls)
        self.assertIn(("delete", 7), srv.calls)
        self.assertEqual(ctx["_aux_profile_ids"], [])

    def test_t01_relay_never_on_fails(self):
        srv, ctx = self._configured()
        ctx["aux_relay_fn"] = lambda r: False
        self.assertEqual(C._case_ax_t01(ctx).verdict, Verdict.FAIL)

    def test_t01_unconfirmed_stop_taints(self):
        srv, ctx = self._configured()
        ctx["aux_idle_fn"] = lambda: False
        r = C._case_ax_t01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(ctx["_tainted"])

    def test_t01_requires_bound_aux(self):
        srv = FakeSrv()
        self.assertEqual(C._case_ax_t01(_ctx(srv)).verdict, Verdict.SKIP)

    def test_k01_pass_when_held_while_paused(self):
        srv, ctx = self._configured()
        r = C._case_ax_k01(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertIn(("pause",), srv.calls)

    def test_k01_fails_when_paused_drops(self):
        srv, ctx = self._configured()
        states = iter([True, False])
        ctx["aux_relay_fn"] = lambda r: next(states)
        r = C._case_ax_k01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("PAUSED", r.reason)

    def test_judge_min_on_off(self):
        ok = [(0, True), (2, True), (30, False), (60, True)]
        self.assertEqual(C.judge_rule_samples(ok, 10, 10).verdict, Verdict.PASS)
        bad = [(0, True), (2, False), (4, True), (6, False)]
        self.assertEqual(C.judge_rule_samples(bad, 10, 10).verdict, Verdict.FAIL)


class TripTest(unittest.TestCase):
    def _run(self, drop=True, reason=6, mask_ok=True):
        srv = FakeSrv()
        ctx = _ctx(srv, attended=True)
        C._case_ax_c01(ctx)

        def prompt(q, t):
            srv.trip_reason = reason
            if drop:
                srv.relay4_shadow = False
            return True
        ctx["operator_prompt_fn"] = prompt
        return srv, ctx, C._case_ax_t02(ctx)

    def test_unattended_skips(self):
        srv = FakeSrv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        self.assertEqual(C._case_ax_t02(ctx).verdict, Verdict.SKIP)

    def test_pass_and_cleared(self):
        srv, ctx, r = self._run()
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(srv.trip_reason, 0)

    def test_relay_did_not_drop_fails(self):
        srv, ctx, r = self._run(drop=False)
        self.assertEqual(r.verdict, Verdict.FAIL)


class RestoreTest(unittest.TestCase):
    def test_restores_to_original_mask(self):
        srv = FakeSrv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        r = C._case_ax_r01(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertFalse(srv.r4)

    def test_failed_restore_taints(self):
        srv = FakeSrv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        srv.control_set_aux_output = lambda **kw: "refused: busy"
        r = C._case_ax_r01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(ctx["_tainted"])

    def test_skips_when_c01_did_nothing(self):
        self.assertEqual(C._case_ax_r01(_ctx(FakeSrv())).verdict, Verdict.SKIP)


if __name__ == "__main__":
    unittest.main()
