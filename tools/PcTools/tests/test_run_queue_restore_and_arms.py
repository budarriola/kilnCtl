#!/usr/bin/env python3
"""Unit tests for ROADMAP.md M15 B9: campaign-runner board-state restore +
arms-differ preflight (run_queue.py).

RESTORE-ON-EXIT: on abnormal termination of the queue loop (any exception,
including KeyboardInterrupt) AFTER the board has been touched, run_queue
re-applies the campaign's baseline preset (last entry's preset by default)
-- but only once the board is confirmed idle (profile_exec not
running/paused), and records attempted/succeeded/failed into the campaign
state file. Normal completion never restores.

ARMS-DIFFER PREFLIGHT: before a queue with >1 distinct preset starts, every
pair of distinct presets' LOCAL payloads (excluding the "name" field --
presets are distinct by name almost by definition, what must differ is the
actual board-bound content) must differ in at least one field, or the
queue refuses naming the identical pair.

All HTTP is mocked exactly like test_run_queue_preflight.py -- no real
socket, no live board, no hardware calls of any kind.

Run with:
  python -m pytest tools/PcTools/tests/test_run_queue_restore_and_arms.py -q
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
    """Rested from the start, always terminal ('done') on every exec poll.
    Tracks every apply/start call so tests can assert what actually
    happened on "the board". ``exec_state`` is mutable so a test can flip
    the board to "running" partway through to exercise the idle-gate."""

    def __init__(self):
        self.exec_state = "done"
        self.started_profile_ids = []
        self.start_ok_ids = None  # None => always ok; else set of ids that succeed
        self._t = 0.0

    def sleep(self, s):
        self._t += s

    def now(self):
        return self._t

    def get_status(self, host, timeout):
        return _status([_channel(25.0, 25.0)])

    def get_exec(self, host, timeout):
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


# --------------------------------------------------------------------------
# RESTORE-ON-EXIT
# --------------------------------------------------------------------------

class RestoreOnExitTest(_Harness):
    def test_normal_completion_no_restore(self):
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        e1 = self._entry("r1.jsonl", _preset("A", kp=1.0), profile_id=7)
        e2 = self._entry("r2.jsonl", _preset("B", kp=2.0), profile_id=8)

        rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                      preflight_fn=_permissive_preflight)

        # Only the two ordinary applies happened -- no third (restore) call.
        self.assertEqual(len(applied), 2)
        self.assertEqual([p["name"] for p in applied], ["A", "B"])

    def test_mid_queue_exception_restores_baseline_and_records_state(self):
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        e1 = self._entry("r1.jsonl", _preset("A", kp=1.0), profile_id=7)
        e2 = self._entry("r2.jsonl", _preset("B", kp=2.0), profile_id=8)
        # entry 2's start POST is refused by the board -- raises inside
        # run_entry AFTER its preset was already applied, so the board was
        # "touched" and a restore is owed.
        self.transport.start_ok_ids = {7}
        state_path = os.path.join(self.tmpdir, "state.json")

        with self.assertRaises(rq.RunQueueError):
            rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                          preflight_fn=_permissive_preflight, state_path=state_path)

        # Baseline (last entry's preset, "B") was re-applied as the THIRD
        # apply call, after e1's own apply and e2's own (failed-to-start)
        # apply.
        self.assertEqual(len(applied), 3)
        self.assertEqual([p["name"] for p in applied], ["A", "B", "B"])

        with open(state_path, "r", encoding="utf-8") as fh:
            state = json.load(fh)
        self.assertIn("restore", state)
        self.assertTrue(state["restore"]["attempted"])
        self.assertTrue(state["restore"]["succeeded"])
        self.assertIsNone(state["restore"]["error"])
        self.assertEqual(state["restore"]["baseline_preset"], str(_preset("B", kp=2.0)))

    def test_restore_failure_is_loud_and_actionable(self):
        calls = []

        def fake_apply(control, preset, zones_host=None):
            calls.append(preset)
            if len(calls) == 3:
                raise RuntimeError("simulated: board unreachable during restore")
            return object()

        e1 = self._entry("r1.jsonl", _preset("A", kp=1.0), profile_id=7)
        e2 = self._entry("r2.jsonl", _preset("B", kp=2.0), profile_id=8)
        self.transport.start_ok_ids = {7}
        state_path = os.path.join(self.tmpdir, "state.json")

        with self.assertLogs(rq.log, level="ERROR") as cm:
            with self.assertRaises(rq.RunQueueError):
                rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                              preflight_fn=_permissive_preflight, state_path=state_path)

        joined = "\n".join(cm.output)
        self.assertIn("CHECK BY HAND", joined)
        self.assertIn("RESTORE-ON-EXIT FAILED", joined)

        with open(state_path, "r", encoding="utf-8") as fh:
            state = json.load(fh)
        self.assertTrue(state["restore"]["attempted"])
        self.assertFalse(state["restore"]["succeeded"])
        self.assertIn("simulated: board unreachable during restore", state["restore"]["error"])

    def test_no_restore_if_board_never_touched(self):
        """A failure that happens before entry 0 is ever attempted (here:
        the arms-differ preflight refusing two identical presets) must not
        attempt a restore at all -- there is nothing to restore, and no
        apply call should be made for it."""
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        same = _preset("A", kp=1.0)
        e1 = self._entry("r1.jsonl", dict(same), profile_id=7)
        e2 = self._entry("r2.jsonl", dict(same, name="B"), profile_id=8)

        with self.assertRaises(rq.RunQueueError):
            rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                          preflight_fn=_permissive_preflight)

        self.assertEqual(applied, [])  # never even reached entry 0's apply

    def test_restore_skipped_while_board_still_firing(self):
        """If the board is still actively firing (running/paused) when the
        abnormal exit is handled, restore must be SKIPPED (never race an
        active firing) -- and that skip must be recorded, not silent."""
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        e1 = self._entry("r1.jsonl", _preset("A", kp=1.0), profile_id=7)
        e2 = self._entry("r2.jsonl", _preset("B", kp=2.0), profile_id=8)
        self.transport.start_ok_ids = {7}
        state_path = os.path.join(self.tmpdir, "state.json")

        # Entries 1 and 2 both run/fail with the board genuinely idle
        # (exec_state stays "done" throughout, same as every other test
        # here -- only the RESTORE probe itself, made right after the
        # abnormal exit, needs to see the board as still actively firing --
        # e.g. some OTHER firing genuinely started between entry 2's own
        # failure and the restore attempt). _board_is_idle is exactly that
        # one probe, so it is patched directly rather than flipping
        # exec_state for the whole run, which would also make entry 1's own
        # run-to-terminal poll (and, with poll_interval_s=0, cfg.now())
        # never advance -- an infinite loop, not a test of the skip path.
        with unittest.mock.patch.object(rq, "_board_is_idle", return_value=False):
            with self.assertLogs(rq.log, level="WARNING") as cm:
                with self.assertRaises(rq.RunQueueError):
                    rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                                  preflight_fn=_permissive_preflight, state_path=state_path)

        # Only the two ordinary applies -- no restore apply call was made.
        self.assertEqual(len(applied), 2)
        self.assertTrue(any("SKIPPING restore" in line for line in cm.output))

        with open(state_path, "r", encoding="utf-8") as fh:
            state = json.load(fh)
        self.assertFalse(state["restore"]["attempted"])
        self.assertIn("still actively firing", state["restore"]["error"])

    def test_explicit_baseline_preset_overrides_last_entry(self):
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        e1 = self._entry("r1.jsonl", _preset("A", kp=1.0), profile_id=7)
        e2 = self._entry("r2.jsonl", _preset("B", kp=2.0), profile_id=8)
        self.transport.start_ok_ids = {7}
        explicit_baseline = _preset("OFF", kp=0.0)

        with self.assertRaises(rq.RunQueueError):
            rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                          preflight_fn=_permissive_preflight,
                          baseline_preset=explicit_baseline)

        self.assertEqual([p["name"] for p in applied], ["A", "B", "OFF"])


# --------------------------------------------------------------------------
# ARMS-DIFFER PREFLIGHT
# --------------------------------------------------------------------------

class ArmsDifferPreflightTest(_Harness):
    def test_identical_arms_refused_naming_the_pair(self):
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        same = _preset("A", kp=1.0)
        e1 = self._entry("r1.jsonl", dict(same), profile_id=7)
        e2 = self._entry("r2.jsonl", dict(same, name="B"), profile_id=8)

        with self.assertRaises(rq.RunQueueError) as cm:
            rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                          preflight_fn=_permissive_preflight)

        msg = str(cm.exception)
        self.assertIn("'A'", msg)
        self.assertIn("'B'", msg)
        self.assertEqual(applied, [])
        self.assertEqual(self.transport.started_profile_ids, [])

    def test_differing_arms_start_normally(self):
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        e1 = self._entry("r1.jsonl", _preset("A", kp=1.0), profile_id=7)
        e2 = self._entry("r2.jsonl", _preset("B", kp=2.0), profile_id=8)

        rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                      preflight_fn=_permissive_preflight)

        self.assertEqual(len(applied), 2)
        self.assertEqual(self.transport.started_profile_ids, [7, 8])

    def test_single_preset_repeat_not_checked(self):
        """A queue that only ever uses ONE preset (e.g. --repeat) has no
        pair to compare -- must never be refused by the arms-differ check."""
        applied = []

        def fake_apply(control, preset, zones_host=None):
            applied.append(preset)
            return object()

        p = _preset("A", kp=1.0)
        e1 = self._entry("r1.jsonl", dict(p), profile_id=7)
        e2 = self._entry("r2.jsonl", dict(p), profile_id=7)

        rq.run_queue([e1, e2], self.cfg, apply_preset_fn=fake_apply,
                      preflight_fn=_permissive_preflight)

        self.assertEqual(len(applied), 2)

    def test_check_arms_differ_pure_function(self):
        """Direct unit coverage of the pure comparator, independent of the
        run_queue wiring above."""
        a = _preset("A", kp=1.0)
        b = _preset("B", kp=1.0)  # only "name" differs -- payload identical
        c = _preset("C", kp=2.0)

        with self.assertRaises(rq.RunQueueError):
            rq._check_arms_differ({"A": a, "B": b})
        rq._check_arms_differ({"A": a, "C": c})  # must not raise
        rq._check_arms_differ({"A": a})  # single entry -- must not raise

    def test_prose_metadata_does_not_mask_identical_arms(self):
        """REGRESSION: excluding only "name" was not enough. Every real A/B
        pair in config_presets/ also carries a per-arm "description", so when
        the substantive field went missing -- exactly the failure this check
        exists to catch -- the two payloads still differed on that prose and
        the check stayed silent. Prose/bookkeeping keys must all be excluded."""
        a = {"name": "arm_a", "description": "ease-off 2.0 arm",
             "zones": [{"index": 0, "pid_kp": 1.0}]}
        b = {"name": "arm_b", "description": "ease-off 3.0 arm (field was dropped)",
             "zones": [{"index": 0, "pid_kp": 1.0}]}
        with self.assertRaises(rq.RunQueueError):
            rq._check_arms_differ({"A": a, "B": b})


if __name__ == "__main__":
    unittest.main()
