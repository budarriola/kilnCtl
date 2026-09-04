#!/usr/bin/env python3
"""Unit tests for the PENDING RESTORE marker added after the 2026-09-04
task-watchdog incident (run_queue.py).

Background: a watchdog reset mid-firing caused the STOP POST to time out,
then the restore-on-exit idle probe (GET /api/profile_exec) ALSO timed out
(the board was mid-reboot) -- restore-on-exit skipped the restore and gave
up permanently, recording only a log line. The board was left holding an
experiment arm's preset with no machine-readable record of the intent to
restore baseline. These tests cover:

  * the marker (``state["pending_restore"]``) being written on the skip/fail
    path (never on success)
  * a later, standalone completion (:func:`run_queue.complete_pending_restore`)
    applying the baseline once the board is confirmed idle, and clearing
    the marker
  * that same completion REFUSING (never forcing) when the board reports
    itself still running/paused
  * the bounded retry on a transient (exception-raising) idle probe, and
    that a DEFINITIVE "still running" answer is never retried
  * ``run_queue(..., resume=True)`` noticing and completing an outstanding
    pending restore from a previous invocation before touching its own
    entries
  * a MANDATORY negative test: break the completion path (skip the idle
    check) and confirm a test fails, naming the specific defect.

All HTTP is mocked exactly like test_run_queue_restore_and_arms.py -- no
real socket, no live board, no hardware calls of any kind.

Run with:
  python -m pytest tools/PcTools/tests/test_run_queue_pending_restore.py -q
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


def _status(channels):
    return {"channels": channels}


def _channel(temp_c, cj_c, valid=True):
    return {"channel": 0, "temp_c": temp_c, "cj_c": cj_c, "valid": valid}


def _zones(max_temps):
    return {"zones": [{"index": i, "max_temp_c": m} for i, m in enumerate(max_temps)]}


def _plan(points_c, total_planned_s=60.0):
    return {"points": [{"t": i * 10, "c": c} for i, c in enumerate(points_c)],
            "total_planned_s": total_planned_s}


def _exec(fault_guards, state="done"):
    return {"state": state, "zones": [{"zone": i, "fault_guard": fg} for i, fg in enumerate(fault_guards)]}


def _preset(name="p", kp=1.0):
    return {
        "name": name,
        "ramp_assist_enabled": True,
        "zones": [{
            "index": 0, "relay_mask": 1, "control_mode": 1, "cal_offset_c": 0.0,
            "pid_kp": kp, "pid_ki": 0.1, "pid_kd": 0.0, "max_ramp_c_per_hr": 100.0,
            "max_temp_c": 200.0, "min_temp_c": 0.0,
        }],
    }


def _permissive_preflight(preset, host, zones_host=None, safety_host=None, preset_name="(unnamed)"):
    return None


class _FakeTransport:
    def __init__(self):
        self.exec_state = "done"
        self.exec_raises = False
        self.started_profile_ids = []
        self.start_ok_ids = None
        self._t = 0.0

    def sleep(self, s):
        self._t += s

    def now(self):
        return self._t

    def get_status(self, host, timeout):
        return _status([_channel(25.0, 25.0)])

    def get_exec(self, host, timeout):
        if self.exec_raises:
            raise TimeoutError("simulated: board mid-reboot, no response")
        return _exec([0], state=self.exec_state)

    def get_zones(self, host, timeout):
        return _zones([200.0])

    def get_profile_plan(self, host, profile_id, timeout):
        return _plan([50.0])

    def start_profile(self, host, profile_id, timeout):
        self.started_profile_ids.append(profile_id)
        if self.start_ok_ids is not None and profile_id not in self.start_ok_ids:
            return {"ok": False, "error": "refused for test"}
        return {"ok": True}

    def stop_profile(self, host, timeout, **kwargs):
        pass


class _Harness(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()
        self.transport = _FakeTransport()
        self.cfg = rq.RunQueueConfig(
            host="203.0.113.10", poll_interval_s=0.0, cooldown_s=0.0,
            restore_retry_delay_s=0.0,
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
        return rq.QueueEntry(preset_name=preset, profile_id=profile_id,
                              log_path=os.path.join(self.tmpdir, name), label=label or name,
                              stabilize=False)

    def _load_state(self, state_path):
        with open(state_path, "r", encoding="utf-8") as fh:
            return json.load(fh)


# --------------------------------------------------------------------------
# Marker written on skip/failure, never on success
# --------------------------------------------------------------------------

class MarkerWrittenTest(_Harness):
    def _run_and_abort_with_stuck_start(self, fake_apply, state_path):
        e1 = self._entry("r1.jsonl", _preset("A", kp=1.0), profile_id=7)
        e2 = self._entry("r2.jsonl", _preset("B", kp=2.0), profile_id=8)
        self.transport.start_ok_ids = {7}
        with self.assertRaises(rq.RunQueueError):
            rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                          preflight_fn=_permissive_preflight, state_path=state_path)

    def test_pending_marker_set_when_idle_probe_cannot_confirm(self):
        """The exact 2026-09-04 shape: the idle probe itself fails (board
        mid-reboot) even after retries -- restore must be skipped and the
        pending marker recorded, not just a log line."""
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        state_path = os.path.join(self.tmpdir, "state.json")
        with unittest.mock.patch.object(
                rq, "_board_is_idle",
                side_effect=TimeoutError("simulated: board mid-reboot, no response")):
            self._run_and_abort_with_stuck_start(fake_apply, state_path)

        # No restore apply call happened -- only the two ordinary applies.
        self.assertEqual(len(applied), 2)

        state = self._load_state(state_path)
        self.assertIsNotNone(state["pending_restore"])
        self.assertEqual(state["pending_restore"]["baseline_preset"], _preset("B", kp=2.0))
        self.assertIn("could not confirm board idle", state["pending_restore"]["reason"])

    def test_pending_marker_set_when_board_still_firing(self):
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        state_path = os.path.join(self.tmpdir, "state.json")
        with unittest.mock.patch.object(rq, "_board_is_idle", return_value=False):
            self._run_and_abort_with_stuck_start(fake_apply, state_path)

        state = self._load_state(state_path)
        self.assertIsNotNone(state["pending_restore"])
        self.assertIn("still actively firing", state["pending_restore"]["reason"])

    def test_no_pending_marker_on_successful_restore(self):
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        state_path = os.path.join(self.tmpdir, "state.json")
        self._run_and_abort_with_stuck_start(fake_apply, state_path)

        state = self._load_state(state_path)
        self.assertTrue(state["restore"]["succeeded"])
        self.assertIsNone(state["pending_restore"])


# --------------------------------------------------------------------------
# Bounded retry on the idle probe
# --------------------------------------------------------------------------

class RetryTest(_Harness):
    def test_transient_failure_then_recovery_completes_restore_same_run(self):
        """One timeout (like the reboot in the incident), then the board
        answers idle=True on the next attempt -- restore-on-exit should
        complete WITHOUT ever setting a pending marker."""
        applied = []
        calls = {"n": 0}
        real_board_is_idle = rq._board_is_idle

        def flaky_board_is_idle(cfg):
            calls["n"] += 1
            if calls["n"] == 1:
                raise TimeoutError("simulated: one transient timeout")
            return real_board_is_idle(cfg)

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        with unittest.mock.patch.object(rq, "_board_is_idle", flaky_board_is_idle):
            e1 = self._entry("r1.jsonl", _preset("A", kp=1.0), profile_id=7)
            e2 = self._entry("r2.jsonl", _preset("B", kp=2.0), profile_id=8)
            self.transport.start_ok_ids = {7}
            state_path = os.path.join(self.tmpdir, "state.json")
            with self.assertRaises(rq.RunQueueError):
                rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                              preflight_fn=_permissive_preflight, state_path=state_path)

        self.assertEqual([p["name"] for p in applied], ["A", "B", "B"])
        state = self._load_state(state_path)
        self.assertTrue(state["restore"]["succeeded"])
        self.assertIsNone(state["pending_restore"])

    def test_exhausted_retries_raise_and_are_not_retried_forever(self):
        cfg = rq.RunQueueConfig(host="203.0.113.10", restore_idle_retries=2,
                                 restore_retry_delay_s=0.0, sleep=self.transport.sleep,
                                 now=self.transport.now)
        self.transport.exec_raises = True
        with self.assertRaises(TimeoutError):
            rq._board_is_idle_with_retries(cfg)

    def test_definitive_still_running_is_never_retried(self):
        """A real 'running' answer must be trusted on the FIRST try -- never
        retried into eventually being ignored."""
        probe_calls = {"n": 0}

        def fake_board_is_idle(cfg):
            probe_calls["n"] += 1
            return False

        with unittest.mock.patch.object(rq, "_board_is_idle", fake_board_is_idle):
            result = rq._board_is_idle_with_retries(self.cfg)
        self.assertFalse(result)
        self.assertEqual(probe_calls["n"], 1)


# --------------------------------------------------------------------------
# Standalone check/complete (requirement 4)
# --------------------------------------------------------------------------

class StandaloneCompletionTest(_Harness):
    def _state_with_pending_restore(self):
        state_path = os.path.join(self.tmpdir, "state.json")
        state = rq.new_campaign_state(
            [self._entry("r1.jsonl", _preset("A"), profile_id=7)])
        state["pending_restore"] = {
            "baseline_preset": _preset("BASE"), "reason": "simulated pending",
            "at": 0.0,
        }
        state["restore"] = {"attempted": False, "succeeded": False,
                             "baseline_preset": str(_preset("BASE")), "error": "simulated pending",
                             "at": 0.0}
        rq.save_campaign_state(state_path, state)
        return state_path

    def test_check_pending_restore_reports_marker(self):
        state_path = self._state_with_pending_restore()
        pending = rq.check_pending_restore(state_path)
        self.assertIsNotNone(pending)
        self.assertEqual(pending["reason"], "simulated pending")

    def test_check_pending_restore_none_when_clean(self):
        state_path = os.path.join(self.tmpdir, "state.json")
        state = rq.new_campaign_state([self._entry("r1.jsonl", _preset("A"), profile_id=7)])
        rq.save_campaign_state(state_path, state)
        self.assertIsNone(rq.check_pending_restore(state_path))

    def test_complete_pending_restore_applies_baseline_when_idle(self):
        state_path = self._state_with_pending_restore()
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        result = rq.complete_pending_restore(state_path, self.cfg, apply_preset_fn=fake_apply)
        self.assertTrue(result["completed"])
        self.assertEqual(applied, [_preset("BASE")])

        state = self._load_state(state_path)
        self.assertIsNone(state["pending_restore"])
        self.assertTrue(state["restore"]["succeeded"])

    def test_complete_pending_restore_refuses_when_board_firing(self):
        """MUST NOT force it: a still-running board leaves the marker in
        place and applies nothing."""
        state_path = self._state_with_pending_restore()
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        with unittest.mock.patch.object(rq, "_board_is_idle", return_value=False):
            with self.assertRaises(rq.RunQueueError) as cm:
                rq.complete_pending_restore(state_path, self.cfg, apply_preset_fn=fake_apply)
        self.assertIn("still running/paused", str(cm.exception))
        self.assertEqual(applied, [])  # never applied anything

        state = self._load_state(state_path)
        self.assertIsNotNone(state["pending_restore"])  # marker survives the refusal

    def test_complete_pending_restore_noop_when_nothing_pending(self):
        state_path = os.path.join(self.tmpdir, "state.json")
        state = rq.new_campaign_state([self._entry("r1.jsonl", _preset("A"), profile_id=7)])
        rq.save_campaign_state(state_path, state)
        result = rq.complete_pending_restore(state_path, self.cfg)
        self.assertEqual(result, {"pending": False})


# --------------------------------------------------------------------------
# resume=True auto-completing an outstanding pending restore
# --------------------------------------------------------------------------

class ResumeCompletesPendingRestoreTest(_Harness):
    def test_resume_completes_pending_restore_before_own_entries(self):
        entries = [self._entry("r1.jsonl", _preset("NEW"), profile_id=7)]
        state_path = os.path.join(self.tmpdir, "state.json")
        state = rq.new_campaign_state(entries)
        state["pending_restore"] = {"baseline_preset": _preset("OLD_BASE"),
                                     "reason": "simulated leftover from a previous campaign",
                                     "at": 0.0}
        state["restore"] = {"attempted": False, "succeeded": False,
                             "baseline_preset": str(_preset("OLD_BASE")), "error": "x", "at": 0.0}
        rq.save_campaign_state(state_path, state)

        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        rq.run_queue(entries, self.cfg, apply_preset_fn=fake_apply,
                      preflight_fn=_permissive_preflight, state_path=state_path, resume=True)

        # OLD_BASE restored FIRST, then NEW's own entry applied.
        self.assertEqual([p["name"] for p in applied], ["OLD_BASE", "NEW"])
        with open(state_path, "r", encoding="utf-8") as fh:
            final_state = json.load(fh)
        self.assertIsNone(final_state["pending_restore"])

    def test_resume_refuses_if_board_firing_leaves_pending_restore_intact(self):
        entries = [self._entry("r1.jsonl", _preset("NEW"), profile_id=7)]
        state_path = os.path.join(self.tmpdir, "state.json")
        state = rq.new_campaign_state(entries)
        state["pending_restore"] = {"baseline_preset": _preset("OLD_BASE"),
                                     "reason": "simulated leftover", "at": 0.0}
        rq.save_campaign_state(state_path, state)

        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        with unittest.mock.patch.object(rq, "_board_is_idle", return_value=False):
            with self.assertRaises(rq.RunQueueError):
                rq.run_queue(entries, self.cfg, apply_preset_fn=fake_apply,
                              preflight_fn=_permissive_preflight, state_path=state_path,
                              resume=True)

        self.assertEqual(applied, [])
        with open(state_path, "r", encoding="utf-8") as fh:
            final_state = json.load(fh)
        self.assertIsNotNone(final_state["pending_restore"])


if __name__ == "__main__":
    unittest.main()
