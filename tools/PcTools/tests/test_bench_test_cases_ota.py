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
import unittest.mock

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
    def __init__(self, trip_reason=None, trip_mask=None, ever_received=True):
        self.trip_reason = trip_reason
        self.trip_mask = trip_mask
        self.ever_received = ever_received


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
              enabled_after_clear=True, readiness_ok=True, relay_energized=False,
              esp_restarts=True, pico_reboots=True, trip_before=0, boot_id_unknown_polls=0,
              boot_id_after=8, diag_unreceived_polls=0):
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

        reset = {"done": False}

        def sw_reset_fn():
            reset["done"] = True
            return {"ok": True}

        def esp_uptime_fn():
            # Before the reset: long uptime. After: restarted (small) unless
            # the fake models an ESP that never actually rebooted.
            if reset["done"] and esp_restarts:
                return 3.0
            return 500.0 + (_clockstate["t"] if reset["done"] else 0.0)

        polls = {"boot": 0, "diag": 0}

        def boot_id_fn():
            # None == unknown (no FW_VERSION frame yet after the ESP restart).
            if not reset["done"]:
                return 7
            polls["boot"] += 1
            if polls["boot"] <= boot_id_unknown_polls:
                return None
            return boot_id_after if pico_reboots else 7

        def diag_fn():
            if not reset["done"]:
                # Pre-reset: received; any pre-latched trip is the test's choice.
                return _SafetyDiag(trip_reason=trip_before, trip_mask=(1 << (trip_before - 1)) if trip_before else 0)
            polls["diag"] += 1
            if polls["diag"] <= diag_unreceived_polls:
                return _SafetyDiag(trip_reason=0, trip_mask=0, ever_received=False)
            return diag

        ctx = {
            "srv": srv, "host": "192.168.4.1",
            "_sw_reset_fn": sw_reset_fn,
            "_esp_uptime_fn": esp_uptime_fn,
            "_pico_boot_id_fn": boot_id_fn,
            "ota_http_client": type("O", (), {"sw_reset": staticmethod(lambda host: {"ok": True})})(),
            "dashboard_http_client": _FakeDashboardClient(relay_energized=relay_energized),
            "_now": now, "_sleep": sleep,
            "_get_safety_status_fn": get_status_fn,
            "_get_safety_diag_fn": diag_fn,
            "_clear_trip_fn": clear_trip_fn,
            "_readiness_trip_ok_fn": lambda: readiness_ok,
        }
        return ctx

    def test_skips_when_not_idle(self):
        ctx = self._ctx(state_name="running")
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

    # ---- reset-confirmation (stale pre-reset read) tests ----

    def test_stale_pre_reset_read_is_not_judged(self):
        """The 20261001T062928Z bench failure: sw_reset returns before the
        ESP reboots, the cached telemetry still shows link up / trip 0, and
        the old case judged that as FAIL. With the ESP uptime never
        dropping, nothing may be judged -- INCONCLUSIVE, and no clear."""
        called = {"v": False}
        ctx = self._ctx(esp_restarts=False, trip_reason=0, trip_mask=0)
        ctx["_clear_trip_fn"] = lambda: called.__setitem__("v", True)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("not confirmed", result.reason)
        self.assertFalse(called["v"])
        self.assertEqual(ctx["_otb01"]["outcome"], "inconclusive_esp_not_restarted")

    def test_stale_read_with_a_trip_present_is_also_not_judged(self):
        # Even a stale trip-6 must not be cleared if the ESP never restarted.
        called = {"v": False}
        ctx = self._ctx(esp_restarts=False)
        ctx["_clear_trip_fn"] = lambda: called.__setitem__("v", True)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertFalse(called["v"])

    def test_confirmed_reset_with_s6a_passes_and_clears(self):
        called = {"v": False}
        ctx = self._ctx()
        orig = ctx["_clear_trip_fn"]

        def clear():
            called["v"] = True
            orig()

        ctx["_clear_trip_fn"] = clear
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertTrue(called["v"])
        self.assertEqual(ctx["_otb01"]["outcome"], "s6a_latched")
        self.assertTrue(ctx["_otb01"]["esp_restart_confirmed"])

    def test_confirmed_reset_without_trip_passes_without_clear(self):
        called = {"v": False}
        ctx = self._ctx(trip_reason=0, trip_mask=0)
        ctx["_clear_trip_fn"] = lambda: called.__setitem__("v", True)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertIn("no S6a latched on sw_reset", result.reason)
        self.assertFalse(called["v"], "clear_trip called although nothing was latched")
        self.assertEqual(ctx["_otb01"]["outcome"], "no_trip")
        self.assertEqual(ctx["_otb01"]["pico_boot_id_before"], 7)
        self.assertEqual(ctx["_otb01"]["pico_boot_id_after"], 8)

    def test_confirmed_reset_with_other_trip_fails_without_clear(self):
        called = {"v": False}
        ctx = self._ctx(trip_reason=3, trip_mask=0x0004)
        ctx["_clear_trip_fn"] = lambda: called.__setitem__("v", True)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertFalse(called["v"], "a non-S6a trip was cleared")

    def test_trip_that_latches_during_settle_window_is_caught(self):
        """No trip at the first read, S6a (debounced) appears a second
        later: must be treated as the S6a outcome, not 'no trip'."""
        ctx = self._ctx()
        reads = {"n": 0}

        def diag():
            reads["n"] += 1
            if reads["n"] <= 2:  # baseline read + first post-reset read
                return _SafetyDiag(trip_reason=0, trip_mask=0)
            return _SafetyDiag(trip_reason=6, trip_mask=0x0020)

        ctx["_get_safety_diag_fn"] = diag
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(ctx["_otb01"]["outcome"], "s6a_latched")

    def test_boot_id_unchanged_and_no_trip_is_inconclusive(self):
        ctx = self._ctx(trip_reason=0, trip_mask=0, pico_reboots=False)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("boot_id unchanged", result.reason)

    def test_boot_id_unchanged_but_trip_latched_is_valid_evidence(self):
        ctx = self._ctx(pico_reboots=False)  # trip 0 before the reset, trip 6 only after, same boot_id
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_trip_latched_before_reset_is_inconclusive_and_no_reset(self):
        sw_reset_called = {"v": False}
        ctx = self._ctx(trip_before=6)
        ctx["_sw_reset_fn"] = lambda: sw_reset_called.__setitem__("v", True)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("already latched", result.reason)
        self.assertFalse(sw_reset_called["v"])
        self.assertEqual(ctx["_otb01"]["outcome"], "inconclusive_trip_prelatched")

    def test_unreceived_baseline_diag_means_no_reset_is_issued(self):
        sw_reset_called = {"v": False}
        ctx = self._ctx()
        ctx["_get_safety_diag_fn"] = lambda: _SafetyDiag(trip_reason=0, trip_mask=0, ever_received=False)
        ctx["_sw_reset_fn"] = lambda: sw_reset_called.__setitem__("v", True)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertFalse(sw_reset_called["v"])

    def test_unknown_baseline_boot_id_means_no_reset_is_issued(self):
        sw_reset_called = {"v": False}
        ctx = self._ctx()
        ctx["_pico_boot_id_fn"] = lambda: None
        ctx["_sw_reset_fn"] = lambda: sw_reset_called.__setitem__("v", True)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertFalse(sw_reset_called["v"])

    def test_boot_id_never_known_after_reset_is_inconclusive_not_pass(self):
        """Placeholder boot_id 0 / protocol_version 0 after the ESP restart
        must not read as 'the Pico rebooted'."""
        ctx = self._ctx(trip_reason=0, trip_mask=0, boot_id_unknown_polls=10**6)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(ctx["_otb01"]["outcome"], "inconclusive_boot_id_unknown")

    def test_boot_id_that_becomes_known_later_is_waited_for(self):
        ctx = self._ctx(trip_reason=0, trip_mask=0, boot_id_unknown_polls=3)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(ctx["_otb01"]["pico_boot_id_after"], 8)

    def test_real_boot_id_zero_after_reset_counts_as_a_change(self):
        # 0 is a legal random boot_id: baseline 7 -> real 0 is a reboot.
        ctx = self._ctx(trip_reason=0, trip_mask=0, boot_id_after=0)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(ctx["_otb01"]["pico_boot_id_after"], 0)

    def test_diag_never_received_after_reset_is_inconclusive_not_no_trip(self):
        ctx = self._ctx(trip_reason=0, trip_mask=0, diag_unreceived_polls=10**6)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(ctx["_otb01"]["outcome"], "inconclusive_diag_not_received")

    def test_diag_unreceived_through_settle_window_is_inconclusive(self):
        ctx = self._ctx(trip_reason=0, trip_mask=0)
        reads = {"n": 0}

        def diag():
            reads["n"] += 1
            if reads["n"] <= 2:  # baseline + first post-reset read are real
                return _SafetyDiag(trip_reason=0, trip_mask=0)
            return _SafetyDiag(trip_reason=0, trip_mask=0, ever_received=False)

        ctx["_get_safety_diag_fn"] = diag
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(ctx["_otb01"]["outcome"], "inconclusive_settle_diag_not_received")

    def test_diag_that_becomes_received_later_is_waited_for(self):
        ctx = self._ctx(trip_reason=0, trip_mask=0, diag_unreceived_polls=3)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_default_boot_id_reader_treats_protocol_version_zero_as_unknown(self):
        fw = lambda pv, b: type("F", (), {"protocol_version": pv, "boot_id": b})()
        mk = lambda f: type("S", (), {"_safety": type("X", (), {"get_fw_version": staticmethod(lambda: f)})()})()
        self.assertIsNone(C._pico_boot_id_known(mk(fw(0, 0))))
        self.assertEqual(C._pico_boot_id_known(mk(fw(16, 0))), 0)
        self.assertEqual(C._pico_boot_id_known(mk(fw(16, 9))), 9)

    def test_no_baseline_means_no_reset_is_issued(self):
        sw_reset_called = {"v": False}
        ctx = self._ctx()
        ctx["_esp_uptime_fn"] = lambda: (_ for _ in ()).throw(RuntimeError("http down"))
        ctx["_sw_reset_fn"] = lambda: sw_reset_called.__setitem__("v", True)
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("http down", result.reason)
        self.assertFalse(sw_reset_called["v"])

    def test_poll_errors_are_counted_and_reported_not_swallowed(self):
        ctx = self._ctx(trip_reason=3, trip_mask=0x0004)  # ends in FAIL, reason carries errors
        inner = ctx["_get_safety_status_fn"]
        n = {"v": 0}

        def flaky():
            n["v"] += 1
            if n["v"] == 1:
                raise OSError("link flap")
            return inner()

        ctx["_get_safety_status_fn"] = flaky
        result = C._case_otb01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("poll errors 1", result.reason)
        self.assertIn("link flap", result.reason)
        self.assertEqual(ctx["_otb01"]["poll_error_count"], 1)
        self.assertIn("link flap", ctx["_otb01"]["last_poll_error"])


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

    def test_no_trip_outcome_passes(self):
        ctx = {"_otb01": {
            "outcome": "no_trip", "link_up": True, "trip_reason": 0, "trip_mask": 0,
            "clear_ok": None, "readiness_trip_ok": None,
        }}
        self.assertEqual(CS._case_sp04(ctx).verdict, Verdict.PASS)

    def test_inconclusive_outcome_propagates(self):
        ctx = {"_otb01": {
            "outcome": "inconclusive_esp_not_restarted", "link_up": False, "trip_reason": None,
            "trip_mask": None, "clear_ok": None, "readiness_trip_ok": None,
        }}
        self.assertEqual(CS._case_sp04(ctx).verdict, Verdict.INCONCLUSIVE)


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

    def push_esp_image(self, host, path, timeout=None):
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

    def rollback_esp(self, host):
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


