#!/usr/bin/env python3
"""Unit tests for kilnctrl.run_queue -- the preset->rested->ceiling->start->
capture->cooldown harness for coupling-matrix A/B firings.

The safety-refusal tests (ceiling, rested) are pure-fixture: no HTTP, no
board. The end-to-end run_entry test drives a fake HTTP transport (patched
urllib.request.urlopen, same convention as test_dashboard_http_client.py /
test_zones_http_client.py) plus a fake apply_preset/control pair, so the
whole apply->wait->check->start->poll->cooldown sequence is exercised
without a socket or a live link.

Run with: python -m pytest tools/PcTools/tests/test_run_queue.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import run_queue as rq  # noqa: E402


# --------------------------------------------------------------------------
# Pure safety-check fixtures
# --------------------------------------------------------------------------

def _status(channels):
    return {"channels": channels}


def _channel(temp_c, cj_c, valid=True):
    return {"channel": 0, "temp_c": temp_c, "cj_c": cj_c, "valid": valid}


def _zones(max_temps):
    return {"zones": [{"index": i, "max_temp_c": m} for i, m in enumerate(max_temps)]}


def _plan(points_c):
    return {"points": [{"t": i * 10, "c": c} for i, c in enumerate(points_c)]}


def _exec(fault_guards, state="running"):
    return {"state": state, "zones": [{"zone": i, "fault_guard": fg} for i, fg in enumerate(fault_guards)]}


class IsRestedTest(unittest.TestCase):
    def test_within_tolerance_is_rested(self):
        st = _status([_channel(25.3, 25.0), _channel(24.6, 25.0)])
        self.assertTrue(rq.is_rested(st, tol_c=1.0))

    def test_over_tolerance_is_not_rested(self):
        st = _status([_channel(30.0, 25.0), _channel(24.6, 25.0)])
        self.assertFalse(rq.is_rested(st, tol_c=1.0))

    def test_invalid_channel_ignored(self):
        st = _status([_channel(99.0, 25.0, valid=False), _channel(25.2, 25.0)])
        self.assertTrue(rq.is_rested(st, tol_c=1.0))

    def test_no_channels_is_not_rested(self):
        self.assertFalse(rq.is_rested(_status([]), tol_c=1.0))

    def test_all_invalid_is_not_rested(self):
        st = _status([_channel(99.0, 25.0, valid=False)])
        self.assertFalse(rq.is_rested(st, tol_c=1.0))


class CheckNoFaultTest(unittest.TestCase):
    def test_all_zero_ok(self):
        rq.check_no_fault(_exec([0, 0, 0]))  # must not raise

    def test_any_nonzero_raises(self):
        with self.assertRaises(rq.RunQueueFaultError):
            rq.check_no_fault(_exec([0, 2, 0]))


class CheckTargetsWithinCeilingTest(unittest.TestCase):
    def test_within_ceiling_ok(self):
        rq.check_targets_within_ceiling(_plan([20, 45, 60, 45]), _zones([80.0, 80.0, 80.0]))

    def test_over_ceiling_raises(self):
        with self.assertRaises(rq.RunQueueError):
            rq.check_targets_within_ceiling(_plan([20, 45, 90, 45]), _zones([80.0, 80.0, 80.0]))

    def test_one_low_ceiling_zone_still_refuses(self):
        # profile targets 60C; zone 1 only rated to 55C -- even though
        # zones 0 and 2 are fine, the whole start must be refused, since
        # /api/profile_plan does not report which zones the profile drives.
        with self.assertRaises(rq.RunQueueError):
            rq.check_targets_within_ceiling(_plan([20, 60]), _zones([80.0, 55.0, 80.0]))

    def test_zero_ceiling_is_refused(self):
        # max_temp_c == 0.0 is zones_http.c's "no ceiling configured" --
        # this harness treats that as unsafe to fire into, same as a real
        # over-ceiling target.
        with self.assertRaises(rq.RunQueueError):
            rq.check_targets_within_ceiling(_plan([20, 30]), _zones([80.0, 0.0]))

    def test_no_points_raises(self):
        with self.assertRaises(rq.RunQueueError):
            rq.check_targets_within_ceiling(_plan([]), _zones([80.0]))


# --------------------------------------------------------------------------
# HTTP transport tests -- patched urllib.request.urlopen.
# --------------------------------------------------------------------------

class _FakeResp:
    def __init__(self, body: bytes):
        self._body = body

    def read(self):
        return self._body

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class HttpTransportTest(unittest.TestCase):
    def test_get_status_parses_json(self):
        body = json.dumps({"channels": []}).encode()
        with unittest.mock.patch("urllib.request.urlopen", return_value=_FakeResp(body)):
            result = rq.get_status("203.0.113.10")
        self.assertEqual(result, {"channels": []})

    def test_start_profile_parses_json(self):
        body = json.dumps({"ok": True}).encode()
        with unittest.mock.patch("urllib.request.urlopen", return_value=_FakeResp(body)):
            result = rq.start_profile("203.0.113.10", 7)
        self.assertEqual(result, {"ok": True})


# --------------------------------------------------------------------------
# End-to-end run_entry against a scripted fake transport.
# --------------------------------------------------------------------------

class _ScriptedTransport:
    """Feeds run_entry a scripted sequence of exec/status bodies so the
    whole apply -> wait_rested -> ceiling-check -> start -> poll -> cooldown
    flow can be exercised deterministically, no sockets, no real sleeps."""

    def __init__(self, status_sequence, exec_sequence, plan_body, zones_body):
        self._status_seq = list(status_sequence)
        self._exec_seq = list(exec_sequence)
        self._plan_body = plan_body
        self._zones_body = zones_body
        self.started_profile_id = None
        self.stopped = False
        self.sleeps = []
        self._t = 0.0

    def sleep(self, s):
        self.sleeps.append(s)
        self._t += s

    def now(self):
        return self._t

    def get_status(self, host, timeout):
        return self._status_seq.pop(0) if len(self._status_seq) > 1 else self._status_seq[0]

    def get_exec(self, host, timeout):
        return self._exec_seq.pop(0) if len(self._exec_seq) > 1 else self._exec_seq[0]

    def get_zones(self, host, timeout):
        return self._zones_body

    def get_profile_plan(self, host, profile_id, timeout):
        return self._plan_body

    def start_profile(self, host, profile_id, timeout):
        self.started_profile_id = profile_id
        return {"ok": True}


def _patched_run_entry(entry, cfg, transport, preset_dict):
    """Runs run_entry with every HTTP call monkeypatched onto `transport`,
    and a fake apply_preset_fn so no config_presets/UART machinery is
    needed. Mirrors run_entry's own call sequence one-for-one."""
    calls = {"applied": None}

    def fake_apply_preset(control, preset, zones_host=None):
        calls["applied"] = (preset, zones_host)
        return object()

    with unittest.mock.patch.object(rq, "get_status", transport.get_status), \
         unittest.mock.patch.object(rq, "get_exec", transport.get_exec), \
         unittest.mock.patch.object(rq, "get_zones", transport.get_zones), \
         unittest.mock.patch.object(rq, "get_profile_plan", transport.get_profile_plan), \
         unittest.mock.patch.object(rq, "start_profile", transport.start_profile):
        rq.run_entry(entry, cfg, control=None, apply_preset_fn=fake_apply_preset)
    return calls


