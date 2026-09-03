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
        # residual heat, not self-heating noise. 25.6 vs 25.0 is +0.6C over
        # a 0.5C tolerance -- it discriminates the one-sided check
        # (25.6 - 25.0 = 0.6 > 0.5 -> refuses) from the old two-sided
        # abs() check (|25.6 - 25.0| = 0.6 > 0.5 -> also refuses, same
        # result -- so this value alone isn't the point); paired with
        # test_below_cold_junction_from_self_heating_is_rested it proves
        # the two forms disagree on this module's actual behavior only in
        # the below-CJ direction. The prior fixture here (30.0 vs 25.0)
        # returned False under BOTH forms and proved nothing was fixed.
        st = _status([_channel(26.91, 28.22), _channel(25.6, 25.0)])
        self.assertFalse(rq.is_rested(st, tol_c=0.5))


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

    def stop_profile(self, host, timeout, **kwargs):
        self.stopped = True


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
         unittest.mock.patch.object(rq, "start_profile", transport.start_profile), \
         unittest.mock.patch.object(rq, "stop_profile", transport.stop_profile):
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
        return {"name": "fake_coupling_only", "ramp_assist_enabled": False, "zones": [zone]}

    def test_coupling_only_preset_applies_over_http_with_no_control(self):
        preset = self._preset()
        with unittest.mock.patch.object(
                rq.zones_http_client, "apply_zone_preset",
                return_value=rq.zones_http_client.ZonesApplyResult(ok=True)) as mock_apply, \
             unittest.mock.patch.object(
                 rq.ramp_assist_http_client, "set_enabled",
                 return_value={"ok": True, "enabled": False}) as mock_ra:
            result = rq._apply_preset_http_only(None, preset, zones_host="203.0.113.10")
        self.assertTrue(result.ok)
        mock_apply.assert_called_once()
        self.assertEqual(mock_apply.call_args.args[0], "203.0.113.10")
        # ramp_assist_enabled must be pinned too -- the preset's own value
        # (False here), not left to whatever the board already has.
        mock_ra.assert_called_once_with("203.0.113.10", False, timeout=unittest.mock.ANY)

    def test_ramp_assist_pin_failure_raises(self):
        """NEGATIVE TEST: the zones write can succeed while pinning
        ramp_assist_enabled fails -- this must raise, not report success,
        since starting a firing without the flag actually pinned is exactly
        the silent-invalidation hazard this field exists to prevent."""
        preset = self._preset()
        with unittest.mock.patch.object(
                rq.zones_http_client, "apply_zone_preset",
                return_value=rq.zones_http_client.ZonesApplyResult(ok=True)), \
             unittest.mock.patch.object(
                 rq.ramp_assist_http_client, "set_enabled",
                 return_value={"ok": False, "error": "ESP_FAIL"}):
            with self.assertRaises(rq.RunQueueError):
                rq._apply_preset_http_only(None, preset, zones_host="203.0.113.10")

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
             unittest.mock.patch.object(
                 rq.ramp_assist_http_client, "set_enabled",
                 return_value={"ok": True, "enabled": False}), \
             unittest.mock.patch.object(rq, "get_status", transport.get_status), \
             unittest.mock.patch.object(rq, "get_exec", transport.get_exec), \
             unittest.mock.patch.object(rq, "get_zones", transport.get_zones), \
             unittest.mock.patch.object(rq, "get_profile_plan", transport.get_profile_plan), \
             unittest.mock.patch.object(rq, "start_profile", transport.start_profile), \
             unittest.mock.patch.object(rq, "stop_profile", transport.stop_profile):
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