class _OtaHttpErr(Exception):
    """Stands in for ota_http_client.OtaHttpError (carries .status)."""

    def __init__(self, status, msg="x"):
        super().__init__(msg)
        self.status = status


def _reset_err():
    """OtaHttpError(status=None) caused by URLError(ConnectionResetError), as
    ota_http_client._push_image() raises on a mid-upload close."""
    import urllib.error
    try:
        try:
            raise urllib.error.URLError(ConnectionResetError(10054, "reset"))
        except urllib.error.URLError as inner:
            raise _OtaHttpErr(None, "unreachable: reset") from inner
    except _OtaHttpErr as e:
        return e


class _RaisingPushClient(_FakeOtaClient):
    def __init__(self, exc, **kw):
        super().__init__(**kw)
        self._exc = exc

    def push_esp_image(self, host, path, timeout=None):
        self.pushed.append(path)
        raise self._exc


class _Clock:
    def __init__(self, step=0.5):
        self.t = 0.0
        self.step = step

    def __call__(self):
        self.t += self.step
        return self.t


class Ote01Test(unittest.TestCase):
    """OT-E01: push to the running app must be refused (409), no reboot."""

    def _ctx(self, **overrides):
        ctx = {
            "srv": _FakeSrv(state_name="idle"), "host": "10.0.0.5",
            "ota_image_path": "/tmp/image.bin", "_isfile_fn": lambda p: True,
            "ota_http_client": _RaisingPushClient(_OtaHttpErr(409, "refused: single-slot")),
            "_now": _Clock(), "_sleep_fn": lambda s: None,
            "_esp_uptime_fn": lambda: 100.0,
            "_crash_report_fn": lambda: {"present": False},
        }
        ctx.update(overrides)
        return ctx

    def test_skips_without_image_path(self):
        self.assertEqual(C._case_ote01(self._ctx(ota_image_path=None)).verdict, Verdict.SKIP)

    def test_skips_when_not_idle(self):
        result = C._case_ote01(self._ctx(srv=_FakeSrv(state_name="running")))
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_409_passes_and_does_not_stash_pre_update(self):
        ctx = self._ctx()
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(result.observed["refusal_form"], "http_409")
        self.assertNotIn("_ote_pre_update", ctx)

    def test_409_as_push_result_passes(self):
        ctx = self._ctx(ota_http_client=_FakeOtaClient(push_result=_OtaPushResult(False, 409)))
        self.assertEqual(C._case_ote01(ctx).verdict, Verdict.PASS)

    def test_accepted_push_fails(self):
        ctx = self._ctx(ota_http_client=_FakeOtaClient(push_result=_OtaPushResult(True, 200)))
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertEqual(result.observed["refusal_form"], "accepted")

    def test_500_fails(self):
        ctx = self._ctx(ota_http_client=_RaisingPushClient(_OtaHttpErr(500, "esp_ota_begin failed")))
        self.assertEqual(C._case_ote01(ctx).verdict, Verdict.FAIL)

    def test_connection_reset_with_live_board_passes_with_note(self):
        ctx = self._ctx(ota_http_client=_RaisingPushClient(_reset_err()))
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(result.observed["refusal_form"], "connection_closed")
        self.assertIn("note", result.observed)

    def test_connection_reset_with_dead_board_fails(self):
        uptimes = iter([100.0])

        def uptime():
            return next(uptimes)  # second read raises StopIteration -> unreadable

        ctx = self._ctx(ota_http_client=_RaisingPushClient(_reset_err()),
                        _esp_uptime_fn=uptime)
        self.assertEqual(C._case_ote01(ctx).verdict, Verdict.FAIL)

    def test_reboot_after_409_fails(self):
        uptimes = iter([100.0, 3.0])
        ctx = self._ctx(_esp_uptime_fn=lambda: next(uptimes))
        result = C._case_ote01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("rebooted", result.reason)

    def test_new_crash_record_fails(self):
        reports = iter([{"present": False}, {"present": True, "acknowledged": False}])
        ctx = self._ctx(_crash_report_fn=lambda: next(reports))
        self.assertEqual(C._case_ote01(ctx).verdict, Verdict.FAIL)

    def test_slow_refusal_passes_but_over_ceiling_fails(self):
        uptimes = iter([100.0, 130.0])
        r = C._case_ote01(self._ctx(_now=_Clock(step=30.0), _esp_uptime_fn=lambda: next(uptimes)))
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertGreater(r.observed["elapsed_s"], 5.0)
        uptimes = iter([100.0, 230.0])
        self.assertEqual(C._case_ote01(self._ctx(_now=_Clock(step=130.0), _esp_uptime_fn=lambda: next(uptimes))).verdict,
                         Verdict.FAIL)

    def test_early_reboot_with_larger_uptime_after_fails(self):
        """uptime 20 -> 40 over a 60 s push: after > before but the board
        restarted early in the push."""
        uptimes = iter([20.0, 40.0])
        ctx = self._ctx(_now=_Clock(step=60.0), _esp_uptime_fn=lambda: next(uptimes))
        r = C._case_ote01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("rebooted", r.reason)

    def test_no_such_file_error_is_not_connection_closed(self):
        ctx = self._ctx(ota_http_client=_RaisingPushClient(_OtaHttpErr(None, "no such file: x")))
        r = C._case_ote01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertNotEqual(r.observed["refusal_form"], "connection_closed")

    def test_type_error_is_not_connection_closed(self):
        ctx = self._ctx(ota_http_client=_RaisingPushClient(TypeError("bad arg")))
        r = C._case_ote01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertNotEqual(r.observed["refusal_form"], "connection_closed")

    def test_skips_when_image_is_not_a_file(self):
        r = C._case_ote01(self._ctx(_isfile_fn=lambda p: False))
        self.assertEqual(r.verdict, Verdict.SKIP)

    def test_interlock_not_ok_skips_before_pushing(self):
        """Plan doc section 6 rule 1: the interlock is confirmed ok
        immediately before the push."""
        client = _FakeOtaClient(interlock_ok=False, interlock_reason="kiln is not idle")
        result = C._case_ote01(self._ctx(ota_http_client=client))
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("kiln is not idle", result.reason)
        self.assertEqual(client.pushed, [], "push_esp_image was called despite a not-ok interlock")

    def test_interlock_read_error_skips(self):
        client = _FakeOtaClient()
        client.get_interlock = lambda host: (_ for _ in ()).throw(RuntimeError("timeout"))
        result = C._case_ote01(self._ctx(ota_http_client=client))
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("could not read /api/ota/interlock", result.reason)

    def test_ote02_not_run_after_ote01(self):
        ctx = self._ctx()
        C._case_ote01(ctx)
        self.assertEqual(C._case_ote02(ctx).verdict, Verdict.NOT_RUN)