class ApplyPresetHttpOnlyTest(unittest.TestCase):
    """The no-serial-port path (control=None): every field a coupling-only
    preset carries is written over POST /api/zones alone -- no UART, no
    crash. Regression coverage for the AttributeError
    ('NoneType' object has no attribute 'set_zone_pid') that
    config_presets.apply_preset(None, ...) used to raise unconditionally.

    HARD NETWORK GUARD: every test here patches urllib.request.urlopen to
    raise if it is ever actually called, on top of the zones_host value
    itself being a non-routable RFC 5737 TEST-NET-3 address
    (203.0.113.10) -- belt and suspenders so a future regression (e.g. a
    mutation that disables the model-field refusal, as this module's own
    mutation-red proof does) fails loudly in-process instead of ever
    reaching a real socket, real or test board included."""

    def setUp(self):
        patcher = unittest.mock.patch(
            "urllib.request.urlopen",
            side_effect=AssertionError(
                "test attempted a real HTTP call -- zones_http_client.apply_zone_preset "
                "should have been mocked before this code path could be reached"))
        self._urlopen_guard = patcher.start()
        self.addCleanup(patcher.stop)

    def _preset(self, **zone_overrides):
        zone = {
            "index": 0, "relay_mask": 1, "control_mode": 2, "cal_offset_c": 0.0,
            "pid_kp": 0.03, "pid_ki": 0.0001, "pid_kd": 0.8,
            "max_ramp_c_per_hr": 900.0, "max_temp_c": 80.0, "min_temp_c": 0.0,
            "coupling_coeff": [0.0, 10.0],
        }
        zone.update(zone_overrides)
        return {"name": "fake_coupling_only", "zones": [zone]}

    def test_coupling_only_preset_applies_over_http_with_no_control(self):
        preset = self._preset()
        with unittest.mock.patch.object(
                rq.zones_http_client, "apply_zone_preset",
                return_value=rq.zones_http_client.ZonesApplyResult(ok=True)) as mock_apply:
            result = rq._apply_preset_http_only(None, preset, zones_host="203.0.113.10")
        self.assertTrue(result.ok)
        mock_apply.assert_called_once()
        self.assertEqual(mock_apply.call_args.args[0], "203.0.113.10")

    def test_verify_mismatch_raises(self):
        preset = self._preset()
        with unittest.mock.patch.object(
                rq.zones_http_client, "apply_zone_preset",
                return_value=rq.zones_http_client.ZonesApplyResult(
                    ok=False, mismatches=["zone 0: pid_kp expected 0.03, got 0.05"])):
            with self.assertRaises(rq.RunQueueError):
                rq._apply_preset_http_only(None, preset, zones_host="203.0.113.10")

    def test_model_field_preset_refuses_and_names_the_zone(self):
        # A preset carrying a thermal model has no HTTP write path for it --
        # must refuse up front (naming the zone), never silently drop the
        # field or silently require a serial port with no explanation.
        preset = self._preset(k_dc=31.9, tau_s=166.9, dead_time_s=41.1)
        with self.assertRaises(rq.RunQueueError) as ctx:
            rq._apply_preset_http_only(None, preset, zones_host="203.0.113.10")
        message = str(ctx.exception)
        self.assertIn("0", message)  # names zone index 0
        self.assertIn("serial-port", message)

    def test_nonNone_control_is_an_internal_error(self):
        preset = self._preset()
        with self.assertRaises(rq.RunQueueError):
            rq._apply_preset_http_only(object(), preset, zones_host="203.0.113.10")

    def test_no_zones_host_refuses(self):
        preset = self._preset()
        with self.assertRaises(rq.RunQueueError):
            rq._apply_preset_http_only(None, preset, zones_host=None)

    def test_run_entry_default_selection_does_not_crash_with_no_control(self):
        """End-to-end: run_entry's default apply_preset_fn selection (no
        explicit override) must pick the HTTP-only path when control=None,
        not crash trying to call a method on it."""
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")
        preset = self._preset()

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=[_exec([0], state="running"), _exec([0], state="done")],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0]),
        )
        entry = rq.QueueEntry(preset_name="fake_coupling_only", profile_id=7,
                               log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now, cooldown_s=0.0)

        with unittest.mock.patch("kilnctrl.config_presets.load_preset_data", return_value=preset), \
             unittest.mock.patch.object(
                 rq.zones_http_client, "apply_zone_preset",
                 return_value=rq.zones_http_client.ZonesApplyResult(ok=True)), \
             unittest.mock.patch.object(rq, "get_status", transport.get_status), \
             unittest.mock.patch.object(rq, "get_exec", transport.get_exec), \
             unittest.mock.patch.object(rq, "get_zones", transport.get_zones), \
             unittest.mock.patch.object(rq, "get_profile_plan", transport.get_profile_plan), \
             unittest.mock.patch.object(rq, "start_profile", transport.start_profile):
            rq.run_entry(entry, cfg, control=None, apply_preset_fn=None)  # must not raise

        self.assertEqual(transport.started_profile_id, 7)


