#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_ota -- OT-B01 (dual reset
trip), OT-E01/E02/E03/E12 (OTA update/rollback/corrupt-image), and SP-04
(the OT-B01 observer in cases_safety.py). Every board/HTTP call is faked;
this wave was never run against the bench board -- it is blocked by a
standing unacknowledged crash report -- so these confirm the case/judgment
LOGIC only.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_ota.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_ota as C  # noqa: E402
from kilnctrl.bench_test import cases_safety as CS  # noqa: E402
from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test.registry import Verdict, get_case  # noqa: E402


class _ExecStatus:
    def __init__(self, state_name="idle"):
        self.state_name = state_name


class _FakeProfiles:
    def __init__(self, state_name="idle"):
        self._state = state_name

    def get_exec_status(self):
        return _ExecStatus(self._state)


class _FakeSrv:
    def __init__(self, state_name="idle"):
        self._profiles = _FakeProfiles(state_name)
        self._safety = None
        self.sent = []

    def _send(self, task_id, payload):
        self.sent.append((task_id, payload))
        return b""


class _SafetyStatus:
    def __init__(self, link_up=False, enabled=False):
        self.link_up = link_up
        self.enabled = enabled


class _SafetyDiag:
    def __init__(self, trip_reason=None, trip_mask=None):
        self.trip_reason = trip_reason
        self.trip_mask = trip_mask


def _clock():
    state = {"t": 0.0}
    return state, (lambda: state["t"]), (lambda s: state.__setitem__("t", state["t"] + s))


class IdleGateTest(unittest.TestCase):
    def test_not_idle_skips(self):
        ctx = {"srv": _FakeSrv(state_name="running")}
        ok, reason = C._is_idle(ctx)
        self.assertFalse(ok)
        self.assertIn("running", reason)

    def test_idle_ok(self):
        ctx = {"srv": _FakeSrv(state_name="idle")}
        ok, _reason = C._is_idle(ctx)
        self.assertTrue(ok)

    def test_status_error_refuses(self):
        srv = _FakeSrv()

        def _raise():
            raise RuntimeError("no reply")

        srv._profiles.get_exec_status = _raise
        ok, reason = C._is_idle({"srv": srv})
        self.assertFalse(ok)
        self.assertIn("RuntimeError", reason)


class Otb01Test(unittest.TestCase):
    def _ctx(self, state_name="idle", link_up=True, trip_reason=6, trip_mask=0x0020,
              enabled_after_clear=True, readiness_ok=True, relay_energized=False):
        _clockstate, now, sleep = _clock()
        srv = _FakeSrv(state_name=state_name)
        statuses = iter([_SafetyStatus(link_up=link_up)] * 5)
        diag = _SafetyDiag(trip_reason=trip_reason, trip_mask=trip_mask)
        cleared = {"v": False}

        def get_status_fn():
            if not link_up:
                return _SafetyStatus(link_up=False)
            if cleared["v"]:
                return _SafetyStatus(link_up=True, enabled=enabled_after_clear)
            return _SafetyStatus(link_up=True, enabled=False)

        def clear_trip_fn():
            cleared["v"] = True

        ctx = {
            "srv": srv, "host": "192.168.4.1", "ap_password": "secret",
            "ota_http_client": type("O", (), {"sw_reset": staticmethod(lambda host, pw: {"ok": True})})(),
            "dashboard_http_client": _FakeDashboardClient(relay_energized=relay_energized),
            "_now": now, "_sleep": sleep,
            "_get_safety_status_fn": get_status_fn,
            "_get_safety_diag_fn": lambda: diag,
            "_clear_trip_fn": clear_trip_fn,
            "_readiness_trip_ok_fn": lambda: readiness_ok,
        }
        return ctx

    def test_skips_when_not_idle(self):
        ctx = self._ctx(state_name="running")
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_skips_without_ap_password(self):
        ctx = self._ctx()
        ctx["ap_password"] = None
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_correct_trip_and_clear_passes_and_stashes_ctx(self):
        ctx = self._ctx()
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(ctx["_otb01"]["trip_mask"], 0x0020)
        self.assertTrue(ctx["_otb01"]["clear_ok"])

    def test_wrong_trip_reason_fails(self):
        ctx = self._ctx(trip_reason=7, trip_mask=0x0040)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_never_clears_a_trip_that_is_not_s6a(self):
        """Plan doc section 6 rule 5: a trip whose reason/mask is not
        exactly S6a (reason 6, mask 1 << 5) stops the run for a human --
        safety_clear_trip() must NOT be called at all."""
        called = {"v": False}
        ctx = self._ctx(trip_reason=7, trip_mask=0x0040)

        def clear_trip_fn():
            called["v"] = True

        ctx["_clear_trip_fn"] = clear_trip_fn
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertFalse(called["v"], "safety_clear_trip() was called on a non-S6a trip")
        self.assertIsNone(ctx["_otb01"]["clear_ok"])

    def test_never_clears_when_mask_has_extra_bits(self):
        called = {"v": False}
        ctx = self._ctx(trip_reason=6, trip_mask=0x0060)

        def clear_trip_fn():
            called["v"] = True

        ctx["_clear_trip_fn"] = clear_trip_fn
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertFalse(called["v"], "safety_clear_trip() was called with extra trip bits set")

    def test_link_never_up_fails(self):
        ctx = self._ctx(link_up=False)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("link did not come back up", result.reason)

    def test_sw_reset_raise_fails(self):
        ctx = self._ctx()
        ctx["_sw_reset_fn"] = lambda: (_ for _ in ()).throw(RuntimeError("boom"))
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("boom", result.reason)

    def test_refuses_dual_reset_while_relay_energized(self):
        """Reviewer's landed-review gap: OT-B01 previously only gated on
        the executor being idle. A relay still energized (e.g. from a
        stuck/manual path unrelated to the executor) must also refuse the
        dual reset -- and sw_reset must never even be attempted."""
        sw_reset_called = {"v": False}
        ctx = self._ctx(relay_energized=True)
        ctx["_sw_reset_fn"] = lambda: sw_reset_called.__setitem__("v", True)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("energized", result.reason)
        self.assertFalse(sw_reset_called["v"], "sw_reset_esp was called while a relay was still energized")

    def test_refuses_dual_reset_when_relay_state_unreadable(self):
        ctx = self._ctx()
        ctx["dashboard_http_client"] = type("D", (), {"get_status": staticmethod(lambda host: (_ for _ in ()).throw(RuntimeError("no reply")))})()
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("could not confirm", result.reason)

    def test_relay_not_energized_still_passes(self):
        ctx = self._ctx(relay_energized=False)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)


