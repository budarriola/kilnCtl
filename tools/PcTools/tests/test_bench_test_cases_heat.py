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


class CleanupRegressionTest(unittest.TestCase):
    """Two defects found reviewing wave 1b, each fixed with its own test
    here; both were paths on which the fixture could be left heating or the
    hidden slot left behind."""

    def test_start_refusal_still_deletes_the_saved_bench_slot(self):
        profiles = _FakeProfilesClient(start_result=_OkReason(ok=False, error="busy"))
        srv = _FakeSrv(profiles=profiles)
        ctx = {"srv": srv}
        _always_ok_preflight(ctx)
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertFalse(ok)
        self.assertIn("busy", reason)
        # the save DID land in the hidden slot, so the teardown must have run
        self.assertEqual(profiles.saved[0][0], C.BENCH_PROFILE_SLOT_ID)
        self.assertEqual(profiles.deleted, [C.BENCH_PROFILE_SLOT_ID])
        self.assertTrue(profiles.stop_called)

    def test_start_raising_still_deletes_the_saved_bench_slot(self):
        class _Raising(_FakeProfilesClient):
            def start(self, profile_id):
                raise RuntimeError("link down")

        profiles = _Raising()
        srv = _FakeSrv(profiles=profiles)
        ctx = {"srv": srv}
        _always_ok_preflight(ctx)
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertFalse(ok)
        self.assertIn("RuntimeError", reason)
        self.assertEqual(profiles.deleted, [C.BENCH_PROFILE_SLOT_ID])

    def test_cleanup_stops_even_when_exec_status_raises(self):
        """A failed get_exec_status() must not skip the stop -- stopping the
        host does not stop a firing."""
        class _BlindStatus(_FakeProfilesClient):
            def get_exec_status(self):
                raise RuntimeError("no reply")

        profiles = _BlindStatus()
        C._cleanup_bench_profile({"srv": _FakeSrv(profiles=profiles)})
        self.assertTrue(profiles.stop_called)
        self.assertEqual(profiles.deleted, [C.BENCH_PROFILE_SLOT_ID])

    def test_cleanup_stops_even_when_stop_raises_then_still_deletes(self):
        class _StopRaises(_FakeProfilesClient):
            def stop(self):
                self.stop_called = True
                raise RuntimeError("no reply")

        profiles = _StopRaises()
        C._cleanup_bench_profile({"srv": _FakeSrv(profiles=profiles)})
        self.assertTrue(profiles.stop_called)
        self.assertEqual(profiles.deleted, [C.BENCH_PROFILE_SLOT_ID])


class _ZoneExecStatusHP:
    def __init__(self, duty=0.0, relay_commanded_on=False, faulted=False, fault_guard=0):
        self.duty = duty
        self.relay_commanded_on = relay_commanded_on
        self.faulted = faulted
        self.fault_guard = fault_guard


class _FakeZonesHttpClient:
    """Stand-in for kilnctrl.zones_http_client -- records what was POSTed and
    replays a canned snapshot for GET, so HP-03/HP-07 can be exercised
    without a board or real HTTP."""

    def __init__(self, snapshot=None):
        self.snapshot = snapshot or {
            "thermo_count": 3, "relay_count": 3,
            "zones": [
                {"index": 0, "zone_type": 0, "max_temp_c": 300.0},
                {"index": 1, "zone_type": 0, "max_temp_c": 300.0},
                {"index": 2, "zone_type": 0, "max_temp_c": 300.0},
            ],
        }
        self.posted_bodies = []
        self.post_result = "ok"

    def get_zones(self, host):
        return self.snapshot

    def build_post_body(self, current, preset):
        # A lightweight stand-in that just records the merged zone view
        # rather than reproducing the real urlencoded wire format -- these
        # tests only need to prove the case function feeds the right
        # (current, preset) shape through and reacts to what comes back.
        merged = {z["index"]: dict(z) for z in current.get("zones", [])}
        for z in preset.get("zones", []):
            merged[z["index"]].update(z)
        return {"current": current, "preset": preset, "merged": merged}

    def post_zones(self, host, body):
        self.posted_bodies.append(body)
        return self.post_result


class _FakeProfilesClientHP(_FakeProfilesClient):
    def __init__(self, exec_statuses=None, **kw):
        super().__init__(**kw)
        self._exec_statuses = list(exec_statuses or [_ExecStatus("done", [])])

    def get_exec_status(self):
        if len(self._exec_statuses) > 1:
            return self._exec_statuses.pop(0)
        return self._exec_statuses[0]


def _install_fake_zones_http_client(fake):
    import kilnctrl.zones_http_client as real
    saved = (real.get_zones, real.build_post_body, real.post_zones)
    real.get_zones = fake.get_zones
    real.build_post_body = fake.build_post_body
    real.post_zones = fake.post_zones
    return saved


def _restore_zones_http_client(saved):
    import kilnctrl.zones_http_client as real
    real.get_zones, real.build_post_body, real.post_zones = saved