class RunEntryEndToEndTest(unittest.TestCase):
    def test_full_sequence_writes_run_and_cooldown_captures(self, tmp_path=None):
        import tempfile
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=[_exec([0], state="running"), _exec([0], state="done")],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7, log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now,
                                 cooldown_s=0.0)

        calls = _patched_run_entry(entry, cfg, transport, entry.preset_name)

        self.assertEqual(transport.started_profile_id, 7)
        self.assertEqual(calls["applied"][1], "203.0.113.10")
        with open(log_path) as fh:
            lines = [json.loads(line) for line in fh if line.strip()]
        self.assertGreaterEqual(len(lines), 1)
        self.assertIn("exec", lines[0])
        self.assertIn("status", lines[0])
        self.assertTrue(os.path.exists(log_path + ".cooldown.jsonl"))

    def test_over_ceiling_profile_refused_before_start(self):
        import tempfile
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=[_exec([0], state="running")],
            plan_body=_plan([20, 45, 90]),  # 90C peak
            zones_body=_zones([80.0, 80.0, 80.0]),  # all zones capped at 80C
        )
        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7, log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now)

        with self.assertRaises(rq.RunQueueError):
            _patched_run_entry(entry, cfg, transport, entry.preset_name)
        self.assertIsNone(transport.started_profile_id)

    def test_unrested_start_refused(self):
        import tempfile
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")

        # Never settles -- stays 10C above cold junction the whole time.
        warm = _status([_channel(35.0, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[warm],
            exec_sequence=[_exec([0], state="running")],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7, log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.02,
                                 sleep=transport.sleep, now=transport.now,
                                 rested_timeout_s=0.01)

        with self.assertRaises(rq.RunQueueError):
            _patched_run_entry(entry, cfg, transport, entry.preset_name)
        self.assertIsNone(transport.started_profile_id)

    def test_fault_mid_run_stops_queue(self):
        import tempfile
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=[_exec([0, 2, 0], state="running")],  # zone 1 faulted
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7, log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now)

        with self.assertRaises(rq.RunQueueFaultError):
            _patched_run_entry(entry, cfg, transport, entry.preset_name)
        self.assertEqual(transport.started_profile_id, 7)  # it did start, then faulted


if __name__ == "__main__":
    unittest.main()
