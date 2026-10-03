#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_autotune -- AT-01..05's flow
logic (start/poll/abort/accept/matrix-read), every board call faked. See
test_bench_test_cases_heat.py for the fake-object pattern this follows.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_autotune.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_autotune as CA  # noqa: E402
from kilnctrl.bench_test import cases_heat as C  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


class _Reading:
    def __init__(self, channel, temperature_c, valid=True):
        self.channel = channel
        self.temperature_c = temperature_c
        self.valid = valid


class _OkReason:
    def __init__(self, ok=True, error=None, reason=None):
        self.ok = ok
        self.error = error
        self.reason = reason

    def __bool__(self):
        return self.ok


class _Model:
    def __init__(self, k_gain_c_per_duty=38.0, tau_s=265.0, dead_time_s=20.0):
        self.k_gain_c_per_duty = k_gain_c_per_duty
        self.tau_s = tau_s
        self.dead_time_s = dead_time_s


class _Relay:
    def __init__(self, ku=1.0, tu_s=100.0, amplitude_c=5.0):
        self.ku = ku
        self.tu_s = tu_s
        self.amplitude_c = amplitude_c


class _Status:
    def __init__(
        self, state_name="idle", model_valid=True, actual_c=24.0, actual_valid=True,
        duty=0.0, model=None, relay_valid=True, relay=None,
    ):
        self.state_name = state_name
        self.model_valid = model_valid
        self.actual_c = actual_c
        self.actual_valid = actual_valid
        self.duty = duty
        self.model = model or _Model()
        self.relay_valid = relay_valid
        self.relay = relay or _Relay()


class _FakeAutotuneClient:
    def __init__(self, statuses=None, start_result=(True, ""), abort_result=None, accept_result=None):
        self._statuses = list(statuses or [_Status(state_name="done")])
        self.start_result = start_result
        self.abort_result = abort_result if abort_result is not None else _OkReason(ok=True)
        self.accept_result = accept_result if accept_result is not None else _OkReason(ok=False, reason="unsettled")
        self.start_calls = []
        self.abort_calls = 0
        self.accept_calls = []

    def get_status(self):
        if len(self._statuses) > 1:
            return self._statuses.pop(0)
        return self._statuses[0]

    def start(self, zone, method, step_duty_or_setpoint_c, relay_d=-1.0, relay_h_c=-1.0):
        self.start_calls.append((zone, method, step_duty_or_setpoint_c, relay_d, relay_h_c))
        return self.start_result

    def abort(self):
        self.abort_calls += 1
        return self.abort_result

    def accept(self, ack_unsettled=False):
        self.accept_calls.append(ack_unsettled)
        return self.accept_result


class _ZoneCfg:
    def __init__(self, index, pid_kp=1.0, pid_ki=0.1, pid_kd=0.0):
        self.index = index
        self.pid_kp = pid_kp
        self.pid_ki = pid_ki
        self.pid_kd = pid_kd


class _FakeControlClient:
    def __init__(self, zones=None):
        self._zones = zones or [_ZoneCfg(0), _ZoneCfg(1), _ZoneCfg(2)]

    def get_zones(self):
        return len(self._zones), 0b111, self._zones


class _FakeSafetyClient:
    def get_diag(self):
        return type("D", (), {"trip_reason": 0})()

    def get_link_stats(self):
        return type("S", (), {"crc_errors": 0, "timeouts": 0, "broadcast_dropped": 0})()


class _FakeIoClient:
    def __init__(self, relays=0):
        self.relays = relays

    def read(self):
        return type("St", (), {"relays": self.relays})()


class _FakeThermoClient:
    def __init__(self, readings):
        self._readings = readings

    def read(self):
        return self._readings


