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


def _aux_text(mask, r4, r1_enabled=False):
    def line(n, en, tcz, hyst=1.0, on=10, off=10):
        return (f"relay {n}: {'ENABLED' if en else 'disabled'}, tc_zone={'none' if tcz == -1 else tcz}, "
                f"hyst_c={hyst}, min_on_s={on}, min_off_s={off}")
    return (f"aux outputs (host=h): quarantined=False, enabled_mask={mask}, conflict_mask=0, \n"
            + "\n".join([line(1, r1_enabled, -1), line(2, False, -1), line(3, False, -1),
                          line(4, r4["enabled"], r4["tc"], r4["hyst"], r4["on"], r4["off"])]))


class FakeSrv:
    """Minimal fake of the MCP functions cases_aux calls."""

    def __init__(self):
        self.r4e = False
        self.r4t = -1
        self.r4h = 1.0
        self.r4on = 10
        self.r4off = 10
        self.r1 = False
        self.exec_state = "idle"
        self.exec_pid = 0
        self.preexisting_slot = False
        self.start_ok = True
        self.pause_ok = True
        self.restore_drops_hyst = False
        self.set_fail_enable = False
        self.relay4_shadow = False
        self.saved = {}
        self.calls = []
        self.exec_idle = True
        self.refuse_r1 = True
        self.trip_reason = 0
        self.delete_fails = False

    @property
    def r4(self):
        return self.r4e

    def mask(self):
        return (8 if self.r4e else 0) | (1 if self.r1 else 0)

    def control_get_aux_outputs(self):
        return _aux_text(self.mask(), {"enabled": self.r4e, "tc": self.r4t, "hyst": self.r4h,
                                       "on": self.r4on, "off": self.r4off}, r1_enabled=self.r1)

    def profiles_list(self):
        names = ["#7 'BENCH_AUX_RULE' zone_mask=0x1 segments=2"] if (7 in self.saved or self.preexisting_slot) else []
        return "\n".join(names) or "no saved profiles"

    def profiles_get_exec_status(self):
        return f"state={self.exec_state} profile=#{self.exec_pid} 'x' segment=0/2"

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
        if self.set_fail_enable and enabled:
            return "refused: nope"
        self.r4e = enabled
        if tc_zone is not None:
            self.r4t = tc_zone
        elif not enabled:
            self.r4t = -1 if tc_zone is None and False else self.r4t
        if kw.get("hyst_c") is not None and not self.restore_drops_hyst:
            self.r4h = kw["hyst_c"]
        if kw.get("min_on_s") is not None:
            self.r4on = kw["min_on_s"]
        if kw.get("min_off_s") is not None:
            self.r4off = kw["min_off_s"]
        return "ok - relay 4"

    def thermo_read(self, ch=0):
        return "CH0: 21.50 C (CJ 23.00 C)"

    def profile_save_bench_aux_rule(self, **kw):
        self.saved[7] = kw
        self.last_save = kw
        return "ok - saved BENCH_AUX_RULE; profile id 7 (confirmed by read-back)"

    def profiles_start(self, pid):
        self.calls.append(("start", pid))
        self.exec_idle = False
        self.relay4_shadow = True
        if not self.start_ok:
            if getattr(self, "start_lies", False):
                self.exec_state, self.exec_pid = "running", pid
            return "refused: interlock"
        self.exec_state, self.exec_pid = "running", pid
        return "ok"

    def profiles_stop(self):
        self.calls.append(("stop",))
        self.exec_idle = True
        self.relay4_shadow = False
        self.exec_state, self.exec_pid = "idle", 0
        return "ok"

    def profiles_pause(self):
        self.calls.append(("pause",))
        if not self.pause_ok:
            return "refused - nothing running to pause"
        self.exec_state = "paused"
        return "ok"

    def profiles_resume(self):
        self.calls.append(("resume",))
        self.exec_state = "running"
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


def _scripted(srv, seq):
    """Relay shadow that replays ``seq`` then repeats its last value."""
    it = iter(seq)
    last = [seq[-1]]

    def f(r):
        try:
            last[0] = next(it)
        except StopIteration:
            pass
        return last[0]
    return f


def _ctx(srv, **kw):
    ctx = {"srv": srv, "aux_confirm": True, "allow_heat": True, "sleep_fn": lambda s: None,
           "aux_window_s": 6.0, "aux_idle_fn": lambda: srv.exec_idle,
           "aux_relay_fn": lambda r: srv.relay4_shadow,
           "aux_post_fn": lambda relay, en, tc: (400, False)}
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
        self.assertTrue(srv.r4e)

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
        srv.r4on = srv.r4off = 0
        ctx["aux_relay_fn"] = _scripted(srv, [False, True, True, False, True])
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
        ctx["aux_relay_fn"] = _scripted(srv, [False, True, True, False])
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
        self.assertFalse(srv.r4e)

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


