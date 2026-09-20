#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_heat -- the bench profile
builder, the rest gate, and the capability_preflight heat-start gate.
Every board call is faked; these confirm the builder/gate LOGIC, never that
a real board answers a certain way (this wave was never run against the
bench board -- it is blocked by a standing unacknowledged crash report).

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_heat.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_heat as C  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


class _Reading:
    def __init__(self, channel, temperature_c, valid=True):
        self.channel = channel
        self.temperature_c = temperature_c
        self.valid = valid


class _OkReason:
    def __init__(self, ok=True, error=None, reason=None, id=None, warning_count=0):
        self.ok = ok
        self.error = error
        self.reason = reason
        self.id = id
        self.warning_count = warning_count


class _ExecStatus:
    def __init__(self, state_name="idle", zones=None):
        self.state_name = state_name
        self.zones = zones or []


class _ZoneStatus:
    def __init__(self, duty=0.0, relay_commanded_on=False):
        self.duty = duty
        self.relay_commanded_on = relay_commanded_on


class _FakeProfilesClient:
    def __init__(self, save_result=None, start_result=None):
        self.save_result = save_result or _OkReason(ok=True, id=C.BENCH_PROFILE_SLOT_ID)
        self.start_result = start_result or _OkReason(ok=True)
        self.saved = []
        self.started = []
        self.stop_called = False
        self.deleted = []

    def save(self, profile_id, name, zone_mask, segments):
        self.saved.append((profile_id, name, zone_mask, segments))
        return self.save_result

    def start(self, profile_id):
        self.started.append(profile_id)
        return self.start_result

    def stop(self):
        self.stop_called = True
        return _OkReason(ok=True)

    def delete(self, profile_id):
        self.deleted.append(profile_id)
        return _OkReason(ok=True)

    def get_exec_status(self):
        return _ExecStatus("idle")


class _FakeSafetyClient:
    def get_link_stats(self):
        return type("S", (), {"crc_errors": 0, "timeouts": 0, "broadcast_dropped": 0})()


class _FakeThermoClient:
    def __init__(self, readings):
        self._readings = readings

    def read(self):
        return self._readings


class _FakeSrv:
    def __init__(self, readings=None, profiles=None):
        self._thermo = _FakeThermoClient(readings if readings is not None else [
            _Reading(0, 24.0), _Reading(1, 24.2), _Reading(2, 23.9),
        ])
        self._profiles = profiles or _FakeProfilesClient()
        self._safety = _FakeSafetyClient()


def _always_ok_preflight(ctx):
    ctx["capability_preflight_run"] = lambda preset, host, **kw: type(
        "R", (), {"ok": True, "board": type("B", (), {})()}
    )()


def _refusing_preflight(ctx, crash=True):
    board = type("B", (), {
        "crash_unacknowledged": crash, "crash_summary": "touch_log_tap_targets panic",
        "readiness_blocked": False, "reachable": True, "error": "",
    })()
    ctx["capability_preflight_run"] = lambda preset, host, **kw: type("R", (), {"ok": False, "board": board})()


class ZoneTempsTest(unittest.TestCase):
    def test_invalid_reading_excluded(self):
        srv = _FakeSrv(readings=[_Reading(0, 24.0), _Reading(1, float("nan"), valid=False)])
        ctx = {"srv": srv}
        temps = C._zone_temps(ctx)
        self.assertEqual(set(temps), {0})