class OpenCaptureBeforeStartTest(unittest.TestCase):
    """Regression coverage for the live incident: run_entry used to POST
    /api/profile_exec/start, confirm the executor left idle, and only THEN
    open the capture file -- so a bad log_path raised FileNotFoundError
    with the heaters already energized and nothing capturing or
    supervising the run. run_entry must open (and validate) the capture
    file BEFORE the start POST, and must never issue that POST at all if
    the file cannot be opened."""

    def test_unwritable_capture_dir_refuses_before_any_start_post(self):
        # A parent directory that does not exist -- exactly tonight's
        # FileNotFoundError shape ('../../logs/coupling/...').
        tmpdir = tempfile.mkdtemp()
        bad_log_path = os.path.join(tmpdir, "does_not_exist_subdir", "run.jsonl")

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=[_exec([0], state="running"), _exec([0], state="done")],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7,
                               log_path=bad_log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now, cooldown_s=0.0)

        with self.assertRaises(rq.RunQueueError):
            _patched_run_entry(entry, cfg, transport, entry.preset_name)

        # The whole point: the kiln must never have been told to start.
        self.assertIsNone(transport.started_profile_id)
        self.assertFalse(os.path.exists(bad_log_path))

    def test_refused_start_leaves_no_stray_empty_capture_file(self):
        # The capture file CAN be opened, but the start POST itself is
        # refused (ok: False) -- must not leave a stray empty file behind.
        #
        # The original version of this test only asserted the file was
        # absent AFTER run_entry raised -- which trivially passed even
        # against the pre-fix code, where the start POST happened BEFORE
        # open(), so a refusal meant open() was never reached and no file
        # was ever created. That proves nothing about the cleanup path
        # itself. To actually exercise "open before start, then clean up
        # after a refusal", this test now asserts the file DOES exist at
        # the moment the (refused) start POST fires -- proof open() ran
        # first, per the reordering -- and only THEN that it is gone
        # afterwards, proving the finally-block cleanup, not just an
        # accident of ordering, removed it.
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=[_exec([0], state="running")],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )

        observed = {"existed_at_start_call": None}

        def refusing_start(host, profile_id, timeout):
            observed["existed_at_start_call"] = os.path.exists(log_path)
            return {"ok": False, "error": "refused"}
        transport.start_profile = refusing_start

        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7,
                               log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now, cooldown_s=0.0)

        with self.assertRaises(rq.RunQueueError):
            _patched_run_entry(entry, cfg, transport, entry.preset_name)

        # The capture file must have existed WHEN the start POST fired
        # (proving open() really ran first, the reordering this test is
        # named for) -- and be gone now (proving the refusal cleanup path,
        # not merely "never created", removed it).
        self.assertTrue(
            observed["existed_at_start_call"],
            "capture file did not exist yet when the start POST fired -- open() did not "
            "run before start_profile()")
        self.assertFalse(os.path.exists(log_path))


class StopOnCrashAfterStartTest(unittest.TestCase):
    """If the start POST succeeds but something later in run_entry raises
    before/while capturing, the kiln must not be left orphaned running --
    run_entry must POST /api/profile_exec/stop before the original
    exception propagates."""

    def test_exception_after_successful_start_stops_the_profile(self):
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=[_exec([0], state="running")],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7,
                               log_path=log_path, label="t")
        # start_confirm_timeout_s=0 with a state that never leaves 'running'
        # (never idle, never terminal) still passes _wait_until_run_active_
        # or_terminal immediately since 'running' is itself an active state
        # -- so force the failure inside the run-capture poll instead by
        # making the timeout computation raise: total_planned_s is missing.
        transport._plan_body = {"points": [{"t": 0, "c": 60}]}  # no total_planned_s

        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now, cooldown_s=0.0)

        with self.assertRaises(rq.RunQueueError) as ctx:
            _patched_run_entry(entry, cfg, transport, entry.preset_name)

        # It must be the ORIGINAL exception (missing total_planned_s), not
        # swallowed or replaced by the stop-profile bookkeeping.
        self.assertIn("total_planned_s", str(ctx.exception))
        self.assertEqual(transport.started_profile_id, 7)
        self.assertTrue(transport.stopped, "stop_profile was never called after the crash")

    def test_start_post_raises_after_board_accepted_still_stops_and_keeps_capture(self):
        # DEFECT 1: the board can ACCEPT the start POST (heaters energize)
        # while the client sees a URLError/HTTPError/timeout raised OUT of
        # start_profile() itself -- before it ever returns a body to
        # inspect. `started` must be set True BEFORE the POST is attempted,
        # so this path still triggers stop_profile(), and the capture file
        # (which was already open) must NOT be deleted -- the run may be
        # live and uncaptured is worse than a non-empty file on disk.
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=[_exec([0], state="running")],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )

        def raising_start(host, profile_id, timeout):
            # Simulates _post_form's own behavior on a socket timeout: it
            # raises RunQueueError, never returns a body at all.
            raise rq.RunQueueError("POST /api/profile_exec/start failed: "
                                    "<urlopen error timed out>")
        transport.start_profile = raising_start

        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7,
                               log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now, cooldown_s=0.0)

        with self.assertRaises(rq.RunQueueError) as ctx:
            _patched_run_entry(entry, cfg, transport, entry.preset_name)

        self.assertIn("timed out", str(ctx.exception))
        self.assertTrue(transport.stopped,
                         "a start POST that raised after the board may have accepted it "
                         "must still trigger stop_profile()")
        # The capture file must survive -- the run may be live with nobody
        # watching it, which is strictly worse than a stray file on disk.
        self.assertTrue(os.path.exists(log_path),
                         "capture file was deleted even though the board may have "
                         "started firing")