class Finding1AmbientTest(unittest.TestCase):
    def test_parses_real_describe_format(self):
        srv = FakeSrv()
        self.assertEqual(C._ambient_c(_ctx(srv)), 21.5)
        srv.thermo_read = lambda ch=0: "CH0: -3.25 C (CJ 20.00 C) [open circuit]"
        self.assertEqual(C._ambient_c(_ctx(srv)), -3.25)

    def test_invalid_is_none_and_cases_inconclusive(self):
        srv = FakeSrv()
        srv.thermo_read = lambda ch=0: "CH0: invalid"
        self.assertIsNone(C._ambient_c(_ctx(srv)))
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        self.assertEqual(C._case_ax_t01(ctx).verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(C._case_ax_k01(ctx).verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(C._case_ax_t02(dict(ctx, attended=True)).verdict, Verdict.INCONCLUSIVE)
        self.assertNotIn(("start", 7), srv.calls)

    def _thresholds(self, case):
        srv = FakeSrv()
        ctx = _ctx(srv, attended=True)
        C._case_ax_c01(ctx)
        ctx["aux_relay_fn"] = _scripted(srv, [False, True, True, True])
        ctx["operator_prompt_fn"] = lambda q, t: False
        case(ctx)
        return srv.last_save

    def test_computed_thresholds(self):
        kw = self._thresholds(C._case_ax_t01)
        self.assertAlmostEqual(kw["threshold_c"], 25.5)
        self.assertAlmostEqual(kw["target_c"], 31.5)
        kw = self._thresholds(C._case_ax_k01)
        self.assertAlmostEqual(kw["threshold_c"], 46.5)
        kw = self._thresholds(C._case_ax_t02)
        self.assertAlmostEqual(kw["threshold_c"], 46.5)


class Finding2StartPauseTest(unittest.TestCase):
    def _cfg(self):
        srv = FakeSrv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        return srv, ctx

    def test_k01_pause_refused_fails_not_pass(self):
        srv, ctx = self._cfg()
        srv.pause_ok = False
        r = C._case_ax_k01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("pause", r.reason)

    def test_k01_start_refused_fails(self):
        srv, ctx = self._cfg()
        srv.start_ok = False
        self.assertEqual(C._case_ax_k01(ctx).verdict, Verdict.FAIL)

    def test_start_refused_text_fails_even_if_state_reads_running(self):
        srv, ctx = self._cfg()
        srv.start_ok = False
        srv.start_lies = True
        r = C._case_ax_k01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_t02_start_refused_fails(self):
        srv, ctx = self._cfg()
        srv.start_ok = False
        self.assertEqual(C._case_ax_t02(dict(ctx, attended=True)).verdict, Verdict.FAIL)

    def test_k01_state_not_running_after_start_fails(self):
        srv, ctx = self._cfg()
        ctx["aux_exec_fn"] = lambda: ("idle", 0)
        ctx["aux_idle_fn"] = lambda: True
        r = C._case_ax_k01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("not running", r.reason)


class Finding3TransitionsTest(unittest.TestCase):
    def test_zero_and_one_transition_inconclusive(self):
        always_on = [(0, True), (2, True), (4, True)]
        self.assertEqual(C.judge_rule_samples(always_on, 10, 10).verdict, Verdict.INCONCLUSIVE)
        one = [(0, False), (2, True), (4, True)]
        self.assertEqual(C.judge_rule_samples(one, 10, 10).verdict, Verdict.INCONCLUSIVE)

    def test_relay_on_before_start_inconclusive_and_no_start(self):
        srv = FakeSrv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        ctx["aux_relay_fn"] = lambda r: True
        r = C._case_ax_t01(ctx)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertNotIn(("start", 7), srv.calls)


class Finding4FirmwareRefusalTest(unittest.TestCase):
    def test_firmware_accepting_fails_even_if_precheck_refuses(self):
        srv = FakeSrv()
        ctx = _ctx(srv, aux_post_fn=lambda relay, en, tc: (200, True))
        r = C._case_ax_c02(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("firmware", r.reason)

    def test_firmware_400_and_precheck_pass(self):
        r = C._case_ax_c02(_ctx(FakeSrv()))
        self.assertEqual(r.verdict, Verdict.PASS)
        self.assertEqual(r.observed["firmware_status"], 400)

    def test_precheck_not_refusing_fails_separately(self):
        srv = FakeSrv()
        srv.refuse_r1 = False
        r = C._case_ax_c02(_ctx(srv))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertFalse(srv.r1)


class Finding5RestoreTest(unittest.TestCase):
    def _srv(self):
        srv = FakeSrv()
        srv.r4e, srv.r4t, srv.r4h, srv.r4on, srv.r4off = False, 2, 3.5, 45, 90
        return srv

    def test_r01_restores_every_field(self):
        srv = self._srv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        srv.r4h = 9.0  # something mutated it beyond the enabled flag
        srv.r4on = 1
        r = C._case_ax_r01(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual((srv.r4e, srv.r4t, srv.r4h, srv.r4on, srv.r4off), (False, 2, 3.5, 45, 90))

    def test_r01_fails_when_field_does_not_read_back(self):
        srv = self._srv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        srv.r4h = 9.0
        srv.restore_drops_hyst = True
        r = C._case_ax_r01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("hyst_c", r.reason)
        self.assertTrue(ctx["_tainted"])

    def test_teardown_hook_restores_after_abort_between_c01_and_r01(self):
        srv = self._srv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        self.assertTrue(srv.r4e)
        self.assertEqual(len(ctx["teardown_hooks"]), 1)
        ctx["teardown_hooks"][0](ctx)  # what the runner teardown does
        self.assertEqual((srv.r4e, srv.r4t, srv.r4h, srv.r4on, srv.r4off), (False, 2, 3.5, 45, 90))
        self.assertNotIn("_tainted", ctx)

    def test_runner_teardown_invokes_hooks(self):
        import inspect
        from kilnctrl.bench_test import runner
        self.assertIn("teardown_hooks", inspect.getsource(runner.BenchTestRunner._teardown_unbounded))

    def test_hook_idempotent_after_r01(self):
        srv = self._srv()
        ctx = _ctx(srv)
        C._case_ax_c01(ctx)
        C._case_ax_r01(ctx)
        n = len(srv.calls)
        ctx["teardown_hooks"][0](ctx)
        self.assertEqual(len(srv.calls), n)


class Finding6C03Test(unittest.TestCase):
    def test_accepted_write_restored_and_taints(self):
        restored = []
        ctx = _ctx(FakeSrv(), aux_zone_mask_post_fn=lambda: (200, {}),
                   aux_zone_mask_restore_fn=lambda: restored.append(1) or True)
        r = C._case_ax_c03(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(restored, [1])
        self.assertTrue(ctx["_tainted"])

    def test_refused_does_not_restore(self):
        restored = []
        ctx = _ctx(FakeSrv(), aux_zone_mask_post_fn=lambda: (400, {}),
                   aux_zone_mask_restore_fn=lambda: restored.append(1) or True)
        self.assertEqual(C._case_ax_c03(ctx).verdict, Verdict.PASS)
        self.assertEqual(restored, [])
        self.assertNotIn("_tainted", ctx)


class Finding7SlotTest(unittest.TestCase):
    def test_c01_skips_when_slot_exists_and_touches_nothing(self):
        srv = FakeSrv()
        srv.preexisting_slot = True
        ctx = _ctx(srv)
        self.assertEqual(C._case_ax_c01(ctx).verdict, Verdict.SKIP)
        self.assertEqual(srv.calls, [])
        self.assertNotIn("_aux_orig", ctx)
        self.assertEqual(C._case_ax_t01(ctx).verdict, Verdict.SKIP)
        self.assertEqual(srv.saved, {})
        self.assertNotIn(("delete", 7), srv.calls)


class Finding7bHeatCaseSlotTest(unittest.TestCase):
    def test_heat_case_skips_when_slot_preexists(self):
        srv = FakeSrv()
        srv.r4e, srv.r4t = True, 0
        srv.preexisting_slot = True
        r = C._case_ax_t01(_ctx(srv))
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertEqual(srv.last_save if hasattr(srv, "last_save") else None, None)
        self.assertNotIn(("delete", 7), srv.calls)


class Finding8TeardownTest(unittest.TestCase):
    def test_no_stop_when_other_profile_running(self):
        srv = FakeSrv()
        srv.exec_state, srv.exec_pid = "running", 3
        ctx = _ctx(srv)
        C._teardown(ctx, 7)
        self.assertNotIn(("stop",), srv.calls)

    def test_stops_own_profile(self):
        srv = FakeSrv()
        srv.exec_state, srv.exec_pid = "running", 7
        C._teardown(_ctx(srv), 7)
        self.assertIn(("stop",), srv.calls)

    def test_no_stop_when_idle(self):
        srv = FakeSrv()
        C._teardown(_ctx(srv), 7)
        self.assertNotIn(("stop",), srv.calls)


if __name__ == "__main__":
    unittest.main()