class Sp04ObserverTest(unittest.TestCase):
    def test_not_run_when_otb01_absent(self):
        result = CS._case_sp04({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_reads_stashed_otb01_data(self):
        ctx = {"_otb01": {
            "link_up": True, "trip_reason": 6, "trip_mask": 0x0020,
            "clear_ok": True, "readiness_trip_ok": True,
        }}
        result = CS._case_sp04(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_reads_stashed_failure(self):
        ctx = {"_otb01": {
            "link_up": True, "trip_reason": 7, "trip_mask": 0x0040,
            "clear_ok": True, "readiness_trip_ok": True,
        }}
        result = CS._case_sp04(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class _OtaPushResult:
    def __init__(self, ok, status_code=200, body=None):
        self.ok = ok
        self.status_code = status_code
        self.body = body or {}


class _FakeOtaClient:
    def __init__(self, push_result=None, phases=None, boot_guard_recovery=False, interlock_ok=True, interlock_reason=""):
        self.push_result = push_result or _OtaPushResult(True)
        self._phases = list(phases or ["done"])
        self.boot_guard_recovery = boot_guard_recovery
        self.interlock_ok = interlock_ok
        self.interlock_reason = interlock_reason
        self.pushed = []

    def push_esp_image(self, host, path, ap_password, timeout=None):
        self.pushed.append(path)
        return self.push_result

    def get_esp_status(self, host):
        phase = self._phases.pop(0) if len(self._phases) > 1 else self._phases[0]
        return {"phase": phase}

    def get_boot_guard_status(self, host):
        return {"recovery_mode": self.boot_guard_recovery}

    def get_interlock(self, host):
        if self.interlock_ok:
            return {"ok": True}
        return {"ok": False, "reason": self.interlock_reason}

    def rollback_esp(self, host, ap_password):
        return {"ok": True}

    def push_esp_image_unauthenticated(self, host, path, timeout=None):
        self.pushed.append(path)
        return self.push_result

    def push_esp_image_with_session(self, host, path, session_cookie, timeout=None):
        self.pushed.append((path, session_cookie))
        return self.push_result


class _FakePartitionClient:
    def __init__(self, running="app"):
        self.running = running

    def get_partitions(self, host):
        return {"running": self.running}


class _FakeZonesClient:
    def __init__(self, zones=None):
        self._zones = zones if zones is not None else {"zones": [{"pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}]}

    def get_zones(self, host):
        return self._zones


class _FakeDashboardClient:
    def __init__(self, fw_build="B1", load_fault=False, relay_energized=False):
        self.fw_build = fw_build
        self.load_fault = load_fault
        self.relay_energized = relay_energized

    def get_status(self, host):
        return {
            "fw_build": self.fw_build, "zones_config_load_fault": self.load_fault,
            "safety_relay_energized": self.relay_energized,
        }


class Ote01Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "ota_image_path": "/tmp/image.bin", "ota_image_build": "B2",
            "ota_http_client": _FakeOtaClient(),
            "partition_http_client": _FakePartitionClient(running="app"),
            "zones_http_client": _FakeZonesClient(),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B2"),
            "_now": lambda: 0.0, "_sleep": lambda s: None,
        }
        ctx.update(overrides)
        return ctx

    def test_skips_without_image_path(self):
        ctx = self._ctx(ota_image_path=None)
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_skips_when_not_idle(self):
        ctx = self._ctx(srv=_FakeSrv(state_name="running"))
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_push_refused_fails(self):
        ctx = self._ctx(ota_http_client=_FakeOtaClient(push_result=_OtaPushResult(False, 400)))
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("refused", result.reason)

    def test_good_push_passes_and_stashes(self):
        ctx = self._ctx()
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertIn("_ote01", ctx)
        self.assertIn("_ote_pre_update", ctx)

    def test_interlock_not_ok_skips_before_pushing(self):
        """Plan doc section 6 rule 1: every OTA action confirms
        GET /api/ota/interlock ok:true immediately before the call --
        a not-ok interlock must refuse before push_esp_image is ever
        attempted."""
        client = _FakeOtaClient(interlock_ok=False, interlock_reason="kiln is not idle")
        ctx = self._ctx(ota_http_client=client)
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("kiln is not idle", result.reason)
        self.assertEqual(client.pushed, [], "push_esp_image was called despite a not-ok interlock")

    def test_interlock_read_error_skips(self):
        client = _FakeOtaClient()
        client.get_interlock = lambda host: (_ for _ in ()).throw(RuntimeError("timeout"))
        ctx = self._ctx(ota_http_client=client)
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("could not read /api/ota/interlock", result.reason)


class Ote02Test(unittest.TestCase):
    def test_not_run_without_ote01(self):
        result = C._case_ote02({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_load_fault_fails(self):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "_ote_pre_update": {"fw_build": "B1", "zones": {"zones": [{"pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}]}},
            "ota_http_client": _FakeOtaClient(),
            "zones_http_client": _FakeZonesClient(),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1", load_fault=True),
            "_now": lambda: 0.0, "_sleep": lambda s: None,
        }
        result = C._case_ote02(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("ZONES_CFG_VERSION", result.reason)

    def test_matching_gains_passes(self):
        gains = {"zones": [{"pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}]}
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "_ote_pre_update": {"fw_build": "B1", "zones": gains},
            "ota_http_client": _FakeOtaClient(),
            "zones_http_client": _FakeZonesClient(zones=gains),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1", load_fault=False),
            "_now": lambda: 0.0, "_sleep": lambda s: None,
        }
        result = C._case_ote02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_interlock_not_ok_skips_before_rolling_back(self):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "_ote_pre_update": {"fw_build": "B1", "zones": {"zones": [{"pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}]}},
            "ota_http_client": _FakeOtaClient(interlock_ok=False, interlock_reason="an update is already in progress"),
            "zones_http_client": _FakeZonesClient(),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1", load_fault=False),
            "_now": lambda: 0.0, "_sleep": lambda s: None,
        }
        result = C._case_ote02(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("already in progress", result.reason)

    def test_gains_changed_without_load_fault_still_fails(self):
        before = {"zones": [{"pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}]}
        after = {"zones": [{"pid_kp": 9.0, "pid_ki": 0.1, "pid_kd": 0.0}]}
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "_ote_pre_update": {"fw_build": "B1", "zones": before},
            "ota_http_client": _FakeOtaClient(),
            "zones_http_client": _FakeZonesClient(zones=after),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1", load_fault=False),
            "_now": lambda: 0.0, "_sleep": lambda s: None,
        }
        result = C._case_ote02(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class Ote03Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "ota_corrupt_image_path": "/tmp/bad.bin",
            "ota_http_client": _FakeOtaClient(push_result=_OtaPushResult(False, 400)),
            "partition_http_client": _FakePartitionClient(running="app"),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1"),
        }
        ctx.update(overrides)
        return ctx

    def test_refused_unchanged_passes(self):
        result = C._case_ote03(self._ctx())
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_accepted_push_fails(self):
        ctx = self._ctx(ota_http_client=_FakeOtaClient(push_result=_OtaPushResult(True, 200)))
        result = C._case_ote03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_interlock_not_ok_skips_before_pushing(self):
        client = _FakeOtaClient(push_result=_OtaPushResult(False, 400), interlock_ok=False, interlock_reason="kiln is not idle")
        ctx = self._ctx(ota_http_client=client)
        result = C._case_ote03(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertEqual(client.pushed, [])

    def test_refused_but_partition_changed_still_fails(self):
        # simulate RUNNING changing across the call by giving different
        # partition clients before/after via a stateful fake
        class _Toggling:
            def __init__(self):
                self.calls = 0

            def get_partitions(self, host):
                self.calls += 1
                return {"running": "app" if self.calls == 1 else "recovery"}

        ctx = self._ctx(partition_http_client=_Toggling())
        result = C._case_ote03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class Ote12Test(unittest.TestCase):
    def test_not_run_without_any_ote_case(self):
        result = C._case_ote12({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_app_running_passes(self):
        ctx = {"_ote01": {}, "host": "10.0.0.5", "partition_http_client": _FakePartitionClient(running="app")}
        result = C._case_ote12(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_recovery_running_fails(self):
        ctx = {"_ote01": {}, "host": "10.0.0.5", "partition_http_client": _FakePartitionClient(running="recovery")}
        result = C._case_ote12(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class Ote04Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "ota_truncated_image_path": "/tmp/truncated.bin",
            "ota_http_client": _FakeOtaClient(push_result=_OtaPushResult(False, 400)),
            "partition_http_client": _FakePartitionClient(running="app"),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1"),
        }
        ctx.update(overrides)
        return ctx

    def test_skips_without_image_path(self):
        result = C._case_ote04(self._ctx(ota_truncated_image_path=None))
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_refused_unchanged_passes(self):
        result = C._case_ote04(self._ctx())
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_accepted_push_fails(self):
        ctx = self._ctx(ota_http_client=_FakeOtaClient(push_result=_OtaPushResult(True, 200)))
        result = C._case_ote04(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_interlock_not_ok_skips_before_pushing(self):
        client = _FakeOtaClient(push_result=_OtaPushResult(False, 400), interlock_ok=False, interlock_reason="not idle")
        ctx = self._ctx(ota_http_client=client)
        result = C._case_ote04(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertEqual(client.pushed, [])


class Ote05Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "ota_wrong_build_image_path": "/tmp/recovery.bin",
            "ota_http_client": _FakeOtaClient(push_result=_OtaPushResult(False, 400)),
            "partition_http_client": _FakePartitionClient(running="app"),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1"),
        }
        ctx.update(overrides)
        return ctx

    def test_skips_without_image_path(self):
        result = C._case_ote05(self._ctx(ota_wrong_build_image_path=None))
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_refused_unchanged_passes(self):
        result = C._case_ote05(self._ctx())
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_accepted_push_fails(self):
        ctx = self._ctx(ota_http_client=_FakeOtaClient(push_result=_OtaPushResult(True, 200)))
        result = C._case_ote05(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_interlock_not_ok_skips_before_pushing(self):
        client = _FakeOtaClient(push_result=_OtaPushResult(False, 400), interlock_ok=False, interlock_reason="not idle")
        ctx = self._ctx(ota_http_client=client)
        result = C._case_ote05(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertEqual(client.pushed, [])


class Ote06Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "ota_image_path": "/tmp/image.bin",
            "ota_http_client": _FakeOtaClient(),
            "partition_http_client": _FakePartitionClient(running="app"),
            "zones_http_client": _FakeZonesClient(),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1"),
            "_now": lambda: 0.0, "_sleep": lambda s: None,
        }
        ctx.update(overrides)
        return ctx

    def test_skips_without_attended_prompt_seam(self):
        ctx = self._ctx()
        self.assertNotIn("attended_prompt", ctx)
        result = C._case_ote06(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertEqual(result.reason, "requires --attended")

    def test_operator_declines_ready_prompt_skips(self):
        ctx = self._ctx(attended_prompt=lambda msg: False)
        result = C._case_ote06(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_operator_no_response_to_ready_prompt_skips(self):
        ctx = self._ctx(attended_prompt=lambda msg: None)
        result = C._case_ote06(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_operator_confirms_and_state_unchanged_passes(self):
        calls = {"n": 0}

        def prompt(msg):
            calls["n"] += 1
            return True

        push_calls = {"n": 0}

        def push_fn():
            push_calls["n"] += 1
            raise RuntimeError("connection dropped")

        ctx = self._ctx(attended_prompt=prompt, _push_fn=push_fn)
        result = C._case_ote06(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(calls["n"], 2)
        self.assertEqual(push_calls["n"], 1)

    def test_operator_does_not_confirm_restoration_is_inconclusive(self):
        responses = iter([True, False])
        ctx = self._ctx(attended_prompt=lambda msg: next(responses), _push_fn=lambda: None)
        result = C._case_ote06(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_fw_build_changed_across_power_loss_fails(self):
        responses = iter([True, True])
        ctx = self._ctx(
            attended_prompt=lambda msg: next(responses), _push_fn=lambda: None,
            dashboard_http_client=_FakeDashboardClient(fw_build="B2"),
        )
        # pre-read uses fw_build_before via _fw_build(ctx, host) each call, so
        # force a changing sequence with a stateful fake
        class _Changing:
            def __init__(self):
                self.n = 0

            def get_status(self, host):
                self.n += 1
                return {"fw_build": "B1" if self.n == 1 else "B2", "zones_config_load_fault": False}

        ctx["dashboard_http_client"] = _Changing()
        result = C._case_ote06(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("fw_build changed", result.reason)

    def test_interlock_not_ok_skips_before_prompting(self):
        ctx = self._ctx(ota_http_client=_FakeOtaClient(interlock_ok=False, interlock_reason="not idle"))
        prompted = {"v": False}
        ctx["attended_prompt"] = lambda msg: prompted.__setitem__("v", True) or True
        result = C._case_ote06(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertFalse(prompted["v"], "operator was prompted despite a not-ok interlock")


class Ote07Ote08Test(unittest.TestCase):
    def _ctx(self, expected_state, state_fn_key, **overrides):
        ctx = {
            "host": "10.0.0.5", "ap_password": "secret", "ota_image_path": "/tmp/image.bin",
            "ota_http_client": _FakeOtaClient(push_result=_OtaPushResult(False, 409), interlock_ok=False, interlock_reason="not idle"),
            state_fn_key: lambda: expected_state,
        }
        ctx.update(overrides)
        return ctx

    def test_ote07_refused_during_running_firing_passes(self):
        ctx = self._ctx("running", "_exec_state_fn")
        result = C._case_ote07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_ote07_push_accepted_during_firing_fails(self):
        ctx = self._ctx("running", "_exec_state_fn", ota_http_client=_FakeOtaClient(push_result=_OtaPushResult(True, 200)))
        result = C._case_ote07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_ote07_wrong_precondition_state_skips(self):
        ctx = self._ctx("idle", "_exec_state_fn")
        result = C._case_ote07(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_ote08_refused_during_autotune_passes(self):
        ctx = self._ctx("stepping", "_autotune_state_fn")
        result = C._case_ote08(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_ote08_push_accepted_during_autotune_fails(self):
        ctx = self._ctx("stepping", "_autotune_state_fn", ota_http_client=_FakeOtaClient(push_result=_OtaPushResult(True, 200)))
        result = C._case_ote08(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_ote07_state_disturbed_by_refused_push_fails(self):
        calls = {"n": 0}

        def state_fn():
            calls["n"] += 1
            return "running" if calls["n"] == 1 else "paused"

        ctx = self._ctx("running", "_exec_state_fn")
        ctx["_exec_state_fn"] = state_fn
        result = C._case_ote07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_ote08_passes_across_a_settling_to_stepping_transition(self):
        """Autotune legitimately moves settling -> stepping during the case.
        Comparing the raw state_name before/after would FAIL that ordinary,
        correct run; the "active" normalization is what makes the judge's
        state_after check mean "still autotuning" rather than "still in the
        exact same sub-state"."""
        seq = iter(["settling", "stepping", "stepping"])
        ctx = self._ctx("stepping", "_autotune_state_fn")
        ctx["_autotune_state_fn"] = lambda: next(seq)
        result = C._case_ote08(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_ote08_fails_when_autotune_dies_across_the_refused_push(self):
        seq = iter(["stepping", "aborted", "aborted"])
        ctx = self._ctx("stepping", "_autotune_state_fn")
        ctx["_autotune_state_fn"] = lambda: next(seq)
        result = C._case_ote08(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("disturbed", result.reason)

    def test_default_expected_states_exist_in_the_real_device_vocabularies(self):
        """The defect this pins: OT-E07 defaulted to "RUNNING" and OT-E08 to
        "ACTIVE", neither of which any real status object can ever return
        (devices_profiles/devices_autotune's STATE_NAMES are lowercase, and
        autotune has no "active" state at all). Both cases were therefore
        structurally incapable of passing on a board, while their unit tests
        -- which fed the same fabricated strings back in -- stayed green."""
        from kilnctrl.devices_profiles import ProfileExecStatus
        from kilnctrl.devices_autotune import AutotuneStatus
        self.assertIn(C.OTE07_DEFAULT_STATE, set(ProfileExecStatus.STATE_NAMES.values()))
        self.assertTrue(
            C.OTE08_ACTIVE_STATES <= set(AutotuneStatus.STATE_NAMES.values()),
            "OTE08_ACTIVE_STATES names a state devices_autotune does not have",
        )
        # ...and the label it normalizes to must NOT collide with a real one.
        self.assertNotIn(C.OTE08_ACTIVE_LABEL, set(AutotuneStatus.STATE_NAMES.values()))


class Ote09Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5",
            "ota_image_path": "/tmp/image.bin",
            "ota_http_client": _FakeOtaClient(push_result=_OtaPushResult(False, 401)),
            "partition_http_client": _FakePartitionClient(running="app"),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1"),
        }
        ctx.update(overrides)
        return ctx

    def test_no_credential_refused_passes(self):
        result = C._case_ote09(self._ctx())
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_no_credential_accepted_fails(self):
        ctx = self._ctx(ota_http_client=_FakeOtaClient(push_result=_OtaPushResult(True, 200)))
        result = C._case_ote09(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_skips_without_image_path(self):
        result = C._case_ote09(self._ctx(ota_image_path=None))
        self.assertEqual(result.verdict, Verdict.SKIP)


class Ote10Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5",
            "ota_image_path": "/tmp/image.bin",
            "web_admin_username": "admin", "web_admin_password": "adminpw",
            "web_user_username": "user1", "web_user_password": "user1pw",
            "ota_http_client": _FakeOtaClient(),
            "_login_fn": lambda user, pw: (200, f"sid-{user}"),
        }
        ctx.update(overrides)
        return ctx

    def test_skips_without_credentials(self):
        result = C._case_ote10(self._ctx(web_user_username=None))
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_admin_ok_user_refused_passes(self):
        def push_with_session(cookie):
            return _OtaPushResult(cookie == "sid-admin")

        ctx = self._ctx(_push_with_session_fn=push_with_session)
        result = C._case_ote10(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_admin_refused_fails(self):
        ctx = self._ctx(_push_with_session_fn=lambda cookie: _OtaPushResult(False))
        result = C._case_ote10(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_user_accepted_fails(self):
        ctx = self._ctx(_push_with_session_fn=lambda cookie: _OtaPushResult(True))
        result = C._case_ote10(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_interlock_not_ok_skips(self):
        ctx = self._ctx(ota_http_client=_FakeOtaClient(interlock_ok=False, interlock_reason="not idle"))
        result = C._case_ote10(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_login_failure_fails(self):
        ctx = self._ctx(_login_fn=lambda user, pw: (401, None))
        result = C._case_ote10(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class RegistryWiringTest(unittest.TestCase):
    def test_sp04_depends_on_otb01(self):
        self.assertEqual(get_case("SP-04").depends_on, "OT-B01")

    def test_judges_wired(self):
        self.assertIs(get_case("OT-B01").judge, C._case_otb01)
        self.assertIs(get_case("OT-E01").judge, C._case_ote01)
        self.assertIs(get_case("OT-E02").judge, C._case_ote02)
        self.assertIs(get_case("OT-E03").judge, C._case_ote03)
        self.assertIs(get_case("OT-E04").judge, C._case_ote04)
        self.assertIs(get_case("OT-E05").judge, C._case_ote05)
        self.assertIs(get_case("OT-E06").judge, C._case_ote06)
        self.assertIs(get_case("OT-E07").judge, C._case_ote07)
        self.assertIs(get_case("OT-E08").judge, C._case_ote08)
        self.assertIs(get_case("OT-E09").judge, C._case_ote09)
        self.assertIs(get_case("OT-E10").judge, C._case_ote10)
        self.assertIs(get_case("OT-E12").judge, C._case_ote12)
        self.assertIs(get_case("SP-04").judge, CS._case_sp04)
        self.assertIs(get_case("OT-P01").judge, C._case_otp01)
        self.assertIs(get_case("OT-P02").judge, C._case_otp02)
        self.assertIs(get_case("OT-P03").judge, C._case_otp03)
        self.assertIs(get_case("OT-P04").judge, C._case_otp04)
        self.assertIs(get_case("OT-P05").judge, C._case_otp05)

    def test_otp_dependency_wiring(self):
        self.assertEqual(get_case("OT-P02").depends_on, "OT-P01")
        self.assertEqual(get_case("OT-P03").depends_on, "OT-P01")
        self.assertEqual(get_case("OT-P04").depends_on, "OT-P01")
        self.assertIsNone(get_case("OT-P05").depends_on)


class _FakeSafetySrv(_FakeSrv):
    """_FakeSrv plus the two describe()-text accessors OT-P* reads."""

    def __init__(self, state_name="idle", fw_text="", diag_text=""):
        super().__init__(state_name=state_name)
        self._fw_text = fw_text
        self._diag_text = diag_text

    def safety_get_fw_version(self):
        return self._fw_text

    def safety_get_diag(self):
        return self._diag_text


class _FakePicoOtaClient(_FakeOtaClient):
    def __init__(self, push_result=None, phases=None, last_error=None, interlock_ok=True, interlock_reason="",
                 rollback_statuses=None):
        super().__init__(push_result=push_result, phases=phases, interlock_ok=interlock_ok, interlock_reason=interlock_reason)
        self._last_error = last_error
        self._rollback_statuses = list(rollback_statuses or ["rebooting"])
        self.rollback_called = False

    def push_pico_image(self, host, path, ap_password, timeout=None):
        self.pushed.append(path)
        return self.push_result

    def get_pico_status(self, host):
        phase = self._phases.pop(0) if len(self._phases) > 1 else self._phases[0]
        return {"phase": phase, "last_error": self._last_error}

    def rollback_pico(self, host, ap_password):
        self.rollback_called = True
        return {"ok": True, "status": "rollback_started"}

    def get_pico_rollback_status(self, host):
        status = self._rollback_statuses.pop(0) if len(self._rollback_statuses) > 1 else self._rollback_statuses[0]
        return {"ok": True, "status": status}


_FW_TEXT_A = "Pico build: aaa1111 built 2026-09-20T00:00:00Z, boot_id=4"
_FW_TEXT_B = "Pico build: bbb2222 built 2026-09-21T00:00:00Z, boot_id=5"
# These are the REAL SafetyDiag.describe() vocabulary (devices_safety.py):
# "boot reason: power-on | state armed | trip_reason 6 [...] | warn_mask
# 0x0000 | trip_mask 0x0020 | uptime ... ms". The first version of this
# file invented "boot_reason: power_on" instead, which let a parser that
# matches nothing the device module ever emits pass all of its tests.
def _diag(boot="power-on", trip_reason=0, trip_mask=0x0000):
    return (f"boot reason: {boot} | state armed | trip_reason {trip_reason} [reason] | "
            f"warn_mask 0x0000 | trip_mask 0x{trip_mask:04x} | uptime 1234 ms")


_DIAG_WATCHDOG = _diag(boot="watchdog")
_DIAG_TRIP = _diag(trip_reason=6, trip_mask=0x0020)
_DIAG_TRIP_NOT_S6A = _diag(trip_reason=2, trip_mask=0x0002)
_DIAG_NO_TRIP = _diag()
_DIAG_UNPARSEABLE = "no such field here"  # _pico_trip_pending's is-None branch: reason=None
_OVERLAP_ERROR = "Pico refused: update would overwrite its running flat image; reflash via SWD"


class Otp01Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_NO_TRIP),
            "host": "10.0.0.5", "ap_password": "secret",
            "ota_pico_image_path": "/tmp/pico.bin", "ota_pico_image_commit": "bbb2222",
            "ota_http_client": _FakePicoOtaClient(),
            "_now": lambda: 0.0, "_sleep": lambda s: None,
            "_commissioning_fn": lambda: {"same": True},
        }
        ctx.update(overrides)
        return ctx

    def test_skips_without_image_path(self):
        result = C._case_otp01(self._ctx(ota_pico_image_path=None))
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_skips_when_not_idle(self):
        result = C._case_otp01(self._ctx(srv=_FakeSafetySrv(state_name="running", fw_text=_FW_TEXT_A, diag_text=_DIAG_NO_TRIP)))
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_skips_when_trip_pending(self):
        result = C._case_otp01(self._ctx(srv=_FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_TRIP)))
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_unreadable_pico_diag_still_pushes(self):
        """_pico_trip_pending() returns None (its is-None branch) when the
        diag text is unparseable, and `if trip_pending:` treats None as
        falsy -- so, as the code stands today, OT-P01 does NOT skip or
        refuse when the Pico's trip status cannot be read; it proceeds to
        push exactly as if no trip were pending. This pins that actual
        behaviour rather than the more cautious one a reader might assume."""
        client = _FakePicoOtaClient(phases=["done"])
        ctx = self._ctx(ota_http_client=client)
        ctx["srv"] = _FakeSafetySrv(fw_text=_FW_TEXT_B, diag_text=_DIAG_UNPARSEABLE)
        result = C._case_otp01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(client.pushed, ["/tmp/pico.bin"])

    def test_interlock_not_ok_skips_before_pushing(self):
        client = _FakePicoOtaClient(interlock_ok=False, interlock_reason="not idle")
        ctx = self._ctx(ota_http_client=client)
        result = C._case_otp01(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertEqual(client.pushed, [])

    def test_push_refused_fails(self):
        client = _FakePicoOtaClient(push_result=_OtaPushResult(False, 400))
        result = C._case_otp01(self._ctx(ota_http_client=client))
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_good_relay_passes_and_stashes(self):
        client = _FakePicoOtaClient(phases=["done"])
        ctx = self._ctx(ota_http_client=client)
        ctx["srv"] = _FakeSafetySrv(fw_text=_FW_TEXT_B, diag_text=_DIAG_NO_TRIP)
        result = C._case_otp01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertIn("_otp01", ctx)
        self.assertEqual(ctx["_otp01"]["commit_after"], "bbb2222")

    def test_watchdog_boot_reason_fails(self):
        client = _FakePicoOtaClient(phases=["done"])
        ctx = self._ctx(ota_http_client=client)
        ctx["srv"] = _FakeSafetySrv(fw_text=_FW_TEXT_B, diag_text=_DIAG_WATCHDOG)
        result = C._case_otp01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_commissioning_changed_fails(self):
        client = _FakePicoOtaClient(phases=["done"])
        ctx = self._ctx(ota_http_client=client)
        ctx["srv"] = _FakeSafetySrv(fw_text=_FW_TEXT_B, diag_text=_DIAG_NO_TRIP)
        seq = iter([{"same": True}, {"same": False}])
        ctx["_commissioning_fn"] = lambda: next(seq)
        result = C._case_otp01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


    def test_running_image_overlap_refusal_is_inconclusive_not_fail(self):
        """The bench Pico runs a flat image with no two-slot bootloader, so
        firmware refuses any overlapping write (state 9). That is correct
        behaviour, not an OTA failure -- it must never read FAIL."""
        client = _FakePicoOtaClient(phases=["failed"], last_error=_OVERLAP_ERROR)
        ctx = self._ctx(ota_http_client=client)
        result = C._case_otp01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)

    def test_other_failed_phase_still_fails(self):
        client = _FakePicoOtaClient(phases=["failed"], last_error="Pico reported FAILED after UPDATE_END: crc")
        result = C._case_otp01(self._ctx(ota_http_client=client))
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_real_describe_text_watchdog_is_detected(self):
        """Negative test for the boot-reason parser against the REAL
        describe() spelling ("boot reason: watchdog"), not an invented one."""
        self.assertEqual(J.parse_diag_boot_reason(_DIAG_WATCHDOG), "watchdog")
        self.assertEqual(J.parse_diag_boot_reason(_DIAG_NO_TRIP), "power-on")
        self.assertIsNone(J.parse_diag_boot_reason("no such field here"))

    def test_multi_bit_boot_reason_still_detects_watchdog(self):
        ctx = self._ctx(ota_http_client=_FakePicoOtaClient(phases=["done"]))
        ctx["srv"] = _FakeSafetySrv(fw_text=_FW_TEXT_B, diag_text=_diag(boot="watchdog+brownout"))
        self.assertEqual(C._case_otp01(ctx).verdict, Verdict.FAIL)


class Otp02Test(unittest.TestCase):
    def test_inconclusive_when_otp01_relay_did_not_apply(self):
        ctx = {
            "srv": _FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_NO_TRIP),
            "host": "10.0.0.5", "ap_password": "secret",
            "ota_http_client": _FakePicoOtaClient(),
            "_otp01": {"commit_before": "aaa1111", "phase": "failed"},
        }
        result = C._case_otp02(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertFalse(ctx["ota_http_client"].rollback_called)

    def test_not_run_without_otp01(self):
        result = C._case_otp02({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_rollback_and_commit_restored_passes(self):
        client = _FakePicoOtaClient(rollback_statuses=["rebooting"])
        ctx = {
            "srv": _FakeSafetySrv(fw_text=_FW_TEXT_B, diag_text=_DIAG_NO_TRIP),
            "host": "10.0.0.5", "ap_password": "secret",
            "ota_http_client": client,
            "_otp01": {"commit_before": "bbb2222", "commit_after": "aaa1111", "phase": "done"},
            "_now": lambda: 0.0, "_sleep": lambda s: None,
            "_commissioning_fn": lambda: {"same": True},
        }
        result = C._case_otp02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertTrue(client.rollback_called)

    def test_rollback_status_not_rebooting_fails(self):
        client = _FakePicoOtaClient(rollback_statuses=["link_down"])
        ctx = {
            "srv": _FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_NO_TRIP),
            "host": "10.0.0.5", "ap_password": "secret",
            "ota_http_client": client,
            "_otp01": {"commit_before": "bbb2222", "commit_after": "aaa1111", "phase": "done"},
            "_now": lambda: 0.0, "_sleep": lambda s: None,
            "_commissioning_fn": lambda: {"same": True},
        }
        result = C._case_otp02(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_skips_without_ap_password(self):
        ctx = {"_otp01": {"commit_before": "x", "phase": "done"}, "ap_password": None}
        result = C._case_otp02(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)


class Otp03Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_NO_TRIP),
            "host": "10.0.0.5", "ap_password": "secret",
            "ota_pico_corrupt_image_path": "/tmp/bad_pico.bin",
            "ota_http_client": _FakePicoOtaClient(push_result=_OtaPushResult(False, 400)),
            "_now": lambda: 0.0, "_sleep": lambda s: None,
        }
        ctx.update(overrides)
        return ctx

    def test_refused_unchanged_passes(self):
        result = C._case_otp03(self._ctx())
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_unreadable_pico_diag_still_pushes(self):
        """Same is-None branch as Otp01Test.test_unreadable_pico_diag_still_
        pushes: an unparseable diag makes trip_pending None, which
        `if trip_pending:` treats as falsy, so OT-P03 proceeds to push the
        corrupt image rather than skipping or refusing outright."""
        client = _FakePicoOtaClient(push_result=_OtaPushResult(False, 400))
        ctx = self._ctx(ota_http_client=client, srv=_FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_UNPARSEABLE))
        result = C._case_otp03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(client.pushed, ["/tmp/bad_pico.bin"])

    def test_accepted_but_failed_phase_passes(self):
        client = _FakePicoOtaClient(push_result=_OtaPushResult(True, 200), phases=["failed"])
        result = C._case_otp03(self._ctx(ota_http_client=client))
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_accepted_and_done_fails(self):
        client = _FakePicoOtaClient(push_result=_OtaPushResult(True, 200), phases=["done"])
        result = C._case_otp03(self._ctx(ota_http_client=client))
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_commit_changed_despite_refusal_fails(self):
        srv_before = _FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_NO_TRIP)

        class _ChangingSrv(_FakeSafetySrv):
            def __init__(self):
                super().__init__(fw_text=_FW_TEXT_A, diag_text=_DIAG_NO_TRIP)
                self.n = 0

            def safety_get_fw_version(self):
                self.n += 1
                return _FW_TEXT_A if self.n == 1 else _FW_TEXT_B

        result = C._case_otp03(self._ctx(srv=_ChangingSrv()))
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_skips_without_image_path(self):
        result = C._case_otp03(self._ctx(ota_pico_corrupt_image_path=None))
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_overlap_refusal_is_inconclusive(self):
        client = _FakePicoOtaClient(push_result=_OtaPushResult(True, 200), phases=["failed"],
                                     last_error=_OVERLAP_ERROR)
        result = C._case_otp03(self._ctx(ota_http_client=client))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)

    def test_skips_when_trip_pending(self):
        client = _FakePicoOtaClient(push_result=_OtaPushResult(False, 400))
        result = C._case_otp03(self._ctx(
            srv=_FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_TRIP),
            ota_http_client=client,
        ))
        self.assertEqual(result.verdict, Verdict.SKIP, result.reason)
        self.assertEqual(client.pushed, [])

    def test_no_trip_pending_proceeds(self):
        result = C._case_otp03(self._ctx(
            srv=_FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_NO_TRIP),
        ))
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)


class Otp04Test(unittest.TestCase):
    def test_not_run_without_otp01(self):
        result = C._case_otp04({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_clean_relay_passes(self):
        ctx = {"_otp01": {"boot_reason": "power-on", "last_error": None}}
        result = C._case_otp04(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_watchdog_signature_fails(self):
        ctx = {"_otp01": {"boot_reason": "watchdog", "last_error": None}}
        result = C._case_otp04(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_last_error_signature_fails(self):
        ctx = {"_otp01": {"boot_reason": "power-on", "last_error": "peer did not confirm RECEIVING in time"}}
        result = C._case_otp04(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_overlap_refusal_is_inconclusive(self):
        ctx = {"_otp01": {"boot_reason": "power-on", "last_error": _OVERLAP_ERROR}}
        result = C._case_otp04(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)


class Otp05Test(unittest.TestCase):
    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_TRIP),
            "host": "10.0.0.5", "ap_password": "secret",
            "ota_pico_image_path": "/tmp/pico.bin",
            "ota_http_client": _FakePicoOtaClient(push_result=_OtaPushResult(False, 409)),
            "_clear_trip_fn": lambda: None,
        }
        ctx.update(overrides)
        return ctx

    def test_no_trip_pending_is_inconclusive(self):
        ctx = self._ctx(srv=_FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_NO_TRIP))
        result = C._case_otp05(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_refused_with_trip_pending_passes_and_clears(self):
        cleared = {"v": False}
        ctx = self._ctx(_clear_trip_fn=lambda: cleared.__setitem__("v", True))
        result = C._case_otp05(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertTrue(cleared["v"])

    def test_accepted_with_trip_pending_fails(self):
        client = _FakePicoOtaClient(push_result=_OtaPushResult(True, 200))
        ctx = self._ctx(ota_http_client=client)
        result = C._case_otp05(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_skips_when_not_idle_and_never_pushes(self):
        client = _FakePicoOtaClient(push_result=_OtaPushResult(False, 409))
        ctx = self._ctx(ota_http_client=client,
                        srv=_FakeSafetySrv(state_name="running", fw_text=_FW_TEXT_A, diag_text=_DIAG_TRIP))
        result = C._case_otp05(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertEqual(client.pushed, [])

    def test_skips_when_interlock_not_ok_and_never_pushes(self):
        client = _FakePicoOtaClient(push_result=_OtaPushResult(False, 409), interlock_ok=False,
                                     interlock_reason="a trip is pending")
        ctx = self._ctx(ota_http_client=client)
        result = C._case_otp05(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertEqual(client.pushed, [])

    def test_non_s6a_trip_is_never_cleared(self):
        """Plan section 6 rule 5: only a confirmed S6a (trip_reason 6,
        trip_mask 0x0020) may be cleared -- any other latched trip stops
        the run for a human."""
        cleared = {"v": False}
        ctx = self._ctx(srv=_FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_TRIP_NOT_S6A),
                        _clear_trip_fn=lambda: cleared.__setitem__("v", True))
        result = C._case_otp05(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertFalse(cleared["v"])

    def test_skips_without_image_path(self):
        result = C._case_otp05(self._ctx(ota_pico_image_path=None))
        self.assertEqual(result.verdict, Verdict.SKIP)


if __name__ == "__main__":
    unittest.main()