class RestGateTest(unittest.TestCase):
    def test_already_rested_returns_immediately(self):
        srv = _FakeSrv(readings=[_Reading(0, 24.0), _Reading(1, 24.5), _Reading(2, 23.8)])
        ctx = {"srv": srv}
        rested, reason = C._rest_gate(ctx, timeout_s=100, poll_s=1)
        self.assertTrue(rested)
        self.assertEqual(reason, "")

    def test_never_rests_times_out(self):
        srv = _FakeSrv(readings=[_Reading(0, 24.0), _Reading(1, 40.0), _Reading(2, 24.0)])
        clock = {"t": 0.0}
        ctx = {
            "srv": srv,
            "_now": lambda: clock["t"],
            "_sleep": lambda s: clock.__setitem__("t", clock["t"] + s),
        }
        rested, reason = C._rest_gate(ctx, timeout_s=20, poll_s=5)
        self.assertFalse(rested)
        self.assertEqual(reason, "not_rested")

    def test_no_valid_reading_fails_fast(self):
        srv = _FakeSrv(readings=[])
        ctx = {"srv": srv}
        rested, reason = C._rest_gate(ctx, timeout_s=100, poll_s=1)
        self.assertFalse(rested)
        self.assertIn("no valid thermo reading", reason)


class CapabilityPreflightGateTest(unittest.TestCase):
    def test_ok_report_allows(self):
        ctx = {}
        _always_ok_preflight(ctx)
        ok, reason = C._capability_preflight_ok(ctx)
        self.assertTrue(ok)
        self.assertEqual(reason, "")

    def test_unacknowledged_crash_refuses(self):
        ctx = {}
        _refusing_preflight(ctx, crash=True)
        ok, reason = C._capability_preflight_ok(ctx)
        self.assertFalse(ok)
        self.assertIn("unacknowledged crash", reason)


class BenchProfileBuilderTest(unittest.TestCase):
    def test_starts_at_bench_slot_with_offset_target(self):
        srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
        ctx = {"srv": srv}
        _always_ok_preflight(ctx)
        ok, reason, ambient = C._start_bench_profile(ctx, zone_mask=0b001, target_offset_c=15.0)
        self.assertTrue(ok, reason)
        self.assertEqual(ambient, 20.0)
        profile_id, name, zone_mask, segments = srv._profiles.saved[0]
        self.assertEqual(profile_id, C.BENCH_PROFILE_SLOT_ID)
        self.assertEqual(zone_mask, 0b001)
        self.assertEqual(segments[0].target_c, 35.0)
        self.assertEqual(srv._profiles.started, [C.BENCH_PROFILE_SLOT_ID])

    def test_refuses_when_preflight_not_ok(self):
        srv = _FakeSrv()
        ctx = {"srv": srv}
        _refusing_preflight(ctx, crash=True)
        ok, reason, ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertFalse(ok)
        self.assertIn("unacknowledged crash", reason)
        self.assertEqual(srv._profiles.saved, [])
        self.assertEqual(srv._profiles.started, [])

    def test_save_refusal_propagates(self):
        profiles = _FakeProfilesClient(save_result=_OkReason(ok=False, error="name too long"))
        srv = _FakeSrv(profiles=profiles)
        ctx = {"srv": srv}
        _always_ok_preflight(ctx)
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertFalse(ok)
        self.assertIn("name too long", reason)
        self.assertEqual(profiles.started, [])

    def test_no_valid_thermo_reading_refuses(self):
        srv = _FakeSrv(readings=[])
        ctx = {"srv": srv}
        _always_ok_preflight(ctx)
        ok, reason, ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertFalse(ok)
        self.assertIsNone(ambient)


class CleanupTest(unittest.TestCase):
    def test_cleanup_stops_running_and_deletes_slot(self):
        profiles = _FakeProfilesClient()
        profiles.get_exec_status = lambda: _ExecStatus("running")
        srv = _FakeSrv(profiles=profiles)
        C._cleanup_bench_profile({"srv": srv})
        self.assertTrue(profiles.stop_called)
        self.assertEqual(profiles.deleted, [C.BENCH_PROFILE_SLOT_ID])

    def test_cleanup_never_raises_on_exec_status_error(self):
        profiles = _FakeProfilesClient()

        def _raise():
            raise RuntimeError("link down")

        profiles.get_exec_status = _raise
        srv = _FakeSrv(profiles=profiles)
        C._cleanup_bench_profile({"srv": srv})  # must not raise
        self.assertEqual(profiles.deleted, [C.BENCH_PROFILE_SLOT_ID])


if __name__ == "__main__":
    unittest.main()