class StopProfileVerifiesRealStopTest(unittest.TestCase):
    """DEFECT 2: stop_profile() used to POST /api/profile_exec/stop and
    discard the response body, so an HTTP 200 carrying {"ok": false} (or a
    board that acked the POST but never actually left RUNNING/PAUSED) was
    indistinguishable from a real stop. stop_profile() must now re-poll
    GET /api/profile_exec afterwards and raise if the state never clears,
    or if the stop POST body itself says {"ok": false}."""

    def test_explicit_ok_false_body_is_not_reported_as_success(self):
        stop_body = json.dumps({"ok": False, "error": "already idle"}).encode()

        def fake_post_form(host, path, fields, timeout):
            self.assertEqual(path, "/api/profile_exec/stop")
            return stop_body.decode()

        with unittest.mock.patch.object(rq, "_post_form", fake_post_form), \
             unittest.mock.patch.object(rq, "_get_json") as mock_get:
            with self.assertRaises(rq.RunQueueError) as ctx:
                rq.stop_profile("203.0.113.10")
        self.assertIn("refused", str(ctx.exception).lower())
        # Must never even get to polling for clearance -- the board already
        # told us the stop did not happen.
        mock_get.assert_not_called()

    def test_state_that_never_clears_is_escalated(self):
        # The stop POST itself acks cleanly ({"ok": true}), but every
        # subsequent GET /api/profile_exec still reports 'running' -- the
        # board never actually stopped. Must raise, bounded by
        # confirm_timeout_s, never hang or silently return as if stopped.
        ok_body = json.dumps({"ok": True}).encode().decode()
        still_running = {"state": "running", "zones": []}

        sleeps = []

        def fake_sleep(s):
            sleeps.append(s)

        t = {"now": 0.0}

        def fake_now():
            return t["now"]

        def fake_post_form(host, path, fields, timeout):
            return ok_body

        def fake_get_json(host, path, timeout):
            t["now"] += 1.0
            return still_running

        with unittest.mock.patch.object(rq, "_post_form", fake_post_form), \
             unittest.mock.patch.object(rq, "_get_json", fake_get_json):
            with self.assertRaises(rq.RunQueueError) as ctx:
                rq.stop_profile("203.0.113.10", confirm_timeout_s=3.0,
                                 poll_interval_s=0.01, sleep=fake_sleep, now=fake_now)
        self.assertIn("may still be firing", str(ctx.exception))

    def test_state_that_clears_promptly_succeeds(self):
        ok_body = json.dumps({"ok": True}).encode().decode()
        states = [{"state": "running", "zones": []}, {"state": "done", "zones": []}]

        def fake_post_form(host, path, fields, timeout):
            return ok_body

        def fake_get_json(host, path, timeout):
            return states.pop(0) if len(states) > 1 else states[0]

        with unittest.mock.patch.object(rq, "_post_form", fake_post_form), \
             unittest.mock.patch.object(rq, "_get_json", fake_get_json):
            rq.stop_profile("203.0.113.10", confirm_timeout_s=5.0, poll_interval_s=0.0,
                             sleep=lambda s: None, now=lambda: 0.0)  # must not raise


class FailedStopEscalationTest(unittest.TestCase):
    """DEFECT 3: when the stop-on-exception path's own stop_profile() call
    also fails, the exception that reaches main() must be unmissable, not
    just the original (unrelated) error with a stop failure buried in a log
    line above it. run_entry must raise a distinct RunQueueStopFailedError,
    and main() must report it with a distinct exit code."""

    def test_run_entry_raises_distinct_error_when_stop_also_fails(self):
        tmpdir = tempfile.mkdtemp()
        log_path = os.path.join(tmpdir, "run.jsonl")

        rested = _status([_channel(25.1, 25.0)])
        transport = _ScriptedTransport(
            status_sequence=[rested],
            exec_sequence=[_exec([0], state="running")],
            plan_body=_plan([20, 45, 60]),
            zones_body=_zones([80.0, 80.0, 80.0]),
        )
        transport._plan_body = {"points": [{"t": 0, "c": 60}]}  # no total_planned_s -> raises

        def failing_stop(host, timeout, **kwargs):
            raise rq.RunQueueError("POST /api/profile_exec/stop failed: connection refused")
        transport.stop_profile = failing_stop

        entry = rq.QueueEntry(preset_name={"name": "fake"}, profile_id=7,
                               log_path=log_path, label="t")
        cfg = rq.RunQueueConfig(host="203.0.113.10", poll_interval_s=0.0,
                                 sleep=transport.sleep, now=transport.now, cooldown_s=0.0)

        with self.assertRaises(rq.RunQueueStopFailedError) as ctx:
            _patched_run_entry(entry, cfg, transport, entry.preset_name)
        message = str(ctx.exception)
        self.assertIn("STILL BE FIRING", message)
        self.assertIn("total_planned_s", message)  # original error preserved too

    def test_main_reports_distinct_exit_code_on_failed_stop(self):
        def boom_run_queue(entries, cfg, control=None, apply_preset_fn=None):
            raise rq.RunQueueStopFailedError("[t] the kiln may STILL BE FIRING: stop failed")

        with unittest.mock.patch.object(rq, "run_queue", boom_run_queue):
            rc = rq.main(["--host", "203.0.113.10", "--run", "p:7:log.jsonl"])
        self.assertEqual(rc, 2)

    def test_main_reports_exit_code_1_for_an_ordinary_refusal(self):
        def boom_run_queue(entries, cfg, control=None, apply_preset_fn=None):
            raise rq.RunQueueError("zones did not settle")

        with unittest.mock.patch.object(rq, "run_queue", boom_run_queue):
            rc = rq.main(["--host", "203.0.113.10", "--run", "p:7:log.jsonl"])
        self.assertEqual(rc, 1)


