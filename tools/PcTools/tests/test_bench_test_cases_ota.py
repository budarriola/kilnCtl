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
              enabled_after_clear=True, readiness_ok=True):
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
    def __init__(self, push_result=None, phases=None, boot_guard_recovery=False):
        self.push_result = push_result or _OtaPushResult(True)
        self._phases = list(phases or ["done"])
        self.boot_guard_recovery = boot_guard_recovery
        self.pushed = []

    def push_esp_image(self, host, path, ap_password, timeout=None):
        self.pushed.append(path)
        return self.push_result

    def get_esp_status(self, host):
        phase = self._phases.pop(0) if len(self._phases) > 1 else self._phases[0]
        return {"phase": phase}

    def get_boot_guard_status(self, host):
        return {"recovery_mode": self.boot_guard_recovery}


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
    def __init__(self, fw_build="B1", load_fault=False):
        self.fw_build = fw_build
        self.load_fault = load_fault

    def get_status(self, host):
        return {"fw_build": self.fw_build, "zones_config_load_fault": self.load_fault}


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


class Ote02Test(unittest.TestCase):
    def test_not_run_without_ote01(self):
        result = C._case_ote02({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_load_fault_fails(self):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "_ote_pre_update": {"fw_build": "B1", "zones": {"zones": [{"pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}]}},
            "ota_http_client": type("O", (), {"rollback_esp": staticmethod(lambda host, pw: {"ok": True})})(),
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
            "ota_http_client": type("O", (), {"rollback_esp": staticmethod(lambda host, pw: {"ok": True})})(),
            "zones_http_client": _FakeZonesClient(zones=gains),
            "dashboard_http_client": _FakeDashboardClient(fw_build="B1", load_fault=False),
            "_now": lambda: 0.0, "_sleep": lambda s: None,
        }
        result = C._case_ote02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_gains_changed_without_load_fault_still_fails(self):
        before = {"zones": [{"pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0}]}
        after = {"zones": [{"pid_kp": 9.0, "pid_ki": 0.1, "pid_kd": 0.0}]}
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5", "ap_password": "secret",
            "_ote_pre_update": {"fw_build": "B1", "zones": before},
            "ota_http_client": type("O", (), {"rollback_esp": staticmethod(lambda host, pw: {"ok": True})})(),
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


class RegistryWiringTest(unittest.TestCase):
    def test_sp04_depends_on_otb01(self):
        self.assertEqual(get_case("SP-04").depends_on, "OT-B01")

    def test_judges_wired(self):
        self.assertIs(get_case("OT-B01").judge, C._case_otb01)
        self.assertIs(get_case("OT-E01").judge, C._case_ote01)
        self.assertIs(get_case("OT-E02").judge, C._case_ote02)
        self.assertIs(get_case("OT-E03").judge, C._case_ote03)
        self.assertIs(get_case("OT-E12").judge, C._case_ote12)
        self.assertIs(get_case("SP-04").judge, CS._case_sp04)


if __name__ == "__main__":
    unittest.main()