class _FakeSrv:
    def __init__(self, readings=None, autotune=None, control=None, io=None, safety=None):
        self._thermo = _FakeThermoClient(readings if readings is not None else [
            _Reading(0, 24.0), _Reading(1, 24.2), _Reading(2, 23.9),
        ])
        self._autotune = autotune or _FakeAutotuneClient()
        self._control = control or _FakeControlClient()
        self._io = io or _FakeIoClient()
        self._safety = safety or _FakeSafetyClient()


def _always_ok_preflight(ctx):
    ctx["capability_preflight_run"] = lambda preset, host, **kw: type(
        "R", (), {"ok": True, "board": type("B", (), {})()}
    )()


class _FakeRampAssist:
    """Fake /api/ramp_assist transport (ctx http_get_json/http_post_json)."""

    def __init__(self, enabled=False, accept_writes=True, restore_fails=False, events=None):
        self.events = events if events is not None else []
        self.enabled = enabled
        self.accept_writes = accept_writes
        self.restore_fails = restore_fails
        self.posts = []

    def get(self, path):
        return 200, {"enabled": self.enabled}

    def post(self, path, fields):
        value = fields["enabled"] == "1"
        self.posts.append(value)
        self.events.append(f"post:{int(value)}")
        if not self.accept_writes or (self.restore_fails and value):
            return 200, {"ok": False}
        self.enabled = value
        return 200, {"ok": True}


def _base_ctx(srv, ramp=None, **extra):
    ramp = ramp or _FakeRampAssist()
    ctx = {
        "srv": srv, "host": "10.0.0.5",
        "_now": lambda: 0.0, "_sleep": lambda s: None,
        "http_get_json": ramp.get, "http_post_json": ramp.post,
    }
    _always_ok_preflight(ctx)
    # Never reach the network for the zones-config read; default: unavailable.
    def _no_zones(host):
        raise RuntimeError("zones config unavailable in unit test")
    ctx["_get_zones_config"] = _no_zones
    ctx.update(extra)
    return ctx


class AT01Test(unittest.TestCase):
    def test_passes_on_a_good_step_fit(self):
        autotune = _FakeAutotuneClient(statuses=[_Status(state_name="idle"), _Status(state_name="done", actual_c=24.0)])
        srv = _FakeSrv(autotune=autotune)
        ctx = _base_ctx(srv)
        result = CA._case_at01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(autotune.start_calls[0][:2], (0, 0))

    def test_skips_when_not_rested(self):
        srv = _FakeSrv(readings=[_Reading(0, 24.0), _Reading(1, 40.0), _Reading(2, 24.0)])
        ctx = _base_ctx(srv, _sleep=lambda s: None)
        ctx["_now"] = self._clock().now
        result = CA._case_at01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def _clock(self):
        class _C:
            t = [0.0]

            def now(self):
                self.t[0] += 100.0
                return self.t[0]
        return _C()

    def test_start_refusal_fails(self):
        autotune = _FakeAutotuneClient(statuses=[_Status(state_name="idle")], start_result=(False, "busy"))
        srv = _FakeSrv(autotune=autotune)
        ctx = _base_ctx(srv)
        result = CA._case_at01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("busy", result.reason)


def _good_at01_srv():
    autotune = _FakeAutotuneClient(statuses=[_Status(state_name="idle"), _Status(state_name="done", actual_c=24.0)])
    return autotune, _FakeSrv(autotune=autotune)