class ParseRunArgWindowsPathTest(unittest.TestCase):
    """Regression coverage for the live incident: --run PRESET:PROFILE_ID:
    LOG_PATH[:LABEL] split naively on ':' treats a Windows drive letter
    (C:/Users/...) as a field separator, silently truncating LOG_PATH to
    'C' and shoving the real path + label together into LABEL. It must
    either parse the absolute path correctly or raise a clear error --
    never silently produce a mangled log path."""

    def test_windows_absolute_path_parses_log_path_correctly(self):
        raw = "coupling_matrix_pre20260902:7:C:/Users/budarriola/logs/noise_floor_p7b.jsonl:noise_floor"
        entry = rq._parse_run_arg(raw)
        self.assertEqual(entry.preset_name, "coupling_matrix_pre20260902")
        self.assertEqual(entry.profile_id, 7)
        self.assertEqual(entry.log_path, "C:/Users/budarriola/logs/noise_floor_p7b.jsonl")
        self.assertEqual(entry.label, "noise_floor")

    def test_windows_absolute_path_no_label_parses_log_path_correctly(self):
        raw = "p:3:C:/Users/budarriola/logs/run.jsonl"
        entry = rq._parse_run_arg(raw)
        self.assertEqual(entry.log_path, "C:/Users/budarriola/logs/run.jsonl")

    def test_windows_backslash_absolute_path_parses_log_path_correctly(self):
        raw = r"p:3:C:\Users\budarriola\logs\run.jsonl"
        entry = rq._parse_run_arg(raw)
        self.assertEqual(entry.log_path, r"C:\Users\budarriola\logs\run.jsonl")

    def test_log_path_never_silently_truncated_to_bare_drive_letter(self):
        # The exact live failure mode: log_path must never come out as just
        # "C" with the real path shoved into label.
        raw = "coupling_matrix_pre20260902:7:C:/Users/budarriola/logs/noise_floor_p7b.jsonl:noise_floor"
        entry = rq._parse_run_arg(raw)
        self.assertNotEqual(entry.log_path, "C")
        self.assertNotIn("noise_floor_p7b.jsonl", entry.label)

    def test_drive_relative_path_no_slash_is_refused(self):
        # DEFECT 5: "C:run.jsonl" (relative to whatever the process's cwd on
        # drive C: happens to be, per Windows path semantics) has no slash
        # after the colon, so it slips past the absolute-path re-merge
        # (which only fires when the next field starts with / or \\) and
        # int(profile_id) never sees it either -- it would silently parse as
        # log_path="C", the very misparse the earlier fix targeted. This
        # must be refused loudly, never silently mangled.
        with self.assertRaises(ValueError):
            rq._parse_run_arg("coupling_matrix_pre20260902:7:C:run.jsonl:noise_floor")

    def test_drive_relative_path_with_subdir_no_leading_slash_is_refused(self):
        # "C:logs\\run.jsonl" -- same drive-relative hazard, with a
        # subdirectory, still no leading slash/backslash right after ':'.
        with self.assertRaises(ValueError):
            rq._parse_run_arg(r"coupling_matrix_pre20260902:7:C:logs\run.jsonl:noise_floor")

    def test_bare_drive_letter_log_path_is_refused(self):
        # A LOG_PATH that parses down to a single letter under any path
        # through this function must be refused outright -- it is always
        # the misparse signature, never a legitimate path.
        with self.assertRaises(ValueError):
            rq._parse_run_arg("preset:7:C")


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
