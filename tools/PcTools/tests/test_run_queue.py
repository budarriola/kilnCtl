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


def _plan(points_c, total_planned_s=3000.0):
    return {"points": [{"t": i * 10, "c": c} for i, c in enumerate(points_c)],
            "total_planned_s": total_planned_s}


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

    def test_below_cold_junction_from_self_heating_is_rested(self):
        # Live reading with the kiln genuinely cold: the MAX31856's
        # on-board cold-junction sensor self-heats, so all three zones read
        # BELOW their own cold junction by 1.31-1.99C. A two-sided
        # abs(temp_c - cj_c) > 1.0C tolerance fails this forever -- the
        # one-sided check must treat it as rested.
        st = _status([
            _channel(26.91, 28.22), _channel(26.78, 28.39), _channel(26.63, 28.62),
        ])
        self.assertTrue(rq.is_rested(st, tol_c=1.0))

    def test_above_cold_junction_over_tolerance_is_not_rested(self):
        # The other direction still must refuse: a channel genuinely
        # HOTTER than its own cold junction by more than tol_c is real
        # residual heat, not self-heating noise.
        st = _status([_channel(26.91, 28.22), _channel(30.0, 25.0)])
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


class RunCompletionSeamTest(unittest.TestCase):
    """Regression coverage for the live noise-floor-campaign defect: run1
    "finished" in 5.5s against a ~50-minute profile because the poll loop
    treated a single sample -- including a still-idle one -- as proof the
    run was over. These tests drive a realistic multi-poll state sequence
    (never a fixture that hands the loop 'done' on its very first read) and
    prove the loop neither exits early nor hangs, plus the timeout and
    global-fault paths that fixtures alone never exercised."""

    def _run(self, exec_sequence, cfg_overrides=None, plan_total_planned_s=3000.0):
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")
        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=exec_sequence,
            plan_body=_plan([20, 45, 60], total_planned_s=plan_total_planned_s),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7, log_path=log_path, label="t")
        cfg_kwargs = dict(host="203.0.113.10", poll_interval_s=0.0,
                           sleep=transport.sleep, now=transport.now, cooldown_s=0.0)
        cfg_kwargs.update(cfg_overrides or {})
        cfg = rq.RunQueueConfig(**cfg_kwargs)
        calls = _patched_run_entry(entry, cfg, transport, entry.preset_name)
        with open(log_path) as fh:
            lines = [json.loads(line) for line in fh if line.strip()]
        return transport, calls, lines

    def test_idle_then_running_then_done_completes_without_exiting_early(self):
        # Two idle samples (the executor has not flipped state yet) before
        # it goes active, then several running polls, then done. Must NOT
        # be treated as finished at the first (idle) sample.
        transport, _calls, lines = self._run(
            exec_sequence=[_exec([0], state="idle"), _exec([0], state="idle"),
                            _exec([0], state="running"), _exec([0], state="running"),
                            _exec([0], state="done")])
        self.assertEqual(transport.started_profile_id, 7)
        # The captured log starts once the run is confirmed active -- idle
        # samples are consumed by the start-confirmation step, not written
        # to the run capture file.
        self.assertTrue(lines)
        for line in lines:
            self.assertNotEqual(line["exec"]["state"], "idle")
        self.assertEqual(lines[-1]["exec"]["state"], "done")

    def test_realistic_multi_poll_running_sequence_captures_every_poll(self):
        # A run that stays 'running' across several polls before 'done' must
        # produce more than one capture line -- proof the loop is actually
        # polling repeatedly, not exiting on the first sample.
        transport, _calls, lines = self._run(
            exec_sequence=[_exec([0], state="running")] * 4 + [_exec([0], state="done")])
        self.assertGreaterEqual(len(lines), 4)
        self.assertEqual(lines[-1]["exec"]["state"], "done")

    def test_stuck_idle_after_start_raises_instead_of_exiting_early(self):
        # The executor never leaves idle at all (e.g. the start silently
        # no-op'd on the board) -- must raise, not silently proceed as if
        # a 0-length run had completed.
        with self.assertRaises(rq.RunQueueError):
            self._run(exec_sequence=[_exec([0], state="idle")],
                       cfg_overrides={"start_confirm_timeout_s": 0.0, "poll_interval_s": 0.01})

    def test_run_stays_running_past_timeout_raises(self):
        # Never reaches a terminal state -- must raise once the computed
        # run timeout elapses, rather than hanging or silently returning.
        with self.assertRaises(rq.RunQueueError):
            self._run(
                exec_sequence=[_exec([0], state="running")],
                cfg_overrides={"run_timeout_multiplier": 0.0, "run_timeout_margin_s": 0.0,
                                "poll_interval_s": 0.01},
                plan_total_planned_s=0.0)

    def test_global_faulted_state_stops_queue_even_with_no_zone_fault_guard(self):
        # profile_executor.h PROFILE_EXEC_FAULTED covers a GLOBAL guard trip
        # too, which need not set any single zone's fault_guard --
        # check_no_fault alone would miss this.
        with self.assertRaises(rq.RunQueueFaultError):
            self._run(exec_sequence=[_exec([0], state="running"), _exec([0], state="faulted")])


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