class RampAssistHandlingTest(unittest.TestCase):
    def test_assist_already_off_is_never_written(self):
        ramp = _FakeRampAssist(enabled=False)
        autotune, srv = _good_at01_srv()
        ctx = _base_ctx(srv, ramp=ramp)
        result = CA._case_at01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(ramp.posts, [])
        self.assertFalse(ctx.get("_tainted"))

    def test_assist_on_is_disabled_for_the_case_then_restored(self):
        ramp = _FakeRampAssist(enabled=True)
        seen = {}
        autotune, srv = _good_at01_srv()
        orig_start = autotune.start

        def start(*a, **kw):
            seen["enabled_at_start"] = ramp.enabled
            return orig_start(*a, **kw)
        autotune.start = start
        ctx = _base_ctx(srv, ramp=ramp)
        result = CA._case_at01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertIs(seen["enabled_at_start"], False)
        self.assertEqual(ramp.posts, [False, True])
        self.assertTrue(ramp.enabled)
        self.assertFalse(ctx.get("_tainted"))
        self.assertEqual(autotune.accept_calls, [])

    def test_assist_restored_even_when_case_body_raises(self):
        events = []
        ramp = _FakeRampAssist(enabled=True, events=events)
        autotune, srv = _good_at01_srv()
        orig_abort = autotune.abort

        def abort():
            events.append("abort")
            return orig_abort()
        autotune.abort = abort
        orig_status = autotune.get_status

        def get_status():
            if autotune.start_calls:
                raise RuntimeError("boom")
            return orig_status()
        autotune.get_status = get_status
        ctx = _base_ctx(srv, ramp=ramp)
        with self.assertRaises(RuntimeError):
            CA._case_at01(ctx)
        self.assertTrue(ramp.enabled)
        self.assertGreaterEqual(autotune.abort_calls, 1)
        self.assertEqual(events, ["post:0", "abort", "post:1"])

    def test_restore_failure_marks_run_tainted(self):
        ramp = _FakeRampAssist(enabled=True, restore_fails=True)
        autotune, srv = _good_at01_srv()
        ctx = _base_ctx(srv, ramp=ramp)
        result = CA._case_at01(ctx)
        self.assertTrue(ctx["_tainted"])
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("could NOT be restored", result.reason)
        self.assertTrue(any("tainted" in e for e in result.evidence))

    def test_disable_failure_skips_without_starting_and_still_restores(self):
        ramp = _FakeRampAssist(enabled=True, accept_writes=False)
        autotune, srv = _good_at01_srv()
        ctx = _base_ctx(srv, ramp=ramp)
        result = CA._case_at04(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertEqual(autotune.start_calls, [])
        self.assertTrue(ramp.enabled)
        self.assertFalse(ctx.get("_tainted"))

    def test_at02_and_at04_also_toggle_and_restore(self):
        for fn, srv in (
            (CA._case_at02, _FakeSrv(autotune=_FakeAutotuneClient(
                statuses=[_Status(state_name="idle"), _Status(state_name="idle")]))),
            (CA._case_at04, _good_at01_srv()[1]),
        ):
            ramp = _FakeRampAssist(enabled=True)
            ctx = _base_ctx(srv, ramp=ramp)
            fn(ctx)
            self.assertEqual(ramp.posts, [False, True], fn.__name__)
            self.assertTrue(ramp.enabled)

    def test_unreadable_route_proceeds_without_writing(self):
        ramp = _FakeRampAssist(enabled=True)
        autotune, srv = _good_at01_srv()
        ctx = _base_ctx(srv, ramp=ramp)
        ctx["http_get_json"] = lambda path: (500, None)
        result = CA._case_at01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(ramp.posts, [])


class AT02Test(unittest.TestCase):
    def test_passes_on_a_clean_abort(self):
        autotune = _FakeAutotuneClient(statuses=[_Status(state_name="idle"), _Status(state_name="idle", duty=0.0)])
        srv = _FakeSrv(autotune=autotune, io=_FakeIoClient(relays=0))
        ctx = _base_ctx(srv)
        result = CA._case_at02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertGreaterEqual(autotune.abort_calls, 1)

    def test_fails_when_relay_still_on_after_abort(self):
        autotune = _FakeAutotuneClient(statuses=[_Status(state_name="idle"), _Status(state_name="idle", duty=0.0)])
        srv = _FakeSrv(autotune=autotune, io=_FakeIoClient(relays=1))
        ctx = _base_ctx(srv)
        result = CA._case_at02(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class AT03Test(unittest.TestCase):
    def test_passes_when_accept_refused_and_gains_stable(self):
        control = _FakeControlClient(zones=[_ZoneCfg(0, 1.0, 0.1, 0.0)])
        autotune = _FakeAutotuneClient(accept_result=_OkReason(ok=False, reason="unsettled"))
        srv = _FakeSrv(autotune=autotune, control=control)
        ctx = _base_ctx(srv)
        result = CA._case_at03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_fails_when_accept_wrongly_succeeds(self):
        control = _FakeControlClient(zones=[_ZoneCfg(0, 1.0, 0.1, 0.0)])
        autotune = _FakeAutotuneClient(accept_result=_OkReason(ok=True))
        srv = _FakeSrv(autotune=autotune, control=control)
        ctx = _base_ctx(srv)
        result = CA._case_at03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class AT04Test(unittest.TestCase):
    def test_passes_on_a_good_relay_fit(self):
        autotune = _FakeAutotuneClient(statuses=[_Status(state_name="idle"), _Status(state_name="done", relay_valid=True)])
        srv = _FakeSrv(autotune=autotune)
        ctx = _base_ctx(srv)
        result = CA._case_at04(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(autotune.start_calls[0][1], 1)

    def test_inconclusive_on_insufficient_relay_amplitude(self):
        autotune = _FakeAutotuneClient(statuses=[
            _Status(state_name="idle"),
            _Status(state_name="done", relay_valid=False, relay=_Relay(amplitude_c=0.2)),
        ])
        srv = _FakeSrv(autotune=autotune)
        ctx = _base_ctx(srv)
        result = CA._case_at04(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


class AT05Test(unittest.TestCase):
    def test_passes_on_a_well_formed_matrix(self):
        srv = _FakeSrv()
        ctx = _base_ctx(srv)
        matrix = [[0, 0.1, 0.05], [0.1, 0, 0.1], [0.05, 0.1, 0]]
        orig = CA._http_get_json
        CA._http_get_json = lambda host, path, **kw: (200, {"matrix": matrix})
        try:
            result = CA._case_at05(ctx)
        finally:
            CA._http_get_json = orig
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_inconclusive_with_no_host(self):
        srv = _FakeSrv()
        ctx = _base_ctx(srv)
        del ctx["host"]
        result = CA._case_at05(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


def _zones_snapshot(k=48.0, tau=287.0):
    return {"zones": [{"index": 0, "k": k, "tau": tau}, {"index": 1, "k": 1.0, "tau": 1.0}]}


class AutotuneHarnessRegressionTest(unittest.TestCase):
    """First real-hardware run (20261003T191941Z_autotune) defects."""

    def test_precondition_accepts_done_and_aborted_not_running(self):
        for name in ("idle", "done", "aborted"):
            srv = _FakeSrv(autotune=_FakeAutotuneClient(statuses=[_Status(state_name=name)]))
            ok, reason = CA._autotune_not_running(_base_ctx(srv))
            self.assertTrue(ok, (name, reason))
        for name in ("settling", "stepping", "relay_approach", "relay_cycling"):
            srv = _FakeSrv(autotune=_FakeAutotuneClient(statuses=[_Status(state_name=name)]))
            ok, reason = CA._autotune_not_running(_base_ctx(srv))
            self.assertFalse(ok, name)
            self.assertIn(f"autotune is running (state={name})", reason)

    def test_at01_runs_when_previous_run_left_state_done(self):
        autotune = _FakeAutotuneClient(statuses=[
            _Status(state_name="done", actual_c=55.0), _Status(state_name="done", actual_c=24.0)])
        srv = _FakeSrv(autotune=autotune)
        result = CA._case_at01(_base_ctx(srv))
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_at02_runs_from_done_and_accepts_aborted_terminal_state(self):
        autotune = _FakeAutotuneClient(statuses=[
            _Status(state_name="done"), _Status(state_name="stepping"), _Status(state_name="aborted")])
        srv = _FakeSrv(autotune=autotune, io=_FakeIoClient(relays=0))
        result = CA._case_at02(_base_ctx(srv))
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_at04_runs_when_previous_run_left_state_aborted(self):
        autotune = _FakeAutotuneClient(statuses=[_Status(state_name="aborted"), _Status(state_name="done")])
        srv = _FakeSrv(autotune=autotune)
        result = CA._case_at04(_base_ctx(srv))
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_baseline_is_the_pre_start_reading_not_final_actual_c(self):
        # Heated to 55 C by a 0.5-duty step: actual_c is the CURRENT temp.
        for fn in (CA._case_at01, CA._case_at04):
            autotune = _FakeAutotuneClient(statuses=[
                _Status(state_name="idle"), _Status(state_name="done", actual_c=55.1)])
            srv = _FakeSrv(autotune=autotune)
            result = fn(_base_ctx(srv))
            self.assertEqual(result.verdict, Verdict.PASS, (fn.__name__, result.reason))
            self.assertEqual(result.observed["baseline_c"], 24.0)
            self.assertEqual(result.observed["final_c"], 55.1)

    def test_baseline_far_from_rested_reference_still_fails(self):
        # Zone 0 is 3 C above the coldest zone before the start: genuinely not rested.
        srv = _FakeSrv(
            readings=[_Reading(0, 25.9), _Reading(1, 24.0), _Reading(2, 24.5)],
            autotune=_FakeAutotuneClient(statuses=[_Status(state_name="idle"), _Status(state_name="done")]))
        srv_ctx = _base_ctx(srv)
        result = CA._at01_body(srv_ctx)
        self.assertEqual(result.verdict, Verdict.PASS)  # 1.9 C is inside the 2 C band
        srv = _FakeSrv(
            readings=[_Reading(0, 26.5), _Reading(1, 24.0), _Reading(2, 24.5)],
            autotune=_FakeAutotuneClient(statuses=[_Status(state_name="idle"), _Status(state_name="done")]))
        result = CA._at01_body(_base_ctx(srv))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("rested reference", result.reason)

    def test_k_and_tau_expectation_come_from_zones_config(self):
        status = _Status(state_name="done", model=_Model(k_gain_c_per_duty=48.45, tau_s=286.9))
        autotune = _FakeAutotuneClient(statuses=[_Status(state_name="idle"), status])
        srv = _FakeSrv(autotune=autotune)
        ctx = _base_ctx(srv, _get_zones_config=lambda host: _zones_snapshot(k=48.0, tau=287.0))
        result = CA._case_at01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(result.observed["k_expected_source"], "zones_config")
        self.assertEqual(result.observed["tau_expected_source"], "zones_config")

    def test_k_outside_committed_tolerance_fails(self):
        status = _Status(state_name="done", model=_Model(k_gain_c_per_duty=38.0, tau_s=287.0))
        autotune = _FakeAutotuneClient(statuses=[_Status(state_name="idle"), status])
        srv = _FakeSrv(autotune=autotune)
        ctx = _base_ctx(srv, _get_zones_config=lambda host: _zones_snapshot(k=48.0, tau=287.0))
        result = CA._case_at01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("fitted K", result.reason)

    def test_falls_back_to_defaults_when_zones_config_unavailable_or_nonpositive(self):
        for getter in (None, lambda host: _zones_snapshot(k=0.0, tau=0.0),
                       lambda host: {"zones": []}, lambda host: {"zones": [{"index": 0}]}):
            autotune = _FakeAutotuneClient(statuses=[_Status(state_name="idle"), _Status(state_name="done")])
            srv = _FakeSrv(autotune=autotune)
            ctx = _base_ctx(srv) if getter is None else _base_ctx(srv, _get_zones_config=getter)
            result = CA._case_at01(ctx)
            self.assertEqual(result.verdict, Verdict.PASS, result.reason)
            self.assertEqual(result.observed["k_expected_source"], "default")
            self.assertEqual(result.observed["tau_expected_source"], "default")


if __name__ == "__main__":
    unittest.main()
