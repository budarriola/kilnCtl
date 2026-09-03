#!/usr/bin/env python3
"""Unit tests for run_queue.py's campaign-resumability additions: the state
file (written/updated durably across entries), --resume (skip completed,
discard-and-rerun an interrupted in_progress entry, never renumber/
overwrite), the unconditional clobber refusal, and the already-running-
board refusal on resume.

Run with:
  tools/PcTools/.venv/Scripts/python.exe -m pytest tools/PcTools/tests/test_run_queue_resume.py -q
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


def _plan(points_c, total_planned_s=3000.0):
    return {"points": [{"t": i * 10, "c": c} for i, c in enumerate(points_c)],
            "total_planned_s": total_planned_s}


def _exec(fault_guards, state="running"):
    return {"state": state, "zones": [{"zone": i, "fault_guard": fg} for i, fg in enumerate(fault_guards)]}


class _ScriptedTransport:
    """Same shape as test_run_queue.py's fake, but get_exec is driven by an
    explicit per-entry sequence so multiple run_entry() calls across one
    run_queue() invocation can each get their own idle->running->done
    script, plus supports a fixed "board state" for the resume precheck."""

    def __init__(self, status_body, exec_sequences, plan_body, zones_body,
                 board_state_for_resume_check="idle"):
        self._status_body = status_body
        self._exec_sequences = list(exec_sequences)  # list of lists, one per entry
        self._plan_body = plan_body
        self._zones_body = zones_body
        self.board_state_for_resume_check = board_state_for_resume_check
        self._resume_check_done = False
        self.started_profile_ids = []
        self.stopped = False
        self._t = 0.0

    def sleep(self, s):
        self._t += s

    def now(self):
        return self._t

    def get_status(self, host, timeout):
        return self._status_body

    def get_exec(self, host, timeout):
        # The very first get_exec call of a resumed run_queue() is
        # check_board_not_running_for_resume's precheck -- serve the fixed
        # board state once, then fall through to the scripted per-entry
        # sequences exactly like the non-resume tests.
        if not self._resume_check_done:
            self._resume_check_done = True
            return _exec([0], state=self.board_state_for_resume_check)
        seq = self._exec_sequences[0]
        val = seq.pop(0) if len(seq) > 1 else seq[0]
        if not seq:
            pass
        if len(self._exec_sequences[0]) == 0:
            pass
        return val

    def advance_entry(self):
        """Call after an entry finishes to move on to the next entry's
        scripted exec sequence."""
        self._exec_sequences.pop(0)

    def get_zones(self, host, timeout):
        return self._zones_body

    def get_profile_plan(self, host, profile_id, timeout):
        return self._plan_body

    def start_profile(self, host, profile_id, timeout):
        self.started_profile_ids.append(profile_id)
        return {"ok": True}

    def stop_profile(self, host, timeout, **kwargs):
        self.stopped = True


def _fake_apply_preset(control, preset, zones_host=None):
    return object()


class _Harness:
    """Runs run_queue() end to end with every HTTP call monkeypatched, and
    a fake apply_preset_fn. Advances transport._exec_sequences to the next
    entry's script after each completed entry by wrapping get_exec so it
    pops the *current* entry's list once that entry's sequence empties.
    Simpler: give each entry a script that is exactly consumed (last
    element repeats), and rely on run_entry() itself only calling get_exec
    the right number of times per entry -- so instead we pre-slice: each
    entry's own script must end in a terminal state (done/faulted) and stay
    there if over-polled, since the transport repeats the last element."""

    def __init__(self, entries, exec_scripts, status_body=None, plan_body=None,
                 zones_body=None, board_state_for_resume_check="idle"):
        self.tmpdir = tempfile.mkdtemp()
        self.entries = entries
        self.transport = _ScriptedTransport(
            status_body=status_body or _status([_channel(25.1, 25.0)]),
            exec_sequences=[list(s) for s in exec_scripts],
            plan_body=plan_body or _plan([20, 45, 60]),
            zones_body=zones_body or _zones([80.0, 80.0, 80.0]),
            board_state_for_resume_check=board_state_for_resume_check,
        )
        self.cfg = rq.RunQueueConfig(
            host="203.0.113.10", poll_interval_s=0.0,
            sleep=self.transport.sleep, now=self.transport.now, cooldown_s=0.0)

    def path(self, name):
        return os.path.join(self.tmpdir, name)

    def run(self, state_path=None, resume=False):
        # Wrap get_exec so it advances to the next entry's script once the
        # *previous* run_entry call has consumed the active one down to its
        # terminal (repeating) tail. We detect "entry boundary" the simple
        # way: run_entry() is called once per entry by run_queue(), so we
        # rotate transport._exec_sequences ourselves via a counting wrapper.
        calls = {"entries_started": 0}
        orig_run_entry = rq.run_entry

        def counting_run_entry(entry, cfg, control=None, apply_preset_fn=None):
            result = orig_run_entry(entry, cfg, control=control, apply_preset_fn=apply_preset_fn)
            calls["entries_started"] += 1
            if self.transport._exec_sequences:
                self.transport._exec_sequences.pop(0)
            return result

        with unittest.mock.patch.object(rq, "get_status", self.transport.get_status), \
             unittest.mock.patch.object(rq, "get_exec", self.transport.get_exec), \
             unittest.mock.patch.object(rq, "get_zones", self.transport.get_zones), \
             unittest.mock.patch.object(rq, "get_profile_plan", self.transport.get_profile_plan), \
             unittest.mock.patch.object(rq, "start_profile", self.transport.start_profile), \
             unittest.mock.patch.object(rq, "stop_profile", self.transport.stop_profile), \
             unittest.mock.patch.object(rq, "run_entry", counting_run_entry):
            rq.run_queue(self.entries, self.cfg, control=None,
                          apply_preset_fn=_fake_apply_preset,
                          state_path=state_path, resume=resume)


def _entry(h: _Harness, name, profile_id=7, label=None):
    return rq.QueueEntry(preset_name={"name": "fake"}, profile_id=profile_id,
                          log_path=h.path(name), label=label or name)


# --------------------------------------------------------------------------
# State file: written and updated across entries.
# --------------------------------------------------------------------------

class StateFileWrittenTest(unittest.TestCase):
    def test_state_file_created_and_marks_every_entry_completed(self):
        h = _Harness.__new__(_Harness)  # placeholder to get tmpdir before entries
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "run1.jsonl")
        e2 = _entry(h, "run2.jsonl")
        h.entries = [e1, e2]
        h.transport._exec_sequences = [
            [_exec([0], state="running"), _exec([0], state="done")],
            [_exec([0], state="running"), _exec([0], state="done")],
        ]
        state_path = h.path("campaign_state.json")

        h.run(state_path=state_path)

        self.assertTrue(os.path.exists(state_path))
        with open(state_path) as fh:
            state = json.load(fh)
        self.assertEqual(len(state["entries"]), 2)
        for se in state["entries"]:
            self.assertEqual(se["status"], "completed")
            self.assertIsNotNone(se["completed_at"])
        self.assertEqual(state["progress"], "2/2 completed")

    def test_state_file_is_human_readable_indented_json(self):
        # requirement 5: readable at a glance. Assert it's multi-line
        # indented JSON, not a single minified blob.
        h = _Harness.__new__(_Harness)
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "run1.jsonl")
        h.entries = [e1]
        h.transport._exec_sequences = [[_exec([0], state="running"), _exec([0], state="done")]]
        state_path = h.path("campaign_state.json")

        h.run(state_path=state_path)

        with open(state_path) as fh:
            raw = fh.read()
        self.assertGreater(raw.count("\n"), 5)
        self.assertIn('"status": "completed"', raw)

    def test_state_file_updated_durably_between_entries_not_only_at_the_end(self):
        # After entry 1 completes but entry 2 is still in flight, the state
        # file on disk (not just in memory) must already reflect entry 1 as
        # completed and entry 2 as in_progress -- prove this by inspecting
        # the file from inside a patched run_entry for entry 2.
        h = _Harness.__new__(_Harness)
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "run1.jsonl")
        e2 = _entry(h, "run2.jsonl")
        h.entries = [e1, e2]
        h.transport._exec_sequences = [
            [_exec([0], state="running"), _exec([0], state="done")],
            [_exec([0], state="running"), _exec([0], state="done")],
        ]
        state_path = h.path("campaign_state.json")

        orig_run_entry = rq.run_entry
        calls = {}

        def wrapper(entry, cfg, control=None, apply_preset_fn=None):
            if entry.log_path == e2.log_path and "state_before_entry2" not in calls:
                with open(state_path) as fh:
                    calls["state_before_entry2"] = json.load(fh)
            result = orig_run_entry(entry, cfg, control=control, apply_preset_fn=apply_preset_fn)
            if h.transport._exec_sequences:
                h.transport._exec_sequences.pop(0)
            return result

        with unittest.mock.patch.object(rq, "get_status", h.transport.get_status), \
             unittest.mock.patch.object(rq, "get_exec", h.transport.get_exec), \
             unittest.mock.patch.object(rq, "get_zones", h.transport.get_zones), \
             unittest.mock.patch.object(rq, "get_profile_plan", h.transport.get_profile_plan), \
             unittest.mock.patch.object(rq, "start_profile", h.transport.start_profile), \
             unittest.mock.patch.object(rq, "stop_profile", h.transport.stop_profile), \
             unittest.mock.patch.object(rq, "run_entry", wrapper):
            rq.run_queue(h.entries, h.cfg, control=None, apply_preset_fn=_fake_apply_preset,
                         state_path=state_path, resume=False)

        before = calls["state_before_entry2"]
        self.assertEqual(before["entries"][0]["status"], "completed")
        self.assertEqual(before["entries"][1]["status"], "in_progress")


# --------------------------------------------------------------------------
# Resume: skip completed, discard-and-rerun in_progress, never overwrite.
# --------------------------------------------------------------------------

class ResumeTest(unittest.TestCase):
    def _write_state(self, path, entries, statuses):
        state = rq.new_campaign_state(entries)
        for se, status in zip(state["entries"], statuses):
            se["status"] = status
            if status == "completed":
                se["completed_at"] = 123.0
        rq.save_campaign_state(path, state)
        return state

    def test_resume_skips_completed_entries(self):
        h = _Harness.__new__(_Harness)
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "run1.jsonl")
        e2 = _entry(h, "run2.jsonl")
        h.entries = [e1, e2]
        state_path = h.path("state.json")

        # e1 already completed with real (non-empty) data on disk; e2 pending.
        with open(e1.log_path, "w") as fh:
            fh.write('{"t": 1}\n')
        self._write_state(state_path, [e1, e2], ["completed", "pending"])

        h.transport.board_state_for_resume_check = "idle"
        h.transport._exec_sequences = [
            [_exec([0], state="running"), _exec([0], state="done")],  # e2 only
        ]

        h.run(state_path=state_path, resume=True)

        # e1 must never have been started again -- its file is untouched.
        with open(e1.log_path) as fh:
            self.assertEqual(fh.read(), '{"t": 1}\n')
        with open(state_path) as fh:
            final_state = json.load(fh)
        self.assertEqual(final_state["entries"][0]["status"], "completed")
        self.assertEqual(final_state["entries"][1]["status"], "completed")

    def test_resume_does_not_overwrite_completed_entrys_file(self):
        # Direct proof the clobber path is exercised: completed entry's
        # capture is non-empty and real, resume must leave the bytes as-is.
        h = _Harness.__new__(_Harness)
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "run1.jsonl")
        h.entries = [e1]
        state_path = h.path("state.json")
        original_bytes = '{"t": 1, "exec": {"state": "done"}}\n' * 50
        with open(e1.log_path, "w") as fh:
            fh.write(original_bytes)
        self._write_state(state_path, [e1], ["completed"])

        h.transport.board_state_for_resume_check = "idle"
        h.transport._exec_sequences = []

        h.run(state_path=state_path, resume=True)

        with open(e1.log_path) as fh:
            self.assertEqual(fh.read(), original_bytes)

    def test_resume_discards_and_reruns_interrupted_in_progress_entry(self):
        h = _Harness.__new__(_Harness)
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "run1.jsonl")
        h.entries = [e1]
        state_path = h.path("state.json")
        # A partial capture left behind by a crash mid-run.
        with open(e1.log_path, "w") as fh:
            fh.write('{"t": 1, "exec": {"state": "running"}}\n')
        self._write_state(state_path, [e1], ["in_progress"])

        h.transport.board_state_for_resume_check = "idle"  # board finished/died too
        h.transport._exec_sequences = [
            [_exec([0], state="running"), _exec([0], state="done")],
        ]

        h.run(state_path=state_path, resume=True)

        # The entry actually re-ran (started_profile_ids non-empty) and its
        # log now holds a fresh, complete capture (ends in "done"), not the
        # old partial content.
        self.assertEqual(h.transport.started_profile_ids, [7])
        with open(e1.log_path) as fh:
            lines = [json.loads(l) for l in fh if l.strip()]
        self.assertEqual(lines[-1]["exec"]["state"], "done")
        with open(state_path) as fh:
            final_state = json.load(fh)
        self.assertEqual(final_state["entries"][0]["status"], "completed")

    def test_resume_does_not_renumber_log_paths(self):
        h = _Harness.__new__(_Harness)
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "noise_floor_p7_run1.jsonl")
        e2 = _entry(h, "noise_floor_p7_run2.jsonl")
        e3 = _entry(h, "noise_floor_p7_run3.jsonl")
        h.entries = [e1, e2, e3]
        state_path = h.path("state.json")
        with open(e1.log_path, "w") as fh:
            fh.write('{"t": 1}\n')
        self._write_state(state_path, [e1, e2, e3], ["completed", "pending", "pending"])

        h.transport.board_state_for_resume_check = "idle"
        h.transport._exec_sequences = [
            [_exec([0], state="running"), _exec([0], state="done")],
            [_exec([0], state="running"), _exec([0], state="done")],
        ]

        h.run(state_path=state_path, resume=True)

        # e2 and e3 must have run at their OWN original log paths -- never
        # renumbered starting from 1 again (that would silently reuse e1's
        # name space or otherwise collide).
        self.assertTrue(os.path.exists(e2.log_path))
        self.assertTrue(os.path.exists(e3.log_path))
        self.assertEqual(h.transport.started_profile_ids, [7, 7])


# --------------------------------------------------------------------------
# Already-running-board refusal on resume.
# --------------------------------------------------------------------------

class BoardAlreadyRunningResumeTest(unittest.TestCase):
    def test_resume_refuses_when_board_is_actively_running(self):
        h = _Harness.__new__(_Harness)
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "run1.jsonl")
        h.entries = [e1]
        state_path = h.path("state.json")
        state = rq.new_campaign_state([e1])
        state["entries"][0]["status"] = "in_progress"
        rq.save_campaign_state(state_path, state)

        h.transport.board_state_for_resume_check = "running"  # still firing!
        h.transport._exec_sequences = [[_exec([0], state="running")]]
        # Belt-and-suspenders against a mutation that removes the refusal:
        # the scripted exec state never reaches a terminal state, so without
        # the refusal run_entry's run-capture poll would spin forever with
        # poll_interval_s=0.0 advancing fake time by zero each iteration.
        # Force fake time to actually advance so a removed refusal fails
        # fast (via the run-timeout RunQueueError) instead of hanging the
        # test process.
        h.cfg.sleep = lambda s: setattr(
            h.transport, "_t", h.transport._t + max(s, 1.0))
        h.cfg.run_timeout_multiplier = 0.0
        h.cfg.run_timeout_margin_s = 2.0

        with self.assertRaises(rq.RunQueueError) as ctx:
            h.run(state_path=state_path, resume=True)
        self.assertIn("already running", str(ctx.exception))
        # Must never have attempted a second start.
        self.assertEqual(h.transport.started_profile_ids, [])
        # And the partial capture must NOT have been touched/discarded --
        # refusing means leaving everything exactly as it was.
        with open(state_path) as fh:
            unchanged = json.load(fh)
        self.assertEqual(unchanged["entries"][0]["status"], "in_progress")

    def test_resume_refuses_when_board_is_paused(self):
        # "paused" is also an active state (_ACTIVE_STATES) -- must refuse
        # too, not just "running".
        h = _Harness.__new__(_Harness)
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "run1.jsonl")
        h.entries = [e1]
        state_path = h.path("state.json")
        state = rq.new_campaign_state([e1])
        rq.save_campaign_state(state_path, state)

        h.transport.board_state_for_resume_check = "paused"
        h.transport._exec_sequences = [[_exec([0], state="running")]]
        h.cfg.sleep = lambda s: setattr(
            h.transport, "_t", h.transport._t + max(s, 1.0))
        h.cfg.run_timeout_multiplier = 0.0
        h.cfg.run_timeout_margin_s = 2.0

        with self.assertRaises(rq.RunQueueError) as ctx:
            h.run(state_path=state_path, resume=True)
        self.assertIn("already running", str(ctx.exception))
        self.assertEqual(h.transport.started_profile_ids, [])


# --------------------------------------------------------------------------
# Clobber refusal, unconditionally (not just under --resume).
# --------------------------------------------------------------------------

class ClobberRefusalTest(unittest.TestCase):
    def test_run_entry_refuses_when_log_path_already_exists_nonempty(self):
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")
        with open(log_path, "w") as fh:
            fh.write('{"t": 1}\n')

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_body=rested,
            exec_sequences=[[_exec([0], state="running"), _exec([0], state="done")]],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        transport._resume_check_done = True

        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7,
                               log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now)

        with unittest.mock.patch.object(rq, "get_status", transport.get_status), \
             unittest.mock.patch.object(rq, "get_exec", transport.get_exec), \
             unittest.mock.patch.object(rq, "get_zones", transport.get_zones), \
             unittest.mock.patch.object(rq, "get_profile_plan", transport.get_profile_plan), \
             unittest.mock.patch.object(rq, "start_profile", transport.start_profile), \
             unittest.mock.patch.object(rq, "stop_profile", transport.stop_profile):
            with self.assertRaises(rq.RunQueueClobberError) as ctx:
                rq.run_entry(entry, cfg, control=None, apply_preset_fn=_fake_apply_preset)
        self.assertIn(repr(log_path), str(ctx.exception))
        # Never started -- refusal happens before the start POST.
        self.assertEqual(transport.started_profile_ids, [])
        # Original bytes must survive -- proof it never opened for write.
        with open(log_path) as fh:
            self.assertEqual(fh.read(), '{"t": 1}\n')

    def test_empty_existing_file_does_not_trigger_clobber_refusal(self):
        # An empty file (e.g. left by some other process, or a filesystem
        # touch) is not "data" -- run_entry should proceed normally.
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")
        open(log_path, "w").close()

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_body=rested,
            exec_sequences=[[_exec([0], state="running"), _exec([0], state="done")]],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        transport._resume_check_done = True  # skip the resume precheck branch
        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7,
                               log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now, cooldown_s=0.0)

        with unittest.mock.patch.object(rq, "get_status", transport.get_status), \
             unittest.mock.patch.object(rq, "get_exec", transport.get_exec), \
             unittest.mock.patch.object(rq, "get_zones", transport.get_zones), \
             unittest.mock.patch.object(rq, "get_profile_plan", transport.get_profile_plan), \
             unittest.mock.patch.object(rq, "start_profile", transport.start_profile), \
             unittest.mock.patch.object(rq, "stop_profile", transport.stop_profile):
            rq.run_entry(entry, cfg, control=None, apply_preset_fn=_fake_apply_preset)  # must not raise
        self.assertEqual(transport.started_profile_ids, [7])

    def test_fresh_campaign_refuses_to_reuse_an_existing_state_file_path(self):
        h = _Harness.__new__(_Harness)
        h.__init__(entries=[], exec_scripts=[])
        e1 = _entry(h, "run1.jsonl")
        h.entries = [e1]
        state_path = h.path("state.json")
        with open(state_path, "w") as fh:
            fh.write('{"already": "here"}')

        h.transport._exec_sequences = [[_exec([0], state="running"), _exec([0], state="done")]]

        with self.assertRaises(rq.RunQueueError) as ctx:
            h.run(state_path=state_path, resume=False)
        self.assertIn("already exists", str(ctx.exception))
        self.assertIn("--resume", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