class JudgeOtaSelfPushRefusedTest(unittest.TestCase):
    def _judge(self, **kw):
        args = dict(refusal_form="http_409", status_code=409, elapsed_s=0.5, uptime_before=10.0,
                    uptime_after=12.0, crash_before={"present": False}, crash_after={"present": False},
                    interlock_ok_after=True)
        args.update(kw)
        return J.judge_ota_self_push_refused(**args)

    def test_409_passes(self):
        self.assertEqual(self._judge().verdict, Verdict.PASS)

    def test_closed_passes_with_note(self):
        r = self._judge(refusal_form="connection_closed", status_code=None)
        self.assertEqual(r.verdict, Verdict.PASS)
        self.assertIn("note", r.observed)

    def test_each_violation_fails(self):
        for kw in (dict(refusal_form="accepted", status_code=200), dict(refusal_form=None, status_code=None),
                   dict(status_code=500), dict(elapsed_s=121.0), dict(elapsed_s=None),
                   dict(uptime_after=5.0), dict(uptime_before=None), dict(uptime_after=None),
                   dict(crash_after={"present": True}), dict(crash_before=None),
                   dict(interlock_ok_after=False), dict(interlock_ok_after=None)):
            self.assertEqual(self._judge(**kw).verdict, Verdict.FAIL, kw)


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
            "_isfile_fn": lambda p: True, "_now": _Clock(1.0), "_sleep_fn": lambda s: None,
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
            "_isfile_fn": lambda p: True, "_now": _Clock(1.0), "_sleep_fn": lambda s: None,
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
            "_isfile_fn": lambda p: True, "_now": _Clock(1.0), "_sleep_fn": lambda s: None,
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



