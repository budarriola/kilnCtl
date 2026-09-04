#!/usr/bin/env python3
"""Unit tests for run_queue.py's capability-preflight wiring: preflight
runs once, before any firing starts; a fatal capability gap or an
unreachable board aborts the whole campaign before entry 0's start POST; a
benign gap logs and continues; every entry's preset is checked (not just
the first); and --resume still preflights.

Two kinds of test here:
  * ones that inject a FAKE preflight_fn (no HTTP at all) to prove the
    WIRING -- that run_queue calls it before any start, for every distinct
    preset, and propagates its failure without ever reaching run_entry.
  * ones that use the REAL capability_preflight.preflight_or_raise with
    urllib.request.urlopen mocked (same convention as
    test_capability_preflight.py) to prove the actual fatal/benign/
    unreachable DECISIONS reach run_queue correctly end to end.

Run with:
  tools/PcTools/.venv/Scripts/python.exe -m pytest tools/PcTools/tests/test_run_queue_preflight.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import run_queue as rq  # noqa: E402
from kilnctrl import capability_preflight as cp  # noqa: E402


def _status(channels):
    return {"channels": channels}


def _channel(temp_c, cj_c, valid=True):
    return {"channel": 0, "temp_c": temp_c, "cj_c": cj_c, "valid": valid}


def _zones(max_temps):
    return {"zones": [{"index": i, "max_temp_c": m} for i, m in enumerate(max_temps)]}


def _plan(points_c, total_planned_s=60.0):
    return {"points": [{"t": i * 10, "c": c} for i, c in enumerate(points_c)],
            "total_planned_s": total_planned_s}


def _exec(fault_guards, state="running"):
    return {"state": state, "zones": [{"zone": i, "fault_guard": fg} for i, fg in enumerate(fault_guards)]}


def _preset(ramp_assist_enabled, name="p"):
    return {
        "name": name,
        "ramp_assist_enabled": ramp_assist_enabled,
        "zones": [{
            "index": 0, "relay_mask": 1, "control_mode": 1, "cal_offset_c": 0.0,
            "pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0, "max_ramp_c_per_hr": 100.0,
            "max_temp_c": 200.0, "min_temp_c": 0.0,
        }],
    }


def _fake_apply_preset(control, preset, zones_host=None):
    return object()


class _FakeTransport:
    """Rested from the start, one immediate 'done' on the first exec poll --
    used for tests that need run_entry to actually run to completion (the
    benign-continues case). Records every start_profile call so a test can
    assert none happened."""

    def __init__(self):
        self.started_profile_ids = []
        self.stopped = False
        self._t = 0.0

    def sleep(self, s):
        self._t += s

    def now(self):
        return self._t

    def get_status(self, host, timeout):
        return _status([_channel(25.0, 25.0)])

    def get_exec(self, host, timeout):
        return _exec([0], state="done")

    def get_zones(self, host, timeout):
        return _zones([200.0])

    def get_profile_plan(self, host, profile_id, timeout):
        return _plan([50.0])

    def start_profile(self, host, profile_id, timeout):
        self.started_profile_ids.append(profile_id)
        return {"ok": True}

    def stop_profile(self, host, timeout, **kwargs):
        self.stopped = True


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


_STATUS_BODY = json.dumps({"fw_version": "1.4.2", "fw_build": "x", "self_protocol_version": 3}).encode()
_RAMP_ASSIST_ABSENT_BODY = json.dumps({"ok": False, "error": "no such endpoint"}).encode()
_RAMP_ASSIST_PRESENT_BODY = json.dumps({"enabled": True}).encode()


def _urlopen_router(responses: dict):
    def _fake_urlopen(req, timeout=None):
        url = req.full_url if hasattr(req, "full_url") else req
        for key, value in responses.items():
            if key in url:
                if isinstance(value, Exception):
                    raise value
                return _fake_response(value)
        raise AssertionError(f"unexpected URL in test: {url}")

    return _fake_urlopen


class _Harness(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.transport = _FakeTransport()
        self.cfg = rq.RunQueueConfig(
            host="203.0.113.10", poll_interval_s=0.0, cooldown_s=0.0,
            sleep=self.transport.sleep, now=self.transport.now)
        self._patchers = [
            unittest.mock.patch.object(rq, "get_status", self.transport.get_status),
            unittest.mock.patch.object(rq, "get_exec", self.transport.get_exec),
            unittest.mock.patch.object(rq, "get_zones", self.transport.get_zones),
            unittest.mock.patch.object(rq, "get_profile_plan", self.transport.get_profile_plan),
            unittest.mock.patch.object(rq, "start_profile", self.transport.start_profile),
            unittest.mock.patch.object(rq, "stop_profile", self.transport.stop_profile),
        ]
        for p in self._patchers:
            p.start()
            self.addCleanup(p.stop)

    def _entry(self, name, preset, profile_id=7, label=None):
        # stabilize=False: these tests exercise the preflight-capability
        # wiring, not TASK 1's stabilisation hold, and their fake
        # urlopen router has no /api/profile stand-in for
        # ensure_stabilized_profile to hit.
        return rq.QueueEntry(preset_name=preset, profile_id=profile_id,
                              log_path=os.path.join(self.tmpdir, name), label=label or name,
                              stabilize=False)


# --------------------------------------------------------------------------
# Wiring: fake preflight_fn, no real HTTP for the probe itself.
# --------------------------------------------------------------------------

class PreflightWiringTest(_Harness):
    def test_preflight_runs_before_any_start_post_on_failure(self):
        calls = []

        def failing_preflight(preset, host, zones_host=None, safety_host=None,
                               preset_name="(unnamed)", **kwargs):
            calls.append(preset_name)
            raise cp.PreflightFailed(cp.PreflightReport(
                preset_name=preset_name, host=host,
                board=cp.BoardInfo(reachable=True), checks=[]))

        entries = [self._entry("a.jsonl", _preset(False, "p1"))]
        with self.assertRaises(cp.PreflightFailed):
            rq.run_queue(entries, self.cfg, control=None, apply_preset_fn=_fake_apply_preset,
                         preflight_fn=failing_preflight)
        self.assertEqual(calls, ["p1"])
        self.assertEqual(self.transport.started_profile_ids, [],
                          "no start POST may be issued once preflight has failed")

    def test_preflight_runs_once_up_front_not_per_entry(self):
        calls = []

        def ok_preflight(preset, host, zones_host=None, safety_host=None,
                          preset_name="(unnamed)", **kwargs):
            calls.append(preset_name)
            return None

        entries = [
            self._entry("a.jsonl", _preset(False, "p1")),
            self._entry("b.jsonl", _preset(False, "p1")),  # same preset, different entry
        ]
        rq.run_queue(entries, self.cfg, control=None, apply_preset_fn=_fake_apply_preset,
                     preflight_fn=ok_preflight)
        # p1 shared by both entries -- checked once, not twice, and both
        # calls happened before either entry's start (both entries did
        # start, but the important thing is preflight didn't run again
        # per-entry).
        self.assertEqual(calls, ["p1"])
        self.assertEqual(self.transport.started_profile_ids, [7, 7])

    def test_multi_entry_queue_checks_every_distinct_preset_before_entry_0_fires(self):
        calls = []

        def selective_preflight(preset, host, zones_host=None, safety_host=None,
                                 preset_name="(unnamed)", **kwargs):
            calls.append(preset_name)
            if preset_name == "p3":
                raise cp.PreflightFailed(cp.PreflightReport(
                    preset_name=preset_name, host=host,
                    board=cp.BoardInfo(reachable=True), checks=[]))
            return None

        entries = [
            self._entry("a.jsonl", _preset(False, "p1")),
            self._entry("b.jsonl", _preset(False, "p2")),
            self._entry("c.jsonl", _preset(False, "p3")),  # this one is fatal
        ]
        with self.assertRaises(cp.PreflightFailed):
            rq.run_queue(entries, self.cfg, control=None, apply_preset_fn=_fake_apply_preset,
                         preflight_fn=selective_preflight)
        # all three presets were checked (p3's gap was found) ...
        self.assertEqual(set(calls), {"p1", "p2", "p3"})
        # ... and entry 0 ("p1", perfectly fine on its own) never fired,
        # because the WHOLE campaign is checked before ANY entry starts.
        self.assertEqual(self.transport.started_profile_ids, [])


# --------------------------------------------------------------------------
# Decisions: real capability_preflight.preflight_or_raise, mocked urllib.
# --------------------------------------------------------------------------

class PreflightDecisionTest(_Harness):
    def test_fatal_missing_capability_aborts_before_any_start(self):
        entries = [self._entry("a.jsonl", _preset(True, "needs-ramp-assist"))]
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            with self.assertRaises(cp.PreflightFailed):
                rq.run_queue(entries, self.cfg, control=None, apply_preset_fn=_fake_apply_preset)
        self.assertEqual(self.transport.started_profile_ids, [])

    def test_benign_missing_capability_logs_and_continues(self):
        entries = [self._entry("a.jsonl", _preset(False, "no-ramp-assist-needed"))]
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            rq.run_queue(entries, self.cfg, control=None, apply_preset_fn=_fake_apply_preset)
        # the run actually proceeded and started the kiln
        self.assertEqual(self.transport.started_profile_ids, [7])

    def test_unreachable_board_aborts_and_is_not_reported_as_fine(self):
        import urllib.error
        entries = [self._entry("a.jsonl", _preset(False, "p1"))]

        def _fake_urlopen(req, timeout=None):
            raise urllib.error.URLError("no route to host")

        with unittest.mock.patch("urllib.request.urlopen", side_effect=_fake_urlopen):
            with self.assertRaises(cp.PreflightFailed) as ctx:
                rq.run_queue(entries, self.cfg, control=None, apply_preset_fn=_fake_apply_preset)
        self.assertFalse(ctx.exception.report.ok)
        self.assertFalse(ctx.exception.report.board.reachable)
        self.assertIn("UNREACHABLE", str(ctx.exception))
        self.assertEqual(self.transport.started_profile_ids, [])

    def test_present_capability_ok_and_continues(self):
        entries = [self._entry("a.jsonl", _preset(True, "p1"))]
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_PRESENT_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            rq.run_queue(entries, self.cfg, control=None, apply_preset_fn=_fake_apply_preset)
        self.assertEqual(self.transport.started_profile_ids, [7])


# --------------------------------------------------------------------------
# --resume still preflights.
# --------------------------------------------------------------------------

class ResumeStillPreflightsTest(_Harness):
    def test_resume_calls_preflight_even_when_every_entry_is_completed(self):
        calls = []

        def recording_preflight(preset, host, zones_host=None, safety_host=None,
                                 preset_name="(unnamed)", **kwargs):
            calls.append(preset_name)
            return None

        entry = self._entry("a.jsonl", _preset(False, "p1"))
        # simulate a state file recording this single entry as already
        # completed (so the entries loop itself does nothing) -- preflight
        # must still run, because the board may have been reflashed (or
        # downgraded) since the original campaign completed.
        state_path = os.path.join(self.tmpdir, "state.json")
        state = rq.new_campaign_state([entry], meta={"host": self.cfg.host})
        state["entries"][0]["status"] = "completed"
        rq._atomic_write_json(state_path, state)

        # board must read as idle for the resume precheck
        self.transport.get_exec = lambda host, timeout: _exec([0], state="idle")
        with unittest.mock.patch.object(rq, "get_exec", self.transport.get_exec):
            rq.run_queue([entry], self.cfg, control=None, apply_preset_fn=_fake_apply_preset,
                         state_path=state_path, resume=True, preflight_fn=recording_preflight)

        self.assertEqual(calls, ["p1"], "resume must still run the capability preflight")
        # nothing re-started -- the entry really was already completed
        self.assertEqual(self.transport.started_profile_ids, [])

    def test_resume_preflight_failure_aborts_before_any_pending_entry_starts(self):
        def failing_preflight(preset, host, zones_host=None, safety_host=None,
                               preset_name="(unnamed)", **kwargs):
            raise cp.PreflightFailed(cp.PreflightReport(
                preset_name=preset_name, host=host,
                board=cp.BoardInfo(reachable=True), checks=[]))

        entry = self._entry("a.jsonl", _preset(False, "p1"))
        state_path = os.path.join(self.tmpdir, "state.json")
        state = rq.new_campaign_state([entry], meta={"host": self.cfg.host})
        rq._atomic_write_json(state_path, state)  # entry left "pending"

        self.transport.get_exec = lambda host, timeout: _exec([0], state="idle")
        with unittest.mock.patch.object(rq, "get_exec", self.transport.get_exec):
            with self.assertRaises(cp.PreflightFailed):
                rq.run_queue([entry], self.cfg, control=None, apply_preset_fn=_fake_apply_preset,
                             state_path=state_path, resume=True, preflight_fn=failing_preflight)
        self.assertEqual(self.transport.started_profile_ids, [])


if __name__ == "__main__":
    unittest.main()
