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


def _diag_text(trip_reason=0, trip_mask=None, current_fault_sources=0):
    """Minimal text `parse_trip_reason`/`parse_trip_mask`/
    `parse_current_fault_sources` (judgments.py) can parse -- shape doesn't
    have to match the real device's `SafetyDiag.describe()` exactly, only the
    field names/values those regexes look for."""
    if trip_mask is None:
        trip_mask = (1 << (trip_reason - 1)) if trip_reason else 0
    return (
        f"trip_reason: {trip_reason} | trip_mask: 0x{trip_mask:04x} | "
        f"current_fault_sources: 0x{current_fault_sources:02x}"
    )


class _FakeSrv:
    def __init__(self, readings=None, profiles=None, diag_sequence=None):
        self._thermo = _FakeThermoClient(readings if readings is not None else [
            _Reading(0, 24.0), _Reading(1, 24.2), _Reading(2, 23.9),
        ])
        self._profiles = profiles or _FakeProfilesClient()
        self._safety = _FakeSafetyClient()
        #: `srv.safety_get_diag()`/`srv.safety_clear_trip()` -- called
        #: directly on `srv`, same as cases_fl.py's `_case_fl11`, distinct
        #: from `srv._safety.get_link_stats()` above. Default: no trip, no
        #: fault sources asserted, so existing HP-07 tests that never mention
        #: this stay PASS-as-before.
        self._diag_sequence = list(diag_sequence) if diag_sequence is not None else [_diag_text()]
        self.safety_clear_trip_called = 0

    def safety_get_diag(self):
        if len(self._diag_sequence) > 1:
            return self._diag_sequence.pop(0)
        return self._diag_sequence[0]

    def safety_clear_trip(self):
        self.safety_clear_trip_called += 1
        # A real clear makes the next diag read report reason 0, unless the
        # test queued its own explicit post-clear text.
        if len(self._diag_sequence) <= 1:
            self._diag_sequence = [_diag_text()]


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


class _FakeProfileEditHttpClient:
    """Stand-in for kilnctrl.profile_edit_http_client -- records what
    post_profile() was called with, so HP-03's on/off-rule attach path can
    be exercised without a board or real HTTP."""

    def __init__(self, raise_exc=None):
        self.calls = []
        self.raise_exc = raise_exc

    def post_profile(self, host, profile_id, name, zone_mask, segments, on_off_rules=None, timeout=8.0):
        self.calls.append({
            "host": host, "profile_id": profile_id, "name": name,
            "zone_mask": zone_mask, "segments": segments, "on_off_rules": on_off_rules,
        })
        if self.raise_exc is not None:
            raise self.raise_exc
        return {"ok": True, "id": profile_id, "warnings": []}


def _install_fake_profile_edit_http_client(fake):
    import kilnctrl.profile_edit_http_client as real
    saved = real.post_profile
    real.post_profile = fake.post_profile
    return saved


def _restore_profile_edit_http_client(saved):
    import kilnctrl.profile_edit_http_client as real
    real.post_profile = saved


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

    def test_on_off_rules_go_through_http_not_uart_save(self):
        """Backward compat: `on_off_rules=None` (every existing caller) must
        keep using the UART SAVE path unchanged -- covered by
        test_starts_at_bench_slot_with_offset_target above, which passes no
        `on_off_rules` and still asserts against `srv._profiles.saved`. This
        test proves the OTHER half: passing `on_off_rules` routes the save
        over POST /api/profile instead, with the UART SAVE never touched."""
        import kilnctrl.profile_edit_http_client as pehc

        fake = _FakeProfileEditHttpClient()
        saved = _install_fake_profile_edit_http_client(fake)
        try:
            srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
            ctx = {"srv": srv, "host": "10.0.0.5"}
            _always_ok_preflight(ctx)
            rule = pehc.OnOffRule(
                zone_index=2, segment_index=0, temp_cmp=pehc.ON_OFF_TEMP_CMP_BELOW,
                temp_source=pehc.ON_OFF_TEMP_SOURCE_MEASURED_THIS_ZONE, temp_threshold_c=30.0,
            )
            ok, reason, ambient = C._start_bench_profile(
                ctx, zone_mask=1 << 2, target_offset_c=10.0, on_off_rules=[rule],
            )
        finally:
            _restore_profile_edit_http_client(saved)
        self.assertTrue(ok, reason)
        self.assertEqual(ambient, 20.0)
        self.assertEqual(srv._profiles.saved, [], "UART SAVE must not be used when on_off_rules is given")
        self.assertEqual(srv._profiles.started, [C.BENCH_PROFILE_SLOT_ID])
        self.assertEqual(len(fake.calls), 1)
        call = fake.calls[0]
        self.assertEqual(call["host"], "10.0.0.5")
        self.assertEqual(call["profile_id"], C.BENCH_PROFILE_SLOT_ID)
        self.assertEqual(call["zone_mask"], 1 << 2)
        self.assertEqual(call["on_off_rules"], [rule])

    def test_on_off_rules_http_failure_refuses_and_never_starts(self):
        """NEGATIVE: a POST /api/profile failure must refuse the start, same
        as an ordinary UART save refusal (test_save_refusal_propagates)."""
        fake = _FakeProfileEditHttpClient(raise_exc=RuntimeError("400: bad on/off rule"))
        saved = _install_fake_profile_edit_http_client(fake)
        try:
            srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
            ctx = {"srv": srv, "host": "10.0.0.5"}
            _always_ok_preflight(ctx)
            import kilnctrl.profile_edit_http_client as pehc

            rule = pehc.OnOffRule(zone_index=2, segment_index=0)
            ok, reason, _ambient = C._start_bench_profile(
                ctx, zone_mask=1 << 2, on_off_rules=[rule],
            )
        finally:
            _restore_profile_edit_http_client(saved)
        self.assertFalse(ok)
        self.assertIn("bad on/off rule", reason)
        self.assertEqual(srv._profiles.started, [])

    def test_on_off_rules_without_host_refuses_before_any_http(self):
        srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
        ctx = {"srv": srv}  # no "host" key
        _always_ok_preflight(ctx)
        import kilnctrl.profile_edit_http_client as pehc

        rule = pehc.OnOffRule(zone_index=2, segment_index=0)
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=1 << 2, on_off_rules=[rule])
        self.assertFalse(ok)
        self.assertIn("no host", reason)

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
    def __init__(self, duty=0.0, relay_commanded_on=False, faulted=False, fault_guard=0, zone=0, actual_c=None):
        self.zone = zone
        self.duty = duty
        self.relay_commanded_on = relay_commanded_on
        self.faulted = faulted
        self.fault_guard = fault_guard
        self.actual_c = actual_c