class _FailThenOk:
    """Callable: raises TimeoutError for its first ``fails`` calls, then
    returns ``value`` (the board still draining a refused body, then serving)."""

    def __init__(self, fails, value):
        self.fails = fails
        self.value = value
        self.calls = 0

    def __call__(self, *a, **k):
        self.calls += 1
        if self.calls <= self.fails:
            raise TimeoutError("timed out")
        return self.value


def _partitions_after_first(fail_calls, always_fail_after_first=False):
    """Partition client: first call answers; calls in ``fail_calls`` (1-based)
    raise; with ``always_fail_after_first`` every later call raises."""
    state = {"n": 0}

    class _P:
        def get_partitions(self, host):
            state["n"] += 1
            if state["n"] > 1 and (always_fail_after_first or state["n"] in fail_calls):
                raise TimeoutError("draining")
            return {"running": "app"}

    return _P(), state


def _dashboard_after_first(fail_calls, always_fail_after_first=False):
    state = {"n": 0}

    class _D:
        def get_status(self, host):
            state["n"] += 1
            if state["n"] > 1 and (always_fail_after_first or state["n"] in fail_calls):
                raise TimeoutError("draining")
            return {"fw_build": "B1"}

    return _D(), state


class RefusalDrainSettleTest(unittest.TestCase):
    """Post-refusal readbacks retry through the firmware's body-drain window."""

    def _uptime_seq(self, seq):
        it = iter(seq)

        def fn():
            try:
                v = next(it)
            except StopIteration:
                raise TimeoutError("draining")
            if v is None:
                raise TimeoutError("draining")
            return v
        return fn

    def test_ote01_uptime_unreadable_then_ok_passes(self):
        sleeps = []
        ctx = Ote01Test()._ctx(_esp_uptime_fn=self._uptime_seq([100.0, None, None, None, 104.0]),
                               _sleep_fn=sleeps.append, _now=_Clock(1.0))
        r = C._case_ote01(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(len(sleeps), 3)
        self.assertGreater(r.observed["uptime_elapsed_s"], r.observed["elapsed_s"])

    def test_ote01_continuity_counts_settle_wait(self):
        """uptime 100 -> 101 passes on the push's own ~1 s alone, but ~10 s
        of settle wait passed before it was readable: a reboot hidden by the
        drain must FAIL once that wait counts."""
        seq = [100.0] + [None] * 10 + [101.0]
        ctx = Ote01Test()._ctx(_esp_uptime_fn=self._uptime_seq(seq), _now=_Clock(1.0))
        r = C._case_ote01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("rebooted", r.reason)
        self.assertLess(r.observed["elapsed_s"], 3.0)
        self.assertGreater(r.observed["uptime_elapsed_s"], 10.0)

    def test_settled_read_attempt_cap_with_frozen_clock(self):
        calls = {"n": 0}

        def fn():
            calls["n"] += 1
            raise TimeoutError("x")

        ctx = {"_now": lambda: 0.0, "_sleep_fn": lambda s: None}
        self.assertEqual(C._settled_read(ctx, fn), (None, None))
        self.assertLessEqual(calls["n"], 42)

    def test_settled_read_stamps_attempt_start(self):
        ticks = iter(range(100, 200))
        vals = iter([None, None, "ok"])

        def fn():
            v = next(vals)
            if v is None:
                raise TimeoutError("x")
            return v

        value, at = C._settled_read({"_now": lambda: next(ticks), "_sleep_fn": lambda s: None}, fn)
        self.assertEqual(value, "ok")
        # tick 100 = deadline, 101 = start of attempt 2 (fails), 102 = deadline check, 103 = start of success
        self.assertEqual(at, 103)

    def test_ote01_all_uptime_reads_raise_fails_unreadable(self):
        sleeps = []
        ctx = Ote01Test()._ctx(_esp_uptime_fn=self._uptime_seq([100.0]), _sleep_fn=sleeps.append,
                               _now=_Clock(1.0))
        r = C._case_ote01(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("unreadable", r.reason)
        self.assertGreaterEqual(len(sleeps), 15)

    def _retry_case(self, fn, cls):
        part, pstate = _partitions_after_first({2, 3, 4})
        dash, dstate = _dashboard_after_first({2, 3})
        r = fn(cls()._ctx(partition_http_client=part, dashboard_http_client=dash))
        self.assertEqual(r.verdict, Verdict.PASS, (fn.__name__, r.reason))
        self.assertGreaterEqual(pstate["n"], 5)
        self.assertGreaterEqual(dstate["n"], 4)

    def test_ote03_retries_then_passes(self):
        self._retry_case(C._case_ote03, Ote03Test)

    def test_ote04_retries_then_passes(self):
        self._retry_case(C._case_ote04, Ote04Test)

    def test_ote05_retries_then_passes(self):
        self._retry_case(C._case_ote05, Ote05Test)

    def test_all_after_reads_raise_fails_unreadable(self):
        for fn, cls in ((C._case_ote03, Ote03Test), (C._case_ote04, Ote04Test), (C._case_ote05, Ote05Test)):
            part, _ = _partitions_after_first(set(), always_fail_after_first=True)
            r = fn(cls()._ctx(partition_http_client=part))
            self.assertEqual(r.verdict, Verdict.FAIL, fn.__name__)
            self.assertIn("unreadable", r.reason)
            self.assertIn("running_after", r.reason)

    def test_ote09_retries_then_passes(self):
        part, _ = _partitions_after_first({2, 3})
        r = C._case_ote09(Ote09Test()._ctx(partition_http_client=part))
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)

    def test_ote12_retries_running_read(self):
        ctx = {"_ote01": {}, "host": "h", "_now": _Clock(1.0), "_sleep_fn": lambda s: None,
               "partition_http_client": unittest.mock.Mock(get_partitions=_FailThenOk(3, {"running": "app"}))}
        self.assertEqual(C._case_ote12(ctx).verdict, Verdict.PASS)


class RefusedImagePushLocalErrorTest(unittest.TestCase):
    """OT-E03/E04/E05 (and E09): a missing file SKIPs; a non-HTTP push
    exception FAILs as error:<ExcName> and is never a refusal."""

    CASES = (
        (C._case_ote03, Ote03Test),
        (C._case_ote04, Ote04Test),
        (C._case_ote05, Ote05Test),
    )

    def test_missing_file_skips_without_pushing(self):
        for fn, cls in self.CASES:
            client = _FakeOtaClient(push_result=_OtaPushResult(False, 400))
            r = fn(cls()._ctx(_isfile_fn=lambda p: False, ota_http_client=client))
            self.assertEqual(r.verdict, Verdict.SKIP, fn.__name__)
            self.assertIn("not a file", r.reason)
            self.assertEqual(client.pushed, [])

    def test_missing_file_skips_ote09(self):
        client = _FakeOtaClient()
        r = C._case_ote09(Ote09Test()._ctx(_isfile_fn=lambda p: False, ota_http_client=client))
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertEqual(client.pushed, [])

    def test_local_exception_fails_not_refused(self):
        for exc in (FileNotFoundError("nope"), TypeError("bad"), _OtaHttpErr(None, "no such file: x")):
            for fn, cls in self.CASES:
                r = fn(cls()._ctx(ota_http_client=_RaisingPushClient(exc)))
                self.assertEqual(r.verdict, Verdict.FAIL, (fn.__name__, exc))
                self.assertIn("error:" + type(exc).__name__, r.reason)

    def test_transport_reset_is_error_for_authenticated_cases(self):
        for fn, cls in self.CASES:
            r = fn(cls()._ctx(ota_http_client=_RaisingPushClient(_reset_err())))
            self.assertEqual(r.verdict, Verdict.FAIL, fn.__name__)
            self.assertIn("error:ConnectionResetError", r.reason)

    def test_session_probe_refusal_is_error_not_refusal(self):
        from kilnctrl.ota_http_client import OtaSessionProbeError
        for code in (401, 403):
            self.assertEqual(C._push_refusal_outcome(OtaSessionProbeError("probe", code)),
                             (False, f"error:session_probe_{code}"))
            for fn, cls in self.CASES:
                r = fn(cls()._ctx(ota_http_client=_RaisingPushClient(OtaSessionProbeError("probe", code))))
                self.assertEqual(r.verdict, Verdict.FAIL, (fn.__name__, code))
                self.assertIn(f"error:session_probe_{code}", r.reason)

    def test_session_probe_refusal_fails_ote07_ote08(self):
        from kilnctrl.ota_http_client import OtaSessionProbeError
        helper = Ote07Ote08Test()
        for fn, key, st in ((C._case_ote07, "_exec_state_fn", "running"),
                            (C._case_ote08, "_autotune_state_fn", "stepping")):
            ctx = helper._ctx(st, key, ota_http_client=_RaisingPushClient(OtaSessionProbeError("probe", 401), interlock_ok=False, interlock_reason="not idle"))
            self.assertEqual(fn(ctx).verdict, Verdict.FAIL, fn.__name__)

    def test_http_status_exception_is_a_refusal(self):
        for fn, cls in self.CASES:
            r = fn(cls()._ctx(ota_http_client=_RaisingPushClient(_OtaHttpErr(400, "bad image"))))
            self.assertEqual(r.verdict, Verdict.PASS, (fn.__name__, r.reason))

    def test_ote09_reset_is_refusal_but_local_error_fails(self):
        def raiser(e):
            return lambda: (_ for _ in ()).throw(e)

        r = C._case_ote09(Ote09Test()._ctx(_push_no_credential_fn=raiser(_reset_err())))
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        r = C._case_ote09(Ote09Test()._ctx(_push_no_credential_fn=raiser(FileNotFoundError("x"))))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("error:FileNotFoundError", r.reason)


class Ote07Ote08DefaultHeatTest(unittest.TestCase):
    """allow_heat=True and no injected starter: the cases start their own
    heat via cases_heat/cases_autotune helpers and always tear it down."""

    def setUp(self):
        from kilnctrl.bench_test import cases_heat as H
        from kilnctrl.bench_test import cases_autotune as A
        self.H, self.A = H, A
        self.state = {"exec": "idle", "at": "idle", "stops": 0, "starts": 0, "relays_off": True, "stop_works": True}
        st = self.state

        class _Prof:
            def get_exec_status(_self):
                return type("S", (), {"state_name": st["exec"]})()

        class _At:
            def get_status(_self):
                return type("S", (), {"state_name": st["at"]})()

            def start(_self, zone, method, step_duty_or_setpoint_c):
                st["starts"] += 1
                st["at"] = "stepping"
                return True, ""

        self.srv = type("Srv", (), {"_profiles": _Prof(), "_autotune": _At()})()
        clock = {"t": 0.0}

        def now():
            clock["t"] += 1.0
            return clock["t"]

        def start_profile(ctx, zone_mask, **kw):
            st["starts"] += 1
            st["exec"] = "running"
            return True, "", 20.0

        def cleanup_profile(ctx):
            st["stops"] += 1
            if st["stop_works"]:
                st["exec"] = "idle"

        def cleanup_at(ctx):
            st["stops"] += 1
            if st["stop_works"]:
                st["at"] = "aborted"

        patches = [
            unittest.mock.patch.object(H, "_start_bench_profile", start_profile),
            unittest.mock.patch.object(H, "_cleanup_bench_profile", cleanup_profile),
            unittest.mock.patch.object(A, "_cleanup_autotune", cleanup_at),
            unittest.mock.patch.object(A, "_relays_off", lambda ctx: st["relays_off"]),
            unittest.mock.patch.object(A, "_at_preflight", lambda ctx: (True, "")),
            unittest.mock.patch.object(A, "_ramp_assist_enabled", lambda ctx: (False, "")),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        self.now = now

    def _ctx(self, kind, **overrides):
        st = self.state
        key, getter = (("_exec_state_fn", lambda: st["exec"]) if kind == "e07"
                       else ("_autotune_state_fn", lambda: st["at"]))
        ctx = {
            "host": "10.0.0.5", "ota_image_path": "/tmp/image.bin", "srv": self.srv, "allow_heat": True, "ota_allow_heat": True,
            "ota_http_client": _FakeOtaClient(push_result=_OtaPushResult(False, 409), interlock_ok=False,
                                              interlock_reason="not idle"),
            key: getter, "_now": self.now, "_sleep_fn": lambda s: None,
        }
        ctx.update(overrides)
        return ctx

    def test_ote07_default_starter_and_stopper_pass(self):
        ctx = self._ctx("e07")
        r = C._case_ote07(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual((self.state["starts"], self.state["stops"]), (1, 1))
        self.assertNotIn("_tainted", ctx)

    def test_ote08_default_starter_and_stopper_pass(self):
        ctx = self._ctx("e08")
        r = C._case_ote08(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual((self.state["starts"], self.state["stops"]), (1, 1))

    def test_stopper_runs_in_finally_when_push_raises(self):
        class Boom(BaseException):
            pass

        def push():
            raise Boom()

        for kind, fn in (("e07", C._case_ote07), ("e08", C._case_ote08)):
            self.state.update(stops=0, exec="idle", at="idle")
            with self.assertRaises(Boom):
                fn(self._ctx(kind, _push_fn=push))
            self.assertEqual(self.state["stops"], 1, kind)

    def test_allow_heat_false_skips_with_operator_hint(self):
        for kind, fn in (("e07", C._case_ote07), ("e08", C._case_ote08)):
            r = fn(self._ctx(kind, allow_heat=False))
            self.assertEqual(r.verdict, Verdict.SKIP)
            self.assertIn("allow_heat not set; OT-E07/E08 start their own heat", r.reason)
            self.assertEqual(self.state["starts"], 0)

    def test_ota_allow_heat_unset_skips_even_with_allow_heat(self):
        for kind, fn in (("e07", C._case_ote07), ("e08", C._case_ote08)):
            r = fn(self._ctx(kind, ota_allow_heat=False))
            self.assertEqual(r.verdict, Verdict.SKIP)
            self.assertIn("ota_allow_heat not set", r.reason)
            self.assertEqual(self.state["starts"], 0)

    def test_start_wait_timeout_confirmed_stopped_skips_after_teardown(self):
        # profile "starts" but never reaches running: teardown must still run.
        with unittest.mock.patch.object(self.H, "_start_bench_profile", lambda ctx, zone_mask, **kw: (True, "", 20.0)):
            r = C._case_ote07(self._ctx("e07"))
        self.assertEqual(r.verdict, Verdict.SKIP, r.reason)
        self.assertEqual(self.state["stops"], 1)

    def test_start_wait_timeout_unconfirmed_stop_fails_and_taints(self):
        self.state["stop_works"] = False
        ctx = self._ctx("e07")

        def start(c, zone_mask, **kw):
            self.state["exec"] = "starting"  # accepted, never reaches running
            return True, "", 20.0

        with unittest.mock.patch.object(self.H, "_start_bench_profile", start):
            r = C._case_ote07(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("could NOT be confirmed stopped", r.reason)
        self.assertTrue(ctx.get("_tainted"))

    def test_autotune_start_raising_after_send_runs_teardown(self):
        st = self.state

        def start(zone, method, step_duty_or_setpoint_c):
            st["at"] = "stepping"  # board accepted it, then the call raised
            raise OSError("link dropped")

        self.srv._autotune.start = start
        ctx = self._ctx("e08")
        r = C._case_ote08(ctx)
        self.assertEqual(st["stops"], 1)
        self.assertEqual(r.verdict, Verdict.SKIP, r.reason)
        st["stop_works"] = False
        st["stops"] = 0
        st["at"] = "idle"
        ctx = self._ctx("e08")
        r = C._case_ote08(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(ctx.get("_tainted"))

    def test_base_exception_during_start_wait_runs_teardown_and_reraises(self):
        class Boom(BaseException):
            pass

        def sleep(_s):
            raise Boom()

        # never reaches running, so the wait sleeps and the sleep raises
        with unittest.mock.patch.object(self.H, "_start_bench_profile", lambda c, zone_mask, **kw: (True, "", 20.0)):
            with self.assertRaises(Boom):
                C._case_ote07(self._ctx("e07", _sleep_fn=sleep))
        self.assertEqual(self.state["stops"], 1)

    def test_tainted_run_skips_both_cases_without_starting_or_touching_ramp_assist(self):
        writes = []
        with unittest.mock.patch.object(self.A, "_ramp_assist_enabled", lambda ctx: (True, "")),                 unittest.mock.patch.object(self.A, "_ramp_assist_set", lambda ctx, en: writes.append(en) or True):
            for kind, fn in (("e07", C._case_ote07), ("e08", C._case_ote08)):
                r = fn(self._ctx(kind, _tainted=True))
                self.assertEqual(r.verdict, Verdict.SKIP, kind)
                self.assertIn("tainted", r.reason)
        self.assertEqual((self.state["starts"], self.state["stops"]), (0, 0))
        self.assertEqual(writes, [])

    def test_busy_executor_skips_without_teardown(self):
        writes = []
        for p in (unittest.mock.patch.object(self.A, "_ramp_assist_enabled", lambda ctx: (True, "")),
                  unittest.mock.patch.object(self.A, "_ramp_assist_set", lambda ctx, en: writes.append(en) or True)):
            p.start()
            self.addCleanup(p.stop)
        for busy in ("paused", "faulted"):
            self.state.update(exec=busy, at="idle", starts=0, stops=0)
            r = C._case_ote07(self._ctx("e07", _exec_state_fn=lambda: "idle_not_expected"))
            self.assertEqual(r.verdict, Verdict.SKIP, busy)
            self.assertIn(repr(busy), r.reason)
            r = C._case_ote08(self._ctx("e08"))
            self.assertEqual(r.verdict, Verdict.SKIP, busy)
            self.assertIn(repr(busy), r.reason)
            self.assertEqual((self.state["starts"], self.state["stops"]), (0, 0), busy)
        self.state.update(exec="idle", at="stepping", starts=0, stops=0)
        r = C._case_ote08(self._ctx("e08", _autotune_state_fn=lambda: "idle"))
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertEqual(self.state["stops"], 0)
        self.assertEqual(writes, [])

    def test_ote07_skips_when_autotune_active_without_teardown(self):
        self.state.update(exec="idle", at="stepping", starts=0, stops=0)
        r = C._case_ote07(self._ctx("e07"))
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertIn("autotune", r.reason)
        self.assertEqual((self.state["starts"], self.state["stops"]), (0, 0))

    def test_refused_start_does_no_case_level_teardown(self):
        with unittest.mock.patch.object(self.H, "_start_bench_profile", lambda c, zone_mask, **kw: (False, "preflight no", None)):
            r = C._case_ote07(self._ctx("e07"))
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertEqual(self.state["stops"], 0)

    def test_ote08_refused_start_does_no_case_level_teardown(self):
        with unittest.mock.patch.object(self.A, "_at_preflight", lambda ctx: (False, "preflight no")):
            r = C._case_ote08(self._ctx("e08"))
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertIn("preflight no", r.reason)
        self.assertEqual((self.state["starts"], self.state["stops"]), (0, 0))
        st = self.state
        self.srv._autotune.start = lambda zone, method, step_duty_or_setpoint_c: (False, "nope")
        r = C._case_ote08(self._ctx("e08"))
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertIn("nope", r.reason)
        self.assertEqual(st["stops"], 0)

    def test_failed_stop_fails_and_taints(self):
        for kind, fn in (("e07", C._case_ote07), ("e08", C._case_ote08)):
            self.state.update(stop_works=False, exec="idle", at="idle")
            ctx = self._ctx(kind)
            r = fn(ctx)
            self.assertEqual(r.verdict, Verdict.FAIL, kind)
            self.assertIn("could NOT be confirmed stopped", r.reason)
            self.assertTrue(ctx.get("_tainted"))

    def test_relays_still_on_after_stop_fails(self):
        self.state["relays_off"] = False
        ctx = self._ctx("e07")
        r = C._case_ote07(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(ctx.get("_tainted"))

    def test_injected_start_fn_takes_precedence(self):
        calls = []
        ctx = self._ctx("e07", _start_state_fn=lambda: (calls.append("s"), self.state.__setitem__("exec", "running")),
                        _stop_state_fn=lambda: calls.append("x"))
        r = C._case_ote07(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(calls, ["s", "x"])
        self.assertEqual(self.state["starts"], 0)


class JudgeOtaPushRefusedUnreadableTest(unittest.TestCase):
    def test_none_readback_is_unreadable(self):
        r = J.judge_ota_push_refused(True, "app", None, "B1", "B1")
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("unreadable", r.reason)
        self.assertIn("running_after", r.reason)

    def test_real_change_still_fails(self):
        r = J.judge_ota_push_refused(True, "app", "recovery", "B1", "B1")
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertNotIn("unreadable", r.reason)

    def test_push_error_fails(self):
        r = J.judge_ota_push_refused(False, "app", "app", "B1", "B1", push_error="error:TypeError")
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("error:TypeError", r.reason)


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

    def test_ote07_state_after_retries_through_drain_window(self):
        seq = iter(["running", None, None, "running"])

        def state_fn():
            v = next(seq)
            if v is None:
                raise TimeoutError("draining")
            return v

        sleeps = []
        ctx = self._ctx("running", "_exec_state_fn", _exec_state_fn=state_fn,
                        _now=_Clock(1.0), _sleep_fn=sleeps.append)
        r = C._case_ote07(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(len(sleeps), 2)

    def test_ote07_ote08_non_http_push_error_fails_early(self):
        for fn, key, st in ((C._case_ote07, "_exec_state_fn", "running"),
                            (C._case_ote08, "_autotune_state_fn", "stepping")):
            for exc in (FileNotFoundError("x"), _reset_err()):
                ctx = self._ctx(st, key, ota_http_client=_RaisingPushClient(exc))
                r = fn(ctx)
                self.assertEqual(r.verdict, Verdict.FAIL, (fn.__name__, exc))
                self.assertIn("error:", r.reason)

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
            # OT-E09 only runs with web auth confirmed on (an unauthenticated
            # push with auth off would really flash the board); default the
            # fake probe to "on" so the existing behavior-under-test tests
            # below don't need to know about this gate.
            "_web_auth_enabled_fn": lambda: True,
            "_isfile_fn": lambda p: True, "_now": _Clock(1.0), "_sleep_fn": lambda s: None,
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

    def test_skips_when_web_auth_is_off(self):
        """With web auth off, POST /api/ota/esp is unauthenticated by design
        (ROUTE_TIER_ADMIN alone, same as every other ADMIN route once auth is
        off) -- pushing here would really flash the board instead of
        demonstrating a refusal, so this case must SKIP rather than push."""
        push_fn = unittest.mock.Mock()
        ctx = self._ctx(_web_auth_enabled_fn=lambda: False, _push_no_credential_fn=push_fn)
        result = C._case_ote09(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
        push_fn.assert_not_called()

    def test_skips_when_auth_status_unreadable(self):
        def raising():
            raise RuntimeError("unreachable")
        result = C._case_ote09(self._ctx(_web_auth_enabled_fn=raising))
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
            "_esp_uptime_fn": lambda: 100.0, "_crash_report_fn": lambda: {"present": False},
            "_now": _Clock(1.0), "_sleep_fn": lambda s: None,
        }
        ctx.update(overrides)
        return ctx

    def test_skips_without_credentials(self):
        result = C._case_ote10(self._ctx(web_user_username=None))
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_admin_ok_user_refused_passes(self):
        def push_with_session(cookie):
            return _OtaPushResult(False, 409) if cookie == "sid-admin" else _OtaPushResult(False, 403)

        ctx = self._ctx(_push_with_session_fn=push_with_session)
        result = C._case_ote10(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_admin_refused_fails(self):
        ctx = self._ctx(_push_with_session_fn=lambda cookie: _OtaPushResult(False, 403))
        result = C._case_ote10(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_admin_ok_push_still_passes_on_recovery_image(self):
        def push_with_session(cookie):
            return _OtaPushResult(True, 200) if cookie == "sid-admin" else _OtaPushResult(False, 401)

        result = C._case_ote10(self._ctx(_push_with_session_fn=push_with_session))
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def _transport_ctx(self, uptimes, crashes, user_exc=True):
        def push(cookie):
            if cookie == "sid-admin" or user_exc:
                raise _reset_err()
            return _OtaPushResult(False, 403)

        u, c = iter(uptimes), iter(crashes)
        return self._ctx(_push_with_session_fn=push, _esp_uptime_fn=lambda: next(u),
                         _crash_report_fn=lambda: next(c))

    def test_admin_connection_closed_with_live_board_passes_with_note(self):
        ctx = self._transport_ctx([10.0, 12.0], [{"p": 0}, {"p": 0}], user_exc=False)
        r = C._case_ote10(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertIn("note", r.observed)

    def test_admin_connection_closed_with_reboot_fails(self):
        ctx = self._transport_ctx([10.0, 2.0], [{"p": 0}, {"p": 0}], user_exc=False)
        self.assertEqual(C._case_ote10(ctx).verdict, Verdict.FAIL)

    def test_admin_connection_closed_with_new_crash_fails(self):
        ctx = self._transport_ctx([10.0, 12.0], [{"p": 0}, {"p": 1}], user_exc=False)
        self.assertEqual(C._case_ote10(ctx).verdict, Verdict.FAIL)

    def test_admin_connection_closed_unreadable_fails(self):
        ctx = self._ctx(_push_with_session_fn=lambda cookie: (_ for _ in ()).throw(_reset_err()),
                        _esp_uptime_fn=lambda: None, _crash_report_fn=lambda: None)
        self.assertEqual(C._case_ote10(ctx).verdict, Verdict.FAIL)

    def test_admin_early_reboot_with_larger_uptime_fails(self):
        ctx = self._transport_ctx([20.0, 40.0], [{"p": 0}, {"p": 0}], user_exc=False)
        ctx["_now"] = _Clock(step=60.0)
        self.assertEqual(C._case_ote10(ctx).verdict, Verdict.FAIL)

    def test_admin_non_transport_error_fails(self):
        for exc in (_OtaHttpErr(None, "no such file: x"), TypeError("bad")):
            ctx = self._ctx(_push_with_session_fn=lambda cookie, e=exc: (_ for _ in ()).throw(e))
            self.assertEqual(C._case_ote10(ctx).verdict, Verdict.FAIL)

    def test_user_transport_error_fails(self):
        ctx = self._transport_ctx([10.0, 12.0], [{"p": 0}, {"p": 0}], user_exc=True)
        self.assertEqual(C._case_ote10(ctx).verdict, Verdict.FAIL)

    def test_user_getting_409_fails(self):
        """409 for the user tier means it passed the ADMIN gate."""
        ctx = self._ctx(_push_with_session_fn=lambda cookie: _OtaPushResult(False, 409))
        self.assertEqual(C._case_ote10(ctx).verdict, Verdict.FAIL)

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

    def push_pico_image(self, host, path, timeout=None):
        self.pushed.append(path)
        return self.push_result

    def get_pico_status(self, host):
        phase = self._phases.pop(0) if len(self._phases) > 1 else self._phases[0]
        return {"phase": phase, "last_error": self._last_error}

    def rollback_pico(self, host):
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

    def test_unreadable_pico_diag_is_inconclusive_not_a_push(self):
        """_pico_trip_pending() returns None (its is-None branch) when the
        diag text is unparseable. An unreadable trip status must not be
        treated as "no trip" (the same "unknown is not negative" rule as
        SP-05's link-down handling) -- OT-P01 now refuses to push and
        reports INCONCLUSIVE rather than proceeding as if the Pico were
        confirmed clear."""
        client = _FakePicoOtaClient(phases=["done"])
        ctx = self._ctx(ota_http_client=client)
        ctx["srv"] = _FakeSafetySrv(fw_text=_FW_TEXT_B, diag_text=_DIAG_UNPARSEABLE)
        result = C._case_otp01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(client.pushed, [])

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

    def test_unreadable_pico_diag_is_inconclusive_not_a_push(self):
        """Same is-None branch as Otp01Test.test_unreadable_pico_diag_is_
        inconclusive_not_a_push: an unparseable diag makes trip_pending
        None, which must not be treated as "no trip" -- OT-P03 now refuses
        to push the corrupt image and reports INCONCLUSIVE instead."""
        client = _FakePicoOtaClient(push_result=_OtaPushResult(False, 400))
        ctx = self._ctx(ota_http_client=client, srv=_FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_UNPARSEABLE))
        result = C._case_otp03(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(client.pushed, [])

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
        self.assertIn("no trip was pending", result.reason)

    def test_unreadable_trip_state_skips_and_never_pushes(self):
        """A diag that fails to parse must read distinctly from "no trip
        pending" -- OT-P05 used to conflate the two (both falsy) and
        proceed as though the trip precondition simply wasn't met, when in
        fact the trip state was never determined at all. This must SKIP,
        name the unreadability in its reason, and never attempt the push."""
        client = _FakePicoOtaClient(push_result=_OtaPushResult(False, 409))
        ctx = self._ctx(ota_http_client=client,
                         srv=_FakeSafetySrv(fw_text=_FW_TEXT_A, diag_text=_DIAG_UNPARSEABLE))
        result = C._case_otp05(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP, result.reason)
        self.assertIn("trip state unreadable", result.reason)
        self.assertEqual(client.pushed, [])

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


class PidGainsMatchTest(unittest.TestCase):
    """GET /api/zones moved pid gains from %.4f to %.9g (2026-09-28); an OTA
    push or rollback across that change must not read as changed gains."""

    def test_format_straddle_is_a_match(self):
        before = {0: {"pid_kp": 5.0, "pid_ki": 0.6, "pid_kd": 0.02}}
        after = {0: {"pid_kp": 5.0, "pid_ki": 0.600000024, "pid_kd": 0.0199999996}}
        self.assertTrue(J.pid_gains_match(before, after))

    def test_small_ki_rounded_by_old_format_is_a_match(self):
        self.assertTrue(J.pid_gains_match({0: {"pid_ki": 0.0}}, {0: {"pid_ki": 3.39999995e-05}}))

    def test_default_gains_replacing_tuned_is_a_mismatch(self):
        self.assertFalse(J.pid_gains_match({0: {"pid_ki": 0.6}}, {0: {"pid_ki": 0.5}}))

    def test_zone_set_or_none_mismatch(self):
        self.assertFalse(J.pid_gains_match({0: {"pid_ki": 0.6}}, {1: {"pid_ki": 0.6}}))
        self.assertFalse(J.pid_gains_match({0: {"pid_ki": 0.6}}, None))
        self.assertTrue(J.pid_gains_match(None, None))

    def test_judge_ota_rollback_passes_across_format_change(self):
        r = J.judge_ota_rollback(True, {0: {"pid_ki": 0.6}}, {0: {"pid_ki": 0.600000024}}, False)
        self.assertEqual(r.verdict, Verdict.PASS)
        r = J.judge_ota_rollback(True, {0: {"pid_ki": 0.6}}, {0: {"pid_ki": 0.1}}, False)
        self.assertEqual(r.verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