class HP03Test(unittest.TestCase):
    def setUp(self):
        self.fake_zhc = _FakeZonesHttpClient()
        self._saved = _install_fake_zones_http_client(self.fake_zhc)

    def tearDown(self):
        _restore_zones_http_client(self._saved)

    def _ctx(self, profiles):
        srv = _FakeSrv(
            readings=[_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 24.0)],
            profiles=profiles,
        )
        clock = {"t": 0.0}
        ctx = {
            "srv": srv, "host": "10.0.0.5",
            "_now": lambda: clock["t"],
            "_sleep": lambda s: clock.__setitem__("t", clock["t"] + s),
        }
        _always_ok_preflight(ctx)
        return ctx

    def test_passes_on_cycling_within_band(self):
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP(), _ZoneExecStatusHP(), _ZoneExecStatusHP(relay_commanded_on=True)]),
            _ExecStatus("running", [_ZoneExecStatusHP(), _ZoneExecStatusHP(), _ZoneExecStatusHP(relay_commanded_on=False)]),
            _ExecStatus("done", [_ZoneExecStatusHP(), _ZoneExecStatusHP(), _ZoneExecStatusHP(relay_commanded_on=True)]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        # Rest gate/ambient calc must see uniform 24C; the run-loop polls
        # must see the target zone sitting ON the on/off target (ambient 24
        # + offset 10 = 34C) so the band check passes -- a static fake
        # thermo reading can't simulate real thermal lag, only the band
        # math, so this switches reading sets after the Nth read() call
        # (rest gate's one read + _start_bench_profile's ambient read).
        calls = {"n": 0}
        rested_readings = [_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 24.0)]
        on_target_readings = [_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 34.0)]

        def _read():
            calls["n"] += 1
            return rested_readings if calls["n"] <= 2 else on_target_readings

        ctx["srv"]._thermo.read = _read
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        # zones were restored: the last POST body must be the unmodified restore
        self.assertGreaterEqual(len(self.fake_zhc.posted_bodies), 2)
        self.assertEqual(self.fake_zhc.posted_bodies[-1]["preset"], {})

    def test_skips_when_not_rested(self):
        srv = _FakeSrv(readings=[_Reading(0, 24.0), _Reading(1, 40.0), _Reading(2, 24.0)])
        clock = {"t": 0.0}
        ctx = {
            "srv": srv, "host": "10.0.0.5",
            "_now": lambda: clock["t"],
            "_sleep": lambda s: clock.__setitem__("t", clock["t"] + s + 100.0),
        }
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_fails_when_relay_never_toggles_and_still_restores(self):
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP(), _ZoneExecStatusHP(), _ZoneExecStatusHP(relay_commanded_on=True)]),
            _ExecStatus("done", [_ZoneExecStatusHP(), _ZoneExecStatusHP(), _ZoneExecStatusHP(relay_commanded_on=True)]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertGreaterEqual(len(self.fake_zhc.posted_bodies), 2)

    def test_post_refusal_fails_without_starting_a_profile(self):
        self.fake_zhc.post_result = "refused: bad field"
        profiles = _FakeProfilesClientHP()
        ctx = self._ctx(profiles)
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertEqual(profiles.started, [])


class HP07Test(unittest.TestCase):
    def setUp(self):
        self.fake_zhc = _FakeZonesHttpClient()
        self._saved = _install_fake_zones_http_client(self.fake_zhc)

    def tearDown(self):
        _restore_zones_http_client(self._saved)

    def _ctx(self, profiles):
        srv = _FakeSrv(
            readings=[_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 24.0)],
            profiles=profiles,
        )
        clock = {"t": 0.0}
        ctx = {
            "srv": srv, "host": "10.0.0.5",
            "_now": lambda: clock["t"],
            "_sleep": lambda s: clock.__setitem__("t", clock["t"] + s),
        }
        _always_ok_preflight(ctx)
        return ctx

    def test_passes_on_a_clean_provoked_fault_and_clear(self):
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP()]),
            _ExecStatus("faulted", [_ZoneExecStatusHP(faulted=True, fault_guard=5)]),
            _ExecStatus("idle", [_ZoneExecStatusHP()]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertTrue(profiles.stop_called)
        self.assertEqual(self.fake_zhc.posted_bodies[-1]["preset"], {})

    def test_fails_when_never_reaches_faulted(self):
        statuses = [_ExecStatus("running", [_ZoneExecStatusHP()])]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        clock = {"t": 0.0}
        ctx["_now"] = lambda: clock["t"]
        ctx["_sleep"] = lambda s: clock.__setitem__("t", clock["t"] + s)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("FAULTED", result.reason)

    def test_skips_when_not_rested(self):
        srv = _FakeSrv(readings=[_Reading(0, 24.0), _Reading(1, 40.0), _Reading(2, 24.0)])
        clock = {"t": 0.0}
        ctx = {
            "srv": srv, "host": "10.0.0.5",
            "_now": lambda: clock["t"],
            "_sleep": lambda s: clock.__setitem__("t", clock["t"] + s + 100.0),
        }
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.SKIP)