class PrestartRestedRetryTest(unittest.TestCase):
    """Regression coverage for the live failed-campaign defect: a single
    entry's marginal drift between wait_until_rested() and the start POST
    used to raise RunQueueError straight out of run_entry, aborting every
    remaining queued run. It must instead retry wait_until_rested() a
    bounded number of times before giving up."""

    def _run(self, status_sequence, rested_timeout_s=5.0):
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")
        transport = _ScriptedTransport(
            status_sequence=status_sequence,
            exec_sequence=[_exec([0], state="running"), _exec([0], state="done")],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7, log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now,
                                 cooldown_s=0.0, rested_timeout_s=rested_timeout_s)
        calls = _patched_run_entry(entry, cfg, transport, entry.preset_name)
        return transport, calls

    def test_drift_before_start_retries_and_proceeds(self):
        rested = _status([_channel(25.1, 25.0)])
        warm = _status([_channel(35.0, 25.0)])
        # 1: wait_until_rested's own poll (rested) -> returns.
        # 2: pre-start re-check reads drifted/warm -> attempt 1 fails.
        # 3: the retry's wait_until_rested poll (rested) -> returns.
        # 4: the retry's re-check reads rested -> proceeds to start.
        transport, _calls = self._run(status_sequence=[rested, warm, rested, rested])
        self.assertEqual(transport.started_profile_id, 7)

    def test_drift_before_start_raises_after_attempts_exhausted(self):
        rested = _status([_channel(25.1, 25.0)])
        warm = _status([_channel(35.0, 25.0)])
        # Every wait_until_rested poll sees a momentary rested reading (so
        # it returns instead of timing out on its own), but every pre-start
        # re-check that follows sees drifted/warm again -- proves this is
        # the bounded-retries-exhausted path, not wait_until_rested's own
        # timeout.
        with self.assertRaises(rq.RunQueueError) as ctx:
            self._run(status_sequence=[rested, warm, rested, warm, rested, warm])
        self.assertIn("did not re-settle", str(ctx.exception))


# --------------------------------------------------------------------------
# --repeat / expand_repeat -- the noise-floor campaign's turn-key form
# (PID_EXPANSION_PLAN.md SS3.3): N repeats of one entry, each with its own
# log file so no two runs can ever land in one file.
# --------------------------------------------------------------------------

class ExpandRepeatTest(unittest.TestCase):
    def test_expand_repeat_produces_n_entries_same_preset_and_profile(self):
        entry = rq.QueueEntry(preset_name="p7_floor", profile_id=7,
                               log_path="logs/coupling/noise_floor_p7.jsonl", label="floor")
        entries = rq.expand_repeat(entry, 5)
        self.assertEqual(len(entries), 5)
        for e in entries:
            self.assertEqual(e.preset_name, "p7_floor")
            self.assertEqual(e.profile_id, 7)

    def test_expand_repeat_gives_every_entry_a_distinct_log_path(self):
        entry = rq.QueueEntry(preset_name="p7_floor", profile_id=7,
                               log_path="logs/coupling/noise_floor_p7.jsonl", label="floor")
        entries = rq.expand_repeat(entry, 5)
        paths = [e.log_path for e in entries]
        # Distinctness is the whole point: two repeats sharing a file is
        # exactly the near-miss log_analysis.MultiRunError exists to catch
        # after the fact -- expand_repeat must prevent it up front instead.
        self.assertEqual(len(paths), len(set(paths)))

    def test_expand_repeat_log_path_naming(self):
        entry = rq.QueueEntry(preset_name="p7_floor", profile_id=7,
                               log_path="logs/coupling/noise_floor_p7.jsonl", label="floor")
        entries = rq.expand_repeat(entry, 3)
        self.assertEqual(entries[0].log_path, "logs/coupling/noise_floor_p7_run1.jsonl")
        self.assertEqual(entries[1].log_path, "logs/coupling/noise_floor_p7_run2.jsonl")
        self.assertEqual(entries[2].log_path, "logs/coupling/noise_floor_p7_run3.jsonl")

    def test_expand_repeat_labels_identify_the_run_number(self):
        entry = rq.QueueEntry(preset_name="p7_floor", profile_id=7,
                               log_path="x.jsonl", label="floor")
        entries = rq.expand_repeat(entry, 2)
        self.assertIn("run1/2", entries[0].label)
        self.assertIn("run2/2", entries[1].label)

    def test_expand_repeat_rejects_n_below_one(self):
        entry = rq.QueueEntry(preset_name="p7_floor", profile_id=7, log_path="x.jsonl")
        with self.assertRaises(ValueError):
            rq.expand_repeat(entry, 0)

    def test_main_repeat_requires_exactly_one_run(self):
        with self.assertRaises(SystemExit):
            rq.main([
                "--host", "203.0.113.10",
                "--run", "p7_floor:7:a.jsonl",
                "--run", "p7_floor:7:b.jsonl",
                "--repeat", "3",
            ])


if __name__ == "__main__":
    unittest.main()
