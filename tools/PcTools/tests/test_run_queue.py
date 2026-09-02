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
            result = rq.get_status("192.168.1.156")
        self.assertEqual(result, {"channels": []})

    def test_start_profile_parses_json(self):
        body = json.dumps({"ok": True}).encode()
        with unittest.mock.patch("urllib.request.urlopen", return_value=_FakeResp(body)):
            result = rq.start_profile("192.168.1.156", 7)
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
        cfg = rq.RunQueueConfig(host="192.168.1.156", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now,
                                 cooldown_s=0.0)

        calls = _patched_run_entry(entry, cfg, transport, entry.preset_name)

        self.assertEqual(transport.started_profile_id, 7)
        self.assertEqual(calls["applied"][1], "192.168.1.156")
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
        cfg = rq.RunQueueConfig(host="192.168.1.156", poll_interval_s=0.0,
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
        cfg = rq.RunQueueConfig(host="192.168.1.156", poll_interval_s=0.02,
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
        cfg = rq.RunQueueConfig(host="192.168.1.156", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now)

        with self.assertRaises(rq.RunQueueFaultError):
            _patched_run_entry(entry, cfg, transport, entry.preset_name)
        self.assertEqual(transport.started_profile_id, 7)  # it did start, then faulted


if __name__ == "__main__":
    unittest.main()
