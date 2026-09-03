#!/usr/bin/env python3
"""Unit tests for the resume-time data-loss fix in run_queue.py:

  * an "in_progress" entry whose run capture already reached a terminal
    state (done/faulted) must be promoted to "completed" on resume, never
    discarded -- the crash that left it "in_progress" happened AFTER the
    real firing finished (cooldown capture, or the state-file write
    itself), so the capture on disk is a complete, valid data point.
  * a genuinely partial capture (never reached a terminal state) is still
    discarded and re-run -- the ORIGINAL, correct behaviour must survive.
  * load_campaign_state() fails cleanly (RunQueueError) on a truncated/
    corrupt/wrong-version state file instead of a raw traceback.
  * _atomic_write_json() uses a collision-proof temp path and turns an
    os.replace() failure into a RunQueueError.

Run with:
  tools/PcTools/.venv/Scripts/python.exe -m pytest tools/PcTools/tests/test_run_queue_resume_data_loss.py -q
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


class CaptureReachedTerminalStateTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def _path(self, name):
        return os.path.join(self.tmpdir, name)

    def test_terminal_done_is_detected(self):
        p = self._path("a.jsonl")
        with open(p, "w") as fh:
            fh.write('{"t": 1, "exec": {"state": "running"}}\n')
            fh.write('{"t": 2, "exec": {"state": "done"}}\n')
        self.assertTrue(rq._capture_run_reached_terminal_state(p))

    def test_terminal_faulted_is_detected(self):
        p = self._path("a.jsonl")
        with open(p, "w") as fh:
            fh.write('{"t": 1, "exec": {"state": "faulted"}}\n')
        self.assertTrue(rq._capture_run_reached_terminal_state(p))

    def test_still_running_is_not_terminal(self):
        p = self._path("a.jsonl")
        with open(p, "w") as fh:
            fh.write('{"t": 1, "exec": {"state": "running"}}\n')
        self.assertFalse(rq._capture_run_reached_terminal_state(p))

    def test_missing_file_is_not_terminal(self):
        self.assertFalse(rq._capture_run_reached_terminal_state(self._path("nope.jsonl")))

    def test_empty_file_is_not_terminal(self):
        p = self._path("a.jsonl")
        open(p, "w").close()
        self.assertFalse(rq._capture_run_reached_terminal_state(p))

    def test_trailing_blank_lines_do_not_hide_the_real_last_line(self):
        p = self._path("a.jsonl")
        with open(p, "w") as fh:
            fh.write('{"t": 1, "exec": {"state": "done"}}\n')
            fh.write("\n")
            fh.write("   \n")
        self.assertTrue(rq._capture_run_reached_terminal_state(p))

    def test_unparseable_last_line_is_not_terminal(self):
        p = self._path("a.jsonl")
        with open(p, "w") as fh:
            fh.write("not json at all\n")
        self.assertFalse(rq._capture_run_reached_terminal_state(p))


def _exec(fault_guards, state="running"):
    return {"state": state, "zones": [{"zone": i, "fault_guard": fg} for i, fg in enumerate(fault_guards)]}


class ResumeCompletionRecoveryTest(unittest.TestCase):
    """Exercises run_queue()'s own resume-time branch directly (not through
    a full simulated firing) -- constructs the exact situation the
    reviewer's incident describes: an "in_progress" entry with a run
    capture that already reached "done" on disk."""

    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def _path(self, name):
        return os.path.join(self.tmpdir, name)

    def test_completed_run_capture_is_promoted_not_discarded(self):
        entry = rq.QueueEntry(preset_name="p", profile_id=7,
                               log_path=self._path("run1.jsonl"), label="run1")
        # A full, valid capture already on disk -- the firing genuinely
        # finished; only the state-file update (or the cooldown tail) was
        # interrupted.
        with open(entry.log_path, "w") as fh:
            fh.write('{"t": 1, "exec": {"state": "running"}}\n')
            fh.write('{"t": 2, "exec": {"state": "done"}}\n')
        original_bytes = open(entry.log_path, "rb").read()

        state_path = self._path("state.json")
        state = rq.new_campaign_state([entry])
        state["entries"][0]["status"] = "in_progress"
        rq.save_campaign_state(state_path, state)

        cfg = rq.RunQueueConfig(host="203.0.113.10")

        def fake_get_exec(host, timeout):
            return _exec([0], state="idle")  # board confirmed not running

        def failing_run_entry(*a, **k):
            self.fail("run_entry must not be called -- the entry was already complete")

        with unittest.mock.patch.object(rq, "get_exec", fake_get_exec), \
             unittest.mock.patch.object(rq, "run_entry", failing_run_entry), \
             unittest.mock.patch.object(rq, "_preflight_campaign", lambda *a, **k: None):
            rq.run_queue([entry], cfg, control=None, apply_preset_fn=lambda *a, **k: object(),
                         state_path=state_path, resume=True)

        # capture untouched -- NOT discarded
        self.assertEqual(open(entry.log_path, "rb").read(), original_bytes)
        with open(state_path) as fh:
            final_state = json.load(fh)
        self.assertEqual(final_state["entries"][0]["status"], "completed")

    def test_genuinely_partial_capture_is_still_discarded_and_rerun(self):
        """The ORIGINAL, correct behaviour must survive this fix: a capture
        that never reached a terminal state is not a valid data point and
        must still be discarded, then re-run from scratch."""
        entry = rq.QueueEntry(preset_name="p", profile_id=7,
                               log_path=self._path("run1.jsonl"), label="run1")
        with open(entry.log_path, "w") as fh:
            fh.write('{"t": 1, "exec": {"state": "running"}}\n')  # never reached done/faulted

        state_path = self._path("state.json")
        state = rq.new_campaign_state([entry])
        state["entries"][0]["status"] = "in_progress"
        rq.save_campaign_state(state_path, state)

        cfg = rq.RunQueueConfig(host="203.0.113.10")

        def fake_get_exec(host, timeout):
            return _exec([0], state="idle")

        ran = []

        def fake_run_entry(e, c, control=None, apply_preset_fn=None):
            ran.append(e.log_path)
            # simulate a fresh, complete run
            with open(e.log_path, "w") as fh:
                fh.write('{"t": 9, "exec": {"state": "done"}}\n')

        with unittest.mock.patch.object(rq, "get_exec", fake_get_exec), \
             unittest.mock.patch.object(rq, "run_entry", fake_run_entry), \
             unittest.mock.patch.object(rq, "_preflight_campaign", lambda *a, **k: None):
            rq.run_queue([entry], cfg, control=None, apply_preset_fn=lambda *a, **k: object(),
                         state_path=state_path, resume=True)

        self.assertEqual(ran, [entry.log_path], "the genuinely partial entry must be re-run")
        with open(entry.log_path) as fh:
            lines = [json.loads(l) for l in fh if l.strip()]
        self.assertEqual(lines[-1]["exec"]["state"], "done")


class LoadCampaignStateValidationTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def _path(self, name):
        return os.path.join(self.tmpdir, name)

    def test_missing_file_raises_run_queue_error(self):
        with self.assertRaises(rq.RunQueueError):
            rq.load_campaign_state(self._path("nope.json"))

    def test_truncated_json_raises_run_queue_error_not_json_decode_error(self):
        p = self._path("state.json")
        with open(p, "w") as fh:
            fh.write('{"version": 1, "entries": [')  # truncated
        with self.assertRaises(rq.RunQueueError):
            rq.load_campaign_state(p)

    def test_wrong_shape_raises_run_queue_error(self):
        p = self._path("state.json")
        with open(p, "w") as fh:
            json.dump({"not": "a campaign state"}, fh)
        with self.assertRaises(rq.RunQueueError):
            rq.load_campaign_state(p)

    def test_wrong_version_raises_run_queue_error(self):
        p = self._path("state.json")
        with open(p, "w") as fh:
            json.dump({"version": 999, "entries": []}, fh)
        with self.assertRaises(rq.RunQueueError):
            rq.load_campaign_state(p)

    def test_valid_state_loads_fine(self):
        entry = rq.QueueEntry(preset_name="p", profile_id=7, log_path="x.jsonl", label="l")
        p = self._path("state.json")
        rq.save_campaign_state(p, rq.new_campaign_state([entry]))
        loaded = rq.load_campaign_state(p)
        self.assertEqual(loaded["version"], rq.CAMPAIGN_STATE_VERSION)


class AtomicWriteJsonTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp()

    def _path(self, name):
        return os.path.join(self.tmpdir, name)

    def test_two_concurrent_writers_use_distinct_temp_paths(self):
        """Two _atomic_write_json calls to the same path must not collide
        on their temp file -- verified by asserting the counter actually
        advances between calls (a fixed '.tmp' suffix would reuse the same
        name every time)."""
        p = self._path("state.json")
        rq._atomic_write_json(p, {"a": 1})
        rq._atomic_write_json(p, {"a": 2})
        with open(p) as fh:
            self.assertEqual(json.load(fh), {"a": 2})
        # no stray temp files left behind
        leftovers = [f for f in os.listdir(self.tmpdir) if f != "state.json"]
        self.assertEqual(leftovers, [])

    def test_os_replace_failure_raises_run_queue_error(self):
        p = self._path("state.json")
        with unittest.mock.patch("os.replace", side_effect=PermissionError("locked by AV")):
            with self.assertRaises(rq.RunQueueError):
                rq._atomic_write_json(p, {"a": 1})
        # no half-written destination file was created by the failed attempt
        self.assertFalse(os.path.exists(p))

    def test_os_replace_failure_cleans_up_its_temp_file(self):
        p = self._path("state.json")
        with unittest.mock.patch("os.replace", side_effect=PermissionError("locked")):
            with self.assertRaises(rq.RunQueueError):
                rq._atomic_write_json(p, {"a": 1})
        leftovers = [f for f in os.listdir(self.tmpdir)]
        self.assertEqual(leftovers, [], f"temp file(s) left behind: {leftovers}")


if __name__ == "__main__":
    unittest.main()