class _FakeZonesHttpClient:
    """Stand-in for kilnctrl.zones_http_client -- records what was POSTed and
    replays a canned snapshot for GET, so HP-03/HP-07 can be exercised
    without a board or real HTTP."""

    def __init__(self, snapshot=None, profiles=None):
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
        # Optional: a _FakeProfilesClient(HP) to check against, so post_zones
        # can simulate the real firmware's RUNNING interlock -- 409 refused
        # (zones_http_post.c:44-48, ota_interlock.c:56-57) whenever the
        # profile has been started and not yet stopped.
        self.profiles = profiles
        self.post_calls = []

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
        was_running = bool(
            self.profiles is not None and self.profiles.started and not self.profiles.stop_called
        )
        self.post_calls.append({"body": body, "was_running": was_running})
        if was_running:
            return "refused: HTTP 409: ota interlock active (profile RUNNING)"
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
        self.fake_pehc = _FakeProfileEditHttpClient()
        self._saved_pehc = _install_fake_profile_edit_http_client(self.fake_pehc)

    def tearDown(self):
        _restore_zones_http_client(self._saved)
        _restore_profile_edit_http_client(self._saved_pehc)

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
            _ExecStatus("running", [_ZoneExecStatusHP(zone=0), _ZoneExecStatusHP(zone=1), _ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
            _ExecStatus("running", [_ZoneExecStatusHP(zone=0), _ZoneExecStatusHP(zone=1), _ZoneExecStatusHP(zone=2, relay_commanded_on=False)]),
            _ExecStatus("done", [_ZoneExecStatusHP(zone=0), _ZoneExecStatusHP(zone=1), _ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
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
            _ExecStatus("running", [_ZoneExecStatusHP(zone=0), _ZoneExecStatusHP(zone=1), _ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
            _ExecStatus("done", [_ZoneExecStatusHP(zone=0), _ZoneExecStatusHP(zone=1), _ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
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


    def test_compacted_status_list_finds_zone_by_field(self):
        """Only zone 2 participates, so `st.zones` is a length-1 list -- the
        old positional `st.zones[2]` never saw a relay sample at all."""
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
            _ExecStatus("running", [_ZoneExecStatusHP(zone=2, relay_commanded_on=False)]),
            _ExecStatus("done", [_ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        calls = {"n": 0}
        rested = [_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 24.0)]
        on_target = [_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 34.0)]

        def _read():
            calls["n"] += 1
            return rested if calls["n"] <= 2 else on_target

        ctx["srv"]._thermo.read = _read
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_restore_failure_is_surfaced_not_swallowed(self):
        """HP-03 changes persistent zone config too; a failed restore used to
        be `except Exception: pass` with the "ok" text never checked."""
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
            _ExecStatus("done", [_ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        import kilnctrl.zones_http_client as real
        fake_post = self.fake_zhc.post_zones
        calls = {"n": 0}

        def post(host, body):
            calls["n"] += 1
            if calls["n"] == 1:
                return fake_post(host, body)
            return "refused: busy"

        real.post_zones = post
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("zone 2 zones config restore failed", result.reason)
        self.assertEqual(calls["n"], 3, "expected on/off POST + two restore attempts")

    def test_leftover_low_ceiling_refuses_before_attaching_rule(self):
        """Opus review: the ceiling preflight must be checked against the
        rule's own `temp_threshold_c` (34C: ambient 24 + on/off offset 10),
        not left to `_start_bench_profile`'s internal ambient+offset target
        alone -- a zone 2 ceiling left at 30C (e.g. a failed HP-07 restore)
        must refuse before ever POSTing the on/off rule."""
        self.fake_zhc.snapshot = {
            "thermo_count": 3, "relay_count": 3,
            "zones": [
                {"index": 0, "zone_type": 0, "max_temp_c": 300.0},
                {"index": 1, "zone_type": 0, "max_temp_c": 300.0},
                {"index": 2, "zone_type": 0, "max_temp_c": 30.0},
            ],
        }
        profiles = _FakeProfilesClientHP()
        ctx = self._ctx(profiles)
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("max_temp_c", result.reason)
        self.assertEqual(len(self.fake_pehc.calls), 0)
        self.assertEqual(profiles.started, [])
        # the on/off zone config was still restored
        self.assertEqual(self.fake_zhc.posted_bodies[-1]["preset"], {})

    def test_on_off_rule_attached_with_correct_fields(self):
        """The defect under fix: HP-03 must attach an enabled on/off rule for
        (zone=target_zone, segment 0) to the profile it saves, or the relay
        can never switch (profile_resolve_on_off_rule() with no match)."""
        import kilnctrl.profile_edit_http_client as pehc

        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
            _ExecStatus("running", [_ZoneExecStatusHP(zone=2, relay_commanded_on=False)]),
            _ExecStatus("done", [_ZoneExecStatusHP(zone=2, relay_commanded_on=True)]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        calls = {"n": 0}
        rested = [_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 24.0)]
        on_target = [_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 34.0)]

        def _read():
            calls["n"] += 1
            return rested if calls["n"] <= 2 else on_target

        ctx["srv"]._thermo.read = _read
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(len(self.fake_pehc.calls), 1)
        rules = self.fake_pehc.calls[0]["on_off_rules"]
        self.assertEqual(len(rules), 1)
        rule = rules[0]
        self.assertEqual(rule.zone_index, 2)
        self.assertEqual(rule.segment_index, 0)
        self.assertTrue(rule.enable)
        self.assertEqual(rule.temp_cmp, pehc.ON_OFF_TEMP_CMP_BELOW)
        self.assertEqual(rule.temp_source, pehc.ON_OFF_TEMP_SOURCE_MEASURED_THIS_ZONE)
        # threshold is ambient(24) + the on/off target offset (10) = 34
        self.assertEqual(rule.temp_threshold_c, 34.0)
        # the UART SAVE path must not be used when a rule is attached
        self.assertEqual(profiles.saved, [])

    def test_pehc_raising_fails_restores_zones_and_deletes_slot(self):
        """NEGATIVE (Opus review): post_profile() raising (e.g. a 400 from a
        bad on/off rule) must FAIL the case, still restore the on/off zone
        config it already POSTed, and still delete the hidden bench slot --
        same teardown obligations as every other HP-03 failure path."""
        self.fake_pehc.raise_exc = RuntimeError("400: bad on/off rule")
        profiles = _FakeProfilesClientHP()
        ctx = self._ctx(profiles)
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("bad on/off rule", result.reason)
        # the on/off zone config POST happened before the profile POST, so a
        # restore POST must follow it regardless of the profile POST's fate
        self.assertGreaterEqual(len(self.fake_zhc.posted_bodies), 2)
        self.assertEqual(self.fake_zhc.posted_bodies[-1]["preset"], {})
        self.assertEqual(profiles.deleted, [C.BENCH_PROFILE_SLOT_ID])
        self.assertEqual(profiles.started, [])

    def test_on_off_post_raising_still_restores(self):
        """A POST that raises may still have been committed by firmware."""
        import kilnctrl.zones_http_client as real
        fake_post = self.fake_zhc.post_zones
        calls = {"n": 0}

        def post(host, body):
            calls["n"] += 1
            if calls["n"] == 1:
                fake_post(host, body)
                raise OSError("read timed out")
            return fake_post(host, body)

        real.post_zones = post
        profiles = _FakeProfilesClientHP()
        ctx = self._ctx(profiles)
        result = C._case_hp03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertEqual(profiles.started, [])
        self.assertEqual(self.fake_zhc.posted_bodies[-1]["preset"], {})


class HP07Test(unittest.TestCase):
    def setUp(self):
        self.fake_zhc = _FakeZonesHttpClient()
        self._saved = _install_fake_zones_http_client(self.fake_zhc)
        self.fake_pehc = _FakeProfileEditHttpClient()
        self._saved_pehc = _install_fake_profile_edit_http_client(self.fake_pehc)

    def tearDown(self):
        _restore_zones_http_client(self._saved)
        _restore_profile_edit_http_client(self._saved_pehc)

    def _ctx(self, profiles, diag_sequence=None):
        # Default: the expected shape once FAULTED is acked -- the ESP
        # released its fault source and the Pico shows exactly S6a
        # (trip_reason 6, mask 0x0020), which `_case_hp07` clears.
        srv = _FakeSrv(
            readings=[_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 24.0)],
            profiles=profiles,
            diag_sequence=diag_sequence if diag_sequence is not None else [_diag_text(trip_reason=6)],
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

    def test_never_posts_zones_while_running(self):
        """Regression for the 2026-09-24 finding: POST /api/zones while
        RUNNING is refused 409 by the OTA/config-write interlock
        (zones_http_post.c:44-48, ota_interlock.c:56-57), so the limit must
        be lowered BEFORE the profile starts and restored only after it has
        been stopped -- never while `_FakeProfilesClientHP` reports the
        profile as started-and-not-yet-stopped."""
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP()]),
            _ExecStatus("faulted", [_ZoneExecStatusHP(faulted=True, fault_guard=5)]),
            _ExecStatus("idle", [_ZoneExecStatusHP()]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        self.fake_zhc.profiles = profiles
        ctx = self._ctx(profiles)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertTrue(self.fake_zhc.post_calls, "expected at least one zones POST")
        self.assertFalse(
            any(c["was_running"] for c in self.fake_zhc.post_calls),
            "a zones POST happened while the profile was RUNNING -- would be refused 409 on real hardware",
        )
        # Exactly two POSTs: lower the limit (while IDLE, before start), then
        # restore it (after stop, in `finally`).
        self.assertEqual(len(self.fake_zhc.post_calls), 2)
        self.assertEqual(self.fake_zhc.post_calls[0]["body"]["preset"]["zones"][0]["max_temp_c"], 27.0)
        self.assertEqual(self.fake_zhc.post_calls[1]["body"]["preset"], {})

    def test_target_pinned_to_limit_despite_ambient_drift(self):
        """The limit comes from one ambient reading; `_start_bench_profile`
        takes another before saving. Upward drift in between must not put
        the target above the limit (firmware refuses target > max_temp_c at
        start), so the target must be exactly the POSTed limit."""
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP()]),
            _ExecStatus("faulted", [_ZoneExecStatusHP(faulted=True, fault_guard=5)]),
            _ExecStatus("idle", [_ZoneExecStatusHP()]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        base = ctx["srv"]._thermo.read()
        fake_zhc = self.fake_zhc

        def drifting_read():
            if not fake_zhc.posted_bodies:
                return base
            return [_Reading(r.channel, r.temperature_c + 0.2) for r in base]

        ctx["srv"]._thermo.read = drifting_read
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        limit = fake_zhc.posted_bodies[0]["preset"]["zones"][0]["max_temp_c"]
        self.assertEqual(limit, 27.0)
        # HP-07 now saves via POST /api/profile (on/off-rule attach path),
        # not the raw UART save -- the pinned target lives in the fake
        # profile_edit_http_client call, not `profiles.saved`.
        self.assertEqual(self.fake_pehc.calls[0]["segments"][0].target_c, limit)

    def test_limit_restored_even_when_lowering_post_raises(self):
        """A lowering POST that raises (e.g. a read timeout after firmware
        already committed) must still be followed by the restore POST."""
        profiles = _FakeProfilesClientHP(exec_statuses=[_ExecStatus("idle", [_ZoneExecStatusHP()])])
        ctx = self._ctx(profiles)
        import kilnctrl.zones_http_client as real
        fake_post = self.fake_zhc.post_zones
        n = {"calls": 0}

        def post(host, body):
            n["calls"] += 1
            if n["calls"] == 1:
                raise TimeoutError("timed out after commit")
            return fake_post(host, body)

        real.post_zones = post
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("lowered max_temp_c", result.reason)
        self.assertEqual(n["calls"], 2, "restore POST did not run after the lowering POST raised")
        self.assertEqual(self.fake_zhc.posted_bodies[-1]["preset"], {})
        self.assertEqual(profiles.started, [])

    def test_multizone_lookup_uses_zone_field_not_position(self):
        """Regression for the positional-index bug: `ProfileExecStatus.zones`
        is COMPACTED to only the participating zones, so a run against
        target_zone=2 (with only zone 2 actually running) must find its
        status by `.zone == 2`, not by `st.zones[2]` -- the old code would
        `IndexError`/silently read `None` against a length-1 list here."""
        ctx_zone = 2
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP(zone=ctx_zone)]),
            _ExecStatus("faulted", [_ZoneExecStatusHP(zone=ctx_zone, faulted=True, fault_guard=5)]),
            _ExecStatus("idle", [_ZoneExecStatusHP(zone=ctx_zone)]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        ctx["_hp07_zone_index"] = ctx_zone
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_exception_in_run_profile_becomes_fail_and_still_restores(self):
        """Regression for the 2026-09-24 bug: `_run_hp07_profile` raising
        (e.g. the board rebooting mid-run) must become a FAIL CaseResult,
        not propagate past the restore -- and the restore POST must still
        happen."""
        class _RaisingProfilesClient(_FakeProfilesClientHP):
            def get_exec_status(self):
                raise RuntimeError("board rebooted mid-run")

        profiles = _RaisingProfilesClient(exec_statuses=[_ExecStatus("running", [_ZoneExecStatusHP()])])
        ctx = self._ctx(profiles)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("RuntimeError", result.reason)
        # the restore POST (empty preset) must still have gone out
        self.assertTrue(self.fake_zhc.posted_bodies)
        self.assertEqual(self.fake_zhc.posted_bodies[-1]["preset"], {})

    def test_restore_failure_after_exception_is_surfaced_with_values(self):
        """A restore failure must never be silently dropped, whether or not
        the run itself raised -- the FAIL reason must name the zone, the
        value left behind, and the original value it should have restored."""
        class _RaisingProfilesClient(_FakeProfilesClientHP):
            def get_exec_status(self):
                raise RuntimeError("board rebooted mid-run")

        profiles = _RaisingProfilesClient(exec_statuses=[_ExecStatus("running", [_ZoneExecStatusHP()])])
        ctx = self._ctx(profiles)
        import kilnctrl.zones_http_client as real
        fake_post = self.fake_zhc.post_zones
        calls = {"n": 0}

        def post(host, body):
            calls["n"] += 1
            if calls["n"] == 1:
                return fake_post(host, body)  # the lowering POST succeeds
            raise OSError("board unreachable")  # every restore attempt fails

        real.post_zones = post
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("zone 0", result.reason)
        self.assertIn("27.0", result.reason)   # value left behind (ambient 24 + margin 3)
        self.assertIn("300.0", result.reason)  # original value from the fake snapshot

    def test_restore_retries_after_first_failure(self):
        """The restore must retry once (after a short delay) before giving
        up, in case the board rebooted mid-run and just isn't answering yet
        on the very first POST after the fault."""
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP()]),
            _ExecStatus("faulted", [_ZoneExecStatusHP(faulted=True, fault_guard=5)]),
            _ExecStatus("idle", [_ZoneExecStatusHP()]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        import kilnctrl.zones_http_client as real
        fake_post = self.fake_zhc.post_zones
        calls = {"n": 0}

        def post(host, body):
            calls["n"] += 1
            # Fail only the FIRST restore attempt (the second post_zones call
            # overall is the restore, since the first is the lowering POST).
            if calls["n"] == 2:
                raise OSError("board unreachable")
            return fake_post(host, body)

        real.post_zones = post
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(calls["n"], 3, "expected lower + failed-restore + retried-restore")
        self.assertEqual(self.fake_zhc.posted_bodies[-1]["preset"], {})

    def test_actual_c_samples_captured_in_observed(self):
        """A future knife-edge failure (never reaching FAULTED) must be
        diagnosable from `observed` without a live rerun."""
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP(actual_c=24.5)]),
            _ExecStatus("running", [_ZoneExecStatusHP(actual_c=25.8)]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIsNotNone(result.observed)
        # the fake profiles client repeats its last exec_status forever
        # (never reaches "faulted"), so the poll loop runs until the fake
        # clock's deadline rather than stopping after 2 -- assert the
        # observed prefix rather than an exact-length list.
        samples = result.observed.get("actual_c_samples")
        self.assertEqual(samples[:2], [24.5, 25.8])
        self.assertTrue(all(s == 25.8 for s in samples[2:]))


    def test_keyboard_interrupt_still_restores_and_attaches_failure(self):
        """A BaseException escapes every `except Exception`, including the
        runner's; a failed restore must then ride on the exception itself."""
        class _InterruptedProfilesClient(_FakeProfilesClientHP):
            def get_exec_status(self):
                raise KeyboardInterrupt()

        profiles = _InterruptedProfilesClient(exec_statuses=[_ExecStatus("running", [_ZoneExecStatusHP()])])
        ctx = self._ctx(profiles)
        import kilnctrl.zones_http_client as real
        fake_post = self.fake_zhc.post_zones
        calls = {"n": 0}

        def post(host, body):
            calls["n"] += 1
            if calls["n"] == 1:
                return fake_post(host, body)
            raise OSError("board unreachable")

        real.post_zones = post
        with self.assertRaises(KeyboardInterrupt) as cm:
            C._case_hp07(ctx)
        self.assertEqual(calls["n"], 3, "restore must still be attempted twice")
        notes = getattr(cm.exception, "__notes__", [])
        self.assertTrue(any("zone 0 max_temp_c restore failed" in n and "300.0" in n for n in notes), notes)

    def test_keyboard_interrupt_with_good_restore_adds_no_note(self):
        class _InterruptedProfilesClient(_FakeProfilesClientHP):
            def get_exec_status(self):
                raise KeyboardInterrupt()

        profiles = _InterruptedProfilesClient(exec_statuses=[_ExecStatus("running", [_ZoneExecStatusHP()])])
        ctx = self._ctx(profiles)
        with self.assertRaises(KeyboardInterrupt) as cm:
            C._case_hp07(ctx)
        self.assertEqual(self.fake_zhc.posted_bodies[-1]["preset"], {})
        self.assertFalse(getattr(cm.exception, "__notes__", []))

    def test_refuses_to_bake_a_leftover_low_ceiling_into_its_restore(self):
        """The restore body is the pre-lowering snapshot; a ceiling already
        left low (36.4C, the 2026-09-24 leftover) must not be lowered and
        then "restored" to itself, and must never be raised by this case."""
        self.fake_zhc.snapshot["zones"][0]["max_temp_c"] = 36.4
        profiles = _FakeProfilesClientHP()
        ctx = self._ctx(profiles)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("refusing to lower/restore", result.reason)
        self.assertIn("36.4", result.reason)
        self.assertEqual(self.fake_zhc.posted_bodies, [])
        self.assertEqual(profiles.started, [])

    def test_pico_headroom_refusal_is_inconclusive_before_any_post(self):
        """Advisory fix, 2026-09-25 review: an unmet bench precondition (no
        other zone clears the required headroom), not a genuine case
        failure -- and it must refuse before touching zone config at all."""
        self.fake_zhc.snapshot["zones"][1]["max_temp_c"] = 10.0
        self.fake_zhc.snapshot["zones"][2]["max_temp_c"] = 10.0
        profiles = _FakeProfilesClientHP()
        ctx = self._ctx(profiles)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(self.fake_zhc.posted_bodies, [])
        self.assertEqual(profiles.started, [])

    def _faulted_statuses(self):
        return [
            _ExecStatus("running", [_ZoneExecStatusHP()]),
            _ExecStatus("faulted", [_ZoneExecStatusHP(faulted=True, fault_guard=5)]),
            _ExecStatus("idle", [_ZoneExecStatusHP()]),
        ]

    def test_posted_on_off_rule_fields(self):
        from kilnctrl import profile_edit_http_client as pehc

        profiles = _FakeProfilesClientHP(exec_statuses=self._faulted_statuses())
        ctx = self._ctx(profiles)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        rule = self.fake_pehc.calls[0]["on_off_rules"][0]
        self.assertEqual(rule.zone_index, 0)
        self.assertEqual(rule.temp_cmp, pehc.ON_OFF_TEMP_CMP_BELOW)
        self.assertEqual(rule.temp_source, pehc.ON_OFF_TEMP_SOURCE_MEASURED_THIS_ZONE)
        self.assertEqual(rule.temp_threshold_c, 27.0 + 50.0)

    def test_lowering_preset_types_zone_as_on_off(self):
        """validate_on_off_rules() (profiles_http.c) refuses an on/off rule
        on a zone not typed ZONE_TYPE_ON_OFF -- the SAME preset POST that
        lowers max_temp_c must also type target_zone this way."""
        profiles = _FakeProfilesClientHP(exec_statuses=self._faulted_statuses())
        ctx = self._ctx(profiles)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        preset_zone = self.fake_zhc.posted_bodies[0]["preset"]["zones"][0]
        self.assertEqual(preset_zone["zone_type"], 1)
        self.assertEqual(preset_zone["failsafe_state"], False)
        self.assertEqual(preset_zone["min_on_s"], 0)
        self.assertEqual(preset_zone["min_off_s"], 0)

    def test_s6a_exact_match_clears_and_passes(self):
        profiles = _FakeProfilesClientHP(exec_statuses=self._faulted_statuses())
        ctx = self._ctx(profiles, diag_sequence=[_diag_text(trip_reason=6)])
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(ctx["srv"].safety_clear_trip_called, 1)
        self.assertEqual(result.observed.get("trip_reason_after_clear"), 0)

    def test_s6a_wrong_reason_does_not_clear_and_fails(self):
        """A different guard latched (e.g. S6b, LINK_DEAD=7) must surface as
        a case failure, never be auto-cleared -- following cases_fl.py's
        `_case_fl11` pattern."""
        profiles = _FakeProfilesClientHP(exec_statuses=self._faulted_statuses())
        ctx = self._ctx(profiles, diag_sequence=[_diag_text(trip_reason=7)])
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertEqual(ctx["srv"].safety_clear_trip_called, 0)

    def test_s6a_wrong_mask_does_not_clear_and_fails(self):
        """reason matches (6) but the mask shows a second guard also
        latched -- plan section 6 rule 5: must not clear on a reason-only
        match."""
        profiles = _FakeProfilesClientHP(exec_statuses=self._faulted_statuses())
        ctx = self._ctx(profiles, diag_sequence=[_diag_text(trip_reason=6, trip_mask=0x0060)])
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertEqual(ctx["srv"].safety_clear_trip_called, 0)

    def test_esp_fault_sources_still_asserted_fails(self):
        profiles = _FakeProfilesClientHP(exec_statuses=self._faulted_statuses())
        ctx = self._ctx(profiles, diag_sequence=[_diag_text(trip_reason=6, current_fault_sources=0x02)])
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("current_fault_sources", result.reason)
        self.assertEqual(ctx["srv"].safety_clear_trip_called, 0)

    def test_done_without_tripping_is_distinct_fail(self):
        """Advisory fix, 2026-09-25 review: the profile reaching DONE on its
        own (never crossing the guard) must be distinguishable from a plain
        poll timeout."""
        statuses = [
            _ExecStatus("running", [_ZoneExecStatusHP()]),
            _ExecStatus("done", [_ZoneExecStatusHP()]),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        ctx = self._ctx(profiles)
        result = C._case_hp07(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("DONE without ever tripping", result.reason)


class Hp07PicoCeilingHeadroomOkTest(unittest.TestCase):
    """`_hp07_pico_ceiling_headroom_ok` in isolation -- the pure preflight
    that refuses HP-07 outright if no other zone's max_temp_c clears the
    Pico ceiling margin."""

    @staticmethod
    def _snap(zones):
        return {"zones": zones}

    def test_refuses_when_no_other_zone_clears_margin(self):
        snap = self._snap([
            {"index": 0, "max_temp_c": 27.0},
            {"index": 1, "max_temp_c": 30.0},  # < 27 + 5 margin
            {"index": 2, "max_temp_c": 20.0},
        ])
        ok, reason = C._hp07_pico_ceiling_headroom_ok(snap, target_zone=0, limit_c=27.0)
        self.assertFalse(ok)
        self.assertIn("30.0", reason)

    def test_passes_when_one_other_zone_clears_margin(self):
        snap = self._snap([
            {"index": 0, "max_temp_c": 27.0},
            {"index": 1, "max_temp_c": 40.0},
            {"index": 2, "max_temp_c": 10.0},
        ])
        ok, reason = C._hp07_pico_ceiling_headroom_ok(snap, target_zone=0, limit_c=27.0)
        self.assertTrue(ok, reason)

    def test_exact_boundary_passes(self):
        # `best_other < limit_c + margin_c` refuses only STRICTLY below --
        # exactly at the margin must be accepted.
        snap = self._snap([{"index": 0, "max_temp_c": 27.0}, {"index": 1, "max_temp_c": 32.0}])
        ok, reason = C._hp07_pico_ceiling_headroom_ok(snap, target_zone=0, limit_c=27.0, margin_c=5.0)
        self.assertTrue(ok, reason)

    def test_just_under_boundary_refuses(self):
        snap = self._snap([{"index": 0, "max_temp_c": 27.0}, {"index": 1, "max_temp_c": 31.999}])
        ok, reason = C._hp07_pico_ceiling_headroom_ok(snap, target_zone=0, limit_c=27.0, margin_c=5.0)
        self.assertFalse(ok)

    def test_ignores_target_zones_own_value(self):
        # target_zone's own (large) ceiling must never count as "other".
        snap = self._snap([{"index": 0, "max_temp_c": 300.0}, {"index": 1, "max_temp_c": 10.0}])
        ok, reason = C._hp07_pico_ceiling_headroom_ok(snap, target_zone=0, limit_c=27.0)
        self.assertFalse(ok)

    def test_ignores_none_zero_and_negative_other_values(self):
        snap = self._snap([
            {"index": 0, "max_temp_c": 27.0},
            {"index": 1, "max_temp_c": None},
            {"index": 2, "max_temp_c": 0.0},
            {"index": 3, "max_temp_c": -5.0},
        ])
        ok, reason = C._hp07_pico_ceiling_headroom_ok(snap, target_zone=0, limit_c=27.0)
        self.assertFalse(ok)


class ZoneCeilingPreflightTest(unittest.TestCase):
    """`_check_zone_ceilings` / its wiring into `_start_bench_profile`:
    refuses a normal (implicit-target) heat case before ever touching the
    board if a participating zone's `max_temp_c` is already below the
    planned target -- e.g. a leftover from an HP-07 restore failure."""

    def setUp(self):
        self.fake_zhc = _FakeZonesHttpClient()
        self._saved = _install_fake_zones_http_client(self.fake_zhc)

    def tearDown(self):
        _restore_zones_http_client(self._saved)

    def test_low_ceiling_refuses_before_touching_the_board(self):
        self.fake_zhc.snapshot["zones"][0]["max_temp_c"] = 30.0  # stuck low
        srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
        ctx = {"srv": srv, "host": "10.0.0.5"}
        _always_ok_preflight(ctx)
        # ambient 20 + default offset 15 = target 35, above the stuck 30C ceiling.
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertFalse(ok)
        self.assertIn("zone 0", reason)
        self.assertIn("max_temp_c", reason)
        self.assertIn("30.0", reason)
        self.assertIn("35.0", reason)
        self.assertEqual(srv._profiles.saved, [])
        self.assertEqual(srv._profiles.started, [])

    def test_healthy_ceiling_allows_start(self):
        srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
        ctx = {"srv": srv, "host": "10.0.0.5"}
        _always_ok_preflight(ctx)
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertTrue(ok, reason)
        self.assertEqual(srv._profiles.started, [C.BENCH_PROFILE_SLOT_ID])

    def test_explicit_target_bypasses_the_ceiling_check(self):
        """HP-07 pins `target_c` to a limit it just lowered `max_temp_c` to
        (target == limit exactly) -- this must NOT be flagged, since it is
        the deliberate edge case this whole harness case exists to exercise."""
        self.fake_zhc.snapshot["zones"][0]["max_temp_c"] = 27.0
        srv = _FakeSrv(readings=[_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 24.0)])
        ctx = {"srv": srv, "host": "10.0.0.5"}
        _always_ok_preflight(ctx)
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=0b001, target_c=27.0)
        self.assertTrue(ok, reason)

    def test_missing_host_does_not_block(self):
        """No host in ctx means the ceiling check can't read the board's
        zones config -- it must pass through (best-effort) rather than
        refuse over its own missing capability."""
        srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
        ctx = {"srv": srv}
        _always_ok_preflight(ctx)
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertTrue(ok, reason)


    def test_zero_ceiling_sentinel_is_not_flagged_as_a_leftover(self):
        """max_temp_c == 0.0 is firmware's "no ceiling configured" state,
        refused by profile_zones_have_ceiling() with its own message."""
        self.fake_zhc.snapshot["zones"][0]["max_temp_c"] = 0.0
        srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
        ctx = {"srv": srv, "host": "10.0.0.5"}
        _always_ok_preflight(ctx)
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertTrue(ok, reason)

    def test_non_participating_low_zone_is_ignored(self):
        self.fake_zhc.snapshot["zones"][1]["max_temp_c"] = 30.0
        srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
        ctx = {"srv": srv, "host": "10.0.0.5"}
        _always_ok_preflight(ctx)
        ok, reason, _ambient = C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertTrue(ok, reason)

    def test_never_writes_zones_config(self):
        self.fake_zhc.snapshot["zones"][0]["max_temp_c"] = 30.0
        srv = _FakeSrv(readings=[_Reading(0, 20.0), _Reading(1, 20.0), _Reading(2, 20.0)])
        ctx = {"srv": srv, "host": "10.0.0.5"}
        _always_ok_preflight(ctx)
        C._start_bench_profile(ctx, zone_mask=0b001)
        self.assertEqual(self.fake_zhc.posted_bodies, [])
        self.assertEqual(self.fake_zhc.snapshot["zones"][0]["max_temp_c"], 30.0)


class HP08Test(unittest.TestCase):
    """GET /api/firing_history requires profile_id (400 without it) -- the
    HP-08 fix adds `?profile_id={BENCH_PROFILE_SLOT_ID}` to the harness's
    call and accepts firmware's real `records` response key."""

    def test_passes_with_profile_id_and_records_key(self):
        calls = []

        def fake_get(host, path):
            calls.append(path)
            return 200, {"records": [{
                "profile_name": C.BENCH_PROFILE_NAME, "run_started_unix_s": 1,
                "duration_s": 10,
            }]}

        orig = C._http_get_json
        C._http_get_json = fake_get
        try:
            result = C._case_hp08({"host": "10.0.0.5"})
        finally:
            C._http_get_json = orig
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(len(calls), 1)
        self.assertIn(f"profile_id={C.BENCH_PROFILE_SLOT_ID}", calls[0])

    def test_no_host_is_inconclusive(self):
        result = C._case_hp08({})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_non_200_fails(self):
        orig = C._http_get_json
        C._http_get_json = lambda host, path: (400, {"error": "profile_id missing"})
        try:
            result = C._case_hp08({"host": "10.0.0.5"})
        finally:
            C._http_get_json = orig
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_missing_profile_id_param_NEGATIVE(self):
        """Reproduces the original HP-08 bug directly, through the actual
        case function: a call with no profile_id query param at all is what
        the un-fixed harness sent, and firmware 400s it. This must call
        C._case_hp08 itself (not just its own local fake in isolation) so it
        can actually fail if a future edit ever drops the `?profile_id=...`
        query param the fix added -- the previous version of this test only
        asserted on a bare local function call and could never fail."""
        def fake_get(host, path):
            if "profile_id" not in path:
                return 400, {"error": "profile_id missing"}
            return 200, {"records": [{
                "profile_name": C.BENCH_PROFILE_NAME, "run_started_unix_s": 1, "duration_s": 10,
            }]}

        orig = C._http_get_json
        C._http_get_json = fake_get
        try:
            # No snapshot in ctx -- forces the live-GET fallback path, which
            # is the one that must include profile_id in the query string.
            result = C._case_hp08({"host": "10.0.0.5"})
        finally:
            C._http_get_json = orig
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_uses_snapshot_before_a_live_get(self):
        """`_cleanup_bench_profile()` snapshots firing history into ctx
        before deleting the hidden slot (the slot's history is erased by the
        delete) -- `_case_hp08` must judge that snapshot rather than issuing
        a live GET that would always find the slot's history already gone."""
        ctx = {
            "host": "10.0.0.5",
            C.HP_FIRING_HISTORY_SNAPSHOTS_KEY: [
                {"records": [{
                    "profile_name": C.BENCH_PROFILE_NAME, "run_started_unix_s": 1, "duration_s": 10,
                }]},
            ],
        }

        def _should_not_be_called(host, path):
            raise AssertionError("a live GET was made even though a snapshot was present")

        orig = C._http_get_json
        C._http_get_json = _should_not_be_called
        try:
            result = C._case_hp08(ctx)
        finally:
            C._http_get_json = orig
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_cleanup_snapshots_history_before_deleting_the_slot(self):
        """The teardown itself: `_cleanup_bench_profile()` must GET firing
        history and stash it in ctx BEFORE calling delete()."""
        profiles = _FakeProfilesClient()
        srv = _FakeSrv(profiles=profiles)
        calls = []

        def fake_get(host, path):
            # Record what had been deleted at GET time, to prove ordering.
            calls.append((path, list(profiles.deleted)))
            return 200, {"records": [{"profile_name": C.BENCH_PROFILE_NAME}]}

        ctx = {"srv": srv, "host": "10.0.0.5", "_http_get_json": fake_get}
        C._cleanup_bench_profile(ctx)
        self.assertEqual(len(calls), 1)
        self.assertIn(f"profile_id={C.BENCH_PROFILE_SLOT_ID}", calls[0][0])
        self.assertEqual(calls[0][1], [], "history GET ran after the slot was already deleted")
        self.assertEqual(profiles.deleted, [C.BENCH_PROFILE_SLOT_ID])
        snapshots = ctx.get(C.HP_FIRING_HISTORY_SNAPSHOTS_KEY)
        self.assertEqual(len(snapshots), 1)
        self.assertEqual(snapshots[0]["records"][0]["profile_name"], C.BENCH_PROFILE_NAME)


class ZoneDiagSnapshotTest(unittest.TestCase):
    """`_zone_diag_snapshot` -- the coordinator's follow-up for HP-02, which
    failed on the bench with zone 2 rising only 1.88C and no per-zone data
    to explain why. Read-only: `/api/profile_exec` for per-zone
    duty/heat_blocked/heat_blocked_sources, `/api/status` for the separate
    top-level zone_blocked_mask."""

    def test_reads_both_routes_and_merges(self):
        calls = []

        def get_json(host, path):
            calls.append(path)
            if path == "/api/profile_exec":
                return 200, {"zones": [
                    {"zone": 0, "duty": 0.5, "heat_blocked": False, "heat_blocked_sources": 0},
                    {"zone": 2, "duty": 0.0, "heat_blocked": True, "heat_blocked_sources": 4},
                ]}
            if path == "/api/status":
                return 200, {"zone_blocked_mask": 4}
            return 404, {}

        ctx = {"host": "10.0.0.5", "_http_get_json": get_json}
        snap = C._zone_diag_snapshot(ctx)
        self.assertEqual(calls, ["/api/profile_exec", "/api/status"])
        self.assertEqual(snap["zones"][0]["duty"], 0.5)
        self.assertFalse(snap["zones"][0]["heat_blocked"])
        self.assertEqual(snap["zones"][2]["heat_blocked_sources"], 4)
        self.assertEqual(snap["zone_blocked_mask"], 4)

    def test_no_host_returns_empty_snapshot_without_calling(self):
        ctx = {}
        snap = C._zone_diag_snapshot(ctx)
        self.assertEqual(snap["zones"], {})
        self.assertIsNone(snap["zone_blocked_mask"])

    def test_never_raises_on_a_failed_read(self):
        """Diagnostic-only: a board that refuses/errors on either route must
        not raise into the polling loop (`_hp_run` calls this every tick)."""
        def raising_get(host, path):
            raise OSError("board unreachable")

        ctx = {"host": "10.0.0.5", "_http_get_json": raising_get}
        snap = C._zone_diag_snapshot(ctx)
        self.assertEqual(snap["zones"], {})
        self.assertIsNone(snap["zone_blocked_mask"])

    def test_status_route_missing_the_mask_key_leaves_it_none(self):
        def get_json(host, path):
            if path == "/api/profile_exec":
                return 200, {"zones": []}
            return 200, {"some_other_field": 1}

        ctx = {"host": "10.0.0.5", "_http_get_json": get_json}
        snap = C._zone_diag_snapshot(ctx)
        self.assertIsNone(snap["zone_blocked_mask"])


class BlockedWhileEnergizedZonesTest(unittest.TestCase):
    def test_names_a_zone_with_duty_and_blocked_simultaneously(self):
        samples = [
            {"zones": {0: {"duty": 0.5, "heat_blocked": False}, 2: {"duty": 0.0, "heat_blocked": True}}},
            {"zones": {0: {"duty": 0.4, "heat_blocked": False}, 2: {"duty": 0.3, "heat_blocked": True}}},
        ]
        self.assertEqual(C._blocked_while_energized_zones(samples), [2])

    def test_empty_when_no_zone_is_both_duty_and_blocked(self):
        samples = [
            {"zones": {0: {"duty": 0.5, "heat_blocked": False}, 2: {"duty": 0.0, "heat_blocked": True}}},
        ]
        self.assertEqual(C._blocked_while_energized_zones(samples), [])

    def test_tolerates_missing_or_none_fields(self):
        samples = [{"zones": {0: {"duty": None, "heat_blocked": True}}}, {}]
        self.assertEqual(C._blocked_while_energized_zones(samples), [])


class HP02DiagnosticTest(unittest.TestCase):
    """`_case_hp02`'s FAIL detail names any zone whose duty was > 0 while
    heat_blocked was set, using the per-zone snapshots `_hp_run` now
    collects every poll tick."""

    def _ctx(self, profiles, get_json):
        srv = _FakeSrv(
            readings=[_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 24.0)],
            profiles=profiles,
        )
        clock = {"t": 0.0}
        ctx = {
            "srv": srv, "host": "10.0.0.5",
            "_now": lambda: clock["t"],
            "_sleep": lambda s: clock.__setitem__("t", clock["t"] + s),
            "_http_get_json": get_json,
        }
        _always_ok_preflight(ctx)
        return ctx

    @staticmethod
    def _stepped_thermo(final_readings):
        """`_hp_run` reads thermo twice outside the poll loop (rest gate +
        `start_zones`, both ambient) and once more for `end_zones` -- this
        returns ambient for the first two calls and `final_readings`
        thereafter, so a test can set a real start/end rise instead of a
        flat, always-equal reading that would make every zone's rise 0."""
        calls = {"n": 0}
        ambient = [_Reading(0, 24.0), _Reading(1, 24.0), _Reading(2, 24.0)]

        def read():
            calls["n"] += 1
            return ambient if calls["n"] <= 2 else final_readings

        return read

    def test_fail_names_the_blocked_zone(self):
        statuses = [
            _ExecStatus("running", []),
            _ExecStatus("done", []),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)

        def get_json(host, path):
            if path == "/api/profile_exec":
                return 200, {"zones": [
                    {"zone": 0, "duty": 0.5, "heat_blocked": False},
                    {"zone": 1, "duty": 0.5, "heat_blocked": False},
                    {"zone": 2, "duty": 0.6, "heat_blocked": True},
                ]}
            return 200, {}

        ctx = self._ctx(profiles, get_json)
        # Zone 2 barely rises (1.88C-style bench symptom) so judge_all_zones_rise FAILs.
        ctx["srv"]._thermo.read = self._stepped_thermo(
            [_Reading(0, 30.0), _Reading(1, 30.0), _Reading(2, 25.9)]
        )
        result = C._case_hp02(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("[2]", result.reason)
        self.assertIn("heat_blocked", result.reason)
        self.assertIn("zone_diag_samples", result.observed)

    def test_pass_is_unaffected_by_diagnostic_capture(self):
        statuses = [
            _ExecStatus("running", []),
            _ExecStatus("done", []),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)

        def get_json(host, path):
            if path == "/api/profile_exec":
                return 200, {"zones": [
                    {"zone": 0, "duty": 0.5, "heat_blocked": False},
                    {"zone": 1, "duty": 0.5, "heat_blocked": False},
                    {"zone": 2, "duty": 0.5, "heat_blocked": False},
                ]}
            return 200, {}

        ctx = self._ctx(profiles, get_json)
        ctx["srv"]._thermo.read = self._stepped_thermo(
            [_Reading(0, 30.0), _Reading(1, 30.0), _Reading(2, 30.0)]
        )
        result = C._case_hp02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_fail_reason_unchanged_when_no_zone_was_blocked_while_energized(self):
        """A FAIL with no duty>0-and-blocked zone must not fabricate a
        blocked-zone claim -- the original judge reason is kept as-is."""
        statuses = [
            _ExecStatus("running", []),
            _ExecStatus("done", []),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)

        def get_json(host, path):
            return 200, {"zones": [
                {"zone": 0, "duty": 0.5, "heat_blocked": False},
                {"zone": 1, "duty": 0.5, "heat_blocked": False},
                {"zone": 2, "duty": 0.5, "heat_blocked": False},
            ]} if path == "/api/profile_exec" else (200, {})

        ctx = self._ctx(profiles, get_json)
        ctx["srv"]._thermo.read = self._stepped_thermo(
            [_Reading(0, 30.0), _Reading(1, 30.0), _Reading(2, 25.9)]
        )
        result = C._case_hp02(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertNotIn("heat_blocked", result.reason)

    def test_diag_samples_collected_across_poll_ticks(self):
        statuses = [
            _ExecStatus("running", []),
            _ExecStatus("running", []),
            _ExecStatus("done", []),
        ]
        profiles = _FakeProfilesClientHP(exec_statuses=statuses)
        calls = {"n": 0}

        def get_json(host, path):
            if path == "/api/profile_exec":
                calls["n"] += 1
                return 200, {"zones": [{"zone": 0, "duty": 0.5, "heat_blocked": False}]}
            return 200, {}

        ctx = self._ctx(profiles, get_json)
        C._case_hp02(ctx)
        run = ctx["_hp02"]
        self.assertEqual(len(run["zone_diag_samples"]), 3)
        self.assertEqual(calls["n"], 3)
